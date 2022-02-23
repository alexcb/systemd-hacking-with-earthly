/* SPDX-License-Identifier: LGPL-2.1-or-later */
/***
  Copyright © 2014-2015 Intel Corporation. All rights reserved.
***/

#include <errno.h>
#include <sys/ioctl.h>
#include <linux/if_arp.h>
#include <linux/if_infiniband.h>

#include "sd-dhcp6-client.h"

#include "alloc-util.h"
#include "dhcp-identifier.h"
#include "dhcp6-internal.h"
#include "dhcp6-lease-internal.h"
#include "dns-domain.h"
#include "event-util.h"
#include "fd-util.h"
#include "hexdecoct.h"
#include "hostname-util.h"
#include "in-addr-util.h"
#include "io-util.h"
#include "random-util.h"
#include "socket-util.h"
#include "strv.h"
#include "web-util.h"

static const uint16_t default_req_opts[] = {
        SD_DHCP6_OPTION_DNS_SERVERS,
        SD_DHCP6_OPTION_DOMAIN_LIST,
        SD_DHCP6_OPTION_NTP_SERVER,
        SD_DHCP6_OPTION_SNTP_SERVERS,
};

#define DHCP6_CLIENT_DONT_DESTROY(client) \
        _cleanup_(sd_dhcp6_client_unrefp) _unused_ sd_dhcp6_client *_dont_destroy_##client = sd_dhcp6_client_ref(client)

static int client_start_transaction(sd_dhcp6_client *client, DHCP6State state);

int sd_dhcp6_client_set_callback(
                sd_dhcp6_client *client,
                sd_dhcp6_client_callback_t cb,
                void *userdata) {

        assert_return(client, -EINVAL);

        client->callback = cb;
        client->userdata = userdata;

        return 0;
}

int sd_dhcp6_client_set_ifindex(sd_dhcp6_client *client, int ifindex) {
        assert_return(client, -EINVAL);
        assert_return(!sd_dhcp6_client_is_running(client), -EBUSY);
        assert_return(ifindex > 0, -EINVAL);

        client->ifindex = ifindex;
        return 0;
}

int sd_dhcp6_client_set_ifname(sd_dhcp6_client *client, const char *ifname) {
        assert_return(client, -EINVAL);
        assert_return(ifname, -EINVAL);

        if (!ifname_valid_full(ifname, IFNAME_VALID_ALTERNATIVE))
                return -EINVAL;

        return free_and_strdup(&client->ifname, ifname);
}

int sd_dhcp6_client_get_ifname(sd_dhcp6_client *client, const char **ret) {
        int r;

        assert_return(client, -EINVAL);

        r = get_ifname(client->ifindex, &client->ifname);
        if (r < 0)
                return r;

        if (ret)
                *ret = client->ifname;

        return 0;
}

int sd_dhcp6_client_set_local_address(
                sd_dhcp6_client *client,
                const struct in6_addr *local_address) {

        assert_return(client, -EINVAL);
        assert_return(!sd_dhcp6_client_is_running(client), -EBUSY);
        assert_return(local_address, -EINVAL);
        assert_return(in6_addr_is_link_local(local_address) > 0, -EINVAL);

        client->local_address = *local_address;

        return 0;
}

int sd_dhcp6_client_set_mac(
                sd_dhcp6_client *client,
                const uint8_t *addr,
                size_t addr_len,
                uint16_t arp_type) {

        assert_return(client, -EINVAL);
        assert_return(addr, -EINVAL);
        assert_return(addr_len <= sizeof(client->hw_addr.bytes), -EINVAL);

        /* Unlike the other setters, it is OK to set a new MAC address while the client is running,
         * as the MAC address is used only when setting DUID or IAID. */

        if (arp_type == ARPHRD_ETHER)
                assert_return(addr_len == ETH_ALEN, -EINVAL);
        else if (arp_type == ARPHRD_INFINIBAND)
                assert_return(addr_len == INFINIBAND_ALEN, -EINVAL);
        else {
                client->arp_type = ARPHRD_NONE;
                client->hw_addr.length = 0;
                return 0;
        }

        memcpy(client->hw_addr.bytes, addr, addr_len);
        client->hw_addr.length = addr_len;
        client->arp_type = arp_type;

        return 0;
}

int sd_dhcp6_client_set_prefix_delegation_hint(
                sd_dhcp6_client *client,
                uint8_t prefixlen,
                const struct in6_addr *pd_prefix) {

        _cleanup_free_ DHCP6Address *prefix = NULL;

        assert_return(client, -EINVAL);
        assert_return(!sd_dhcp6_client_is_running(client), -EBUSY);

        if (!pd_prefix) {
                /* clear previous assignments. */
                dhcp6_ia_clear_addresses(&client->ia_pd);
                return 0;
        }

        assert_return(prefixlen > 0 && prefixlen <= 128, -EINVAL);

        prefix = new(DHCP6Address, 1);
        if (!prefix)
                return -ENOMEM;

        *prefix = (DHCP6Address) {
                .iapdprefix.address = *pd_prefix,
                .iapdprefix.prefixlen = prefixlen,
        };

        LIST_PREPEND(addresses, client->ia_pd.addresses, TAKE_PTR(prefix));
        return 1;
}

int sd_dhcp6_client_add_vendor_option(sd_dhcp6_client *client, sd_dhcp6_option *v) {
        int r;

        assert_return(client, -EINVAL);
        assert_return(!sd_dhcp6_client_is_running(client), -EBUSY);

        if (!v) {
                /* Clear the previous assignments. */
                ordered_set_clear(client->vendor_options);
                return 0;
        }

        r = ordered_set_ensure_put(&client->vendor_options, &dhcp6_option_hash_ops, v);
        if (r < 0)
                return r;

        sd_dhcp6_option_ref(v);

        return 1;
}

static int client_ensure_duid(sd_dhcp6_client *client) {
        assert(client);

        if (client->duid_len != 0)
                return 0;

        return dhcp_identifier_set_duid_en(client->test_mode, &client->duid, &client->duid_len);
}

/**
 * Sets DUID. If duid is non-null, the DUID is set to duid_type + duid
 * without further modification. Otherwise, if duid_type is supported, DUID
 * is set based on that type. Otherwise, an error is returned.
 */
static int dhcp6_client_set_duid_internal(
                sd_dhcp6_client *client,
                DUIDType duid_type,
                const void *duid,
                size_t duid_len,
                usec_t llt_time) {
        int r;

        assert_return(client, -EINVAL);
        assert_return(!sd_dhcp6_client_is_running(client), -EBUSY);
        assert_return(duid_len == 0 || duid, -EINVAL);

        if (duid) {
                r = dhcp_validate_duid_len(duid_type, duid_len, true);
                if (r < 0) {
                        r = dhcp_validate_duid_len(duid_type, duid_len, false);
                        if (r < 0)
                                return log_dhcp6_client_errno(client, r, "Failed to validate length of DUID: %m");

                        log_dhcp6_client(client, "Using DUID of type %u of incorrect length, proceeding.", duid_type);
                }

                client->duid.type = htobe16(duid_type);
                memcpy(&client->duid.raw.data, duid, duid_len);
                client->duid_len = sizeof(client->duid.type) + duid_len;

        } else {
                r = dhcp_identifier_set_duid(duid_type, client->hw_addr.bytes, client->hw_addr.length,
                                             client->arp_type, llt_time, client->test_mode, &client->duid, &client->duid_len);
                if (r == -EOPNOTSUPP)
                        return log_dhcp6_client_errno(client, r,
                                                      "Failed to set %s. MAC address is not set or "
                                                      "interface type is not supported.",
                                                      duid_type_to_string(duid_type));
                if (r < 0)
                        return log_dhcp6_client_errno(client, r, "Failed to set %s: %m",
                                                      duid_type_to_string(duid_type));
        }

        return 0;
}

int sd_dhcp6_client_set_duid(
                sd_dhcp6_client *client,
                uint16_t duid_type,
                const void *duid,
                size_t duid_len) {
        return dhcp6_client_set_duid_internal(client, duid_type, duid, duid_len, 0);
}

int sd_dhcp6_client_set_duid_llt(
                sd_dhcp6_client *client,
                usec_t llt_time) {
        return dhcp6_client_set_duid_internal(client, DUID_TYPE_LLT, NULL, 0, llt_time);
}

int sd_dhcp6_client_duid_as_string(
                sd_dhcp6_client *client,
                char **duid) {
        _cleanup_free_ char *p = NULL, *s = NULL, *t = NULL;
        const char *v;
        int r;

        assert_return(client, -EINVAL);
        assert_return(client->duid_len > 0, -ENODATA);
        assert_return(duid, -EINVAL);

        v = duid_type_to_string(be16toh(client->duid.type));
        if (v) {
                s = strdup(v);
                if (!s)
                        return -ENOMEM;
        } else {
                r = asprintf(&s, "%0x", client->duid.type);
                if (r < 0)
                        return -ENOMEM;
        }

        t = hexmem(&client->duid.raw.data, client->duid_len);
        if (!t)
                return -ENOMEM;

        p = strjoin(s, ":", t);
        if (!p)
                return -ENOMEM;

        *duid = TAKE_PTR(p);

        return 0;
}

int sd_dhcp6_client_set_iaid(sd_dhcp6_client *client, uint32_t iaid) {
        assert_return(client, -EINVAL);
        assert_return(!sd_dhcp6_client_is_running(client), -EBUSY);

        client->ia_na.header.id = htobe32(iaid);
        client->ia_pd.header.id = htobe32(iaid);
        client->iaid_set = true;

        return 0;
}

static int client_ensure_iaid(sd_dhcp6_client *client) {
        int r;
        uint32_t iaid;

        assert(client);

        if (client->iaid_set)
                return 0;

        r = dhcp_identifier_set_iaid(client->ifindex, client->hw_addr.bytes, client->hw_addr.length,
                                     /* legacy_unstable_byteorder = */ true,
                                     /* use_mac = */ client->test_mode,
                                     &iaid);
        if (r < 0)
                return r;

        client->ia_na.header.id = iaid;
        client->ia_pd.header.id = iaid;
        client->iaid_set = true;

        return 0;
}

int sd_dhcp6_client_get_iaid(sd_dhcp6_client *client, uint32_t *iaid) {
        assert_return(client, -EINVAL);
        assert_return(iaid, -EINVAL);

        if (!client->iaid_set)
                return -ENODATA;

        *iaid = be32toh(client->ia_na.header.id);

        return 0;
}

void dhcp6_client_set_test_mode(sd_dhcp6_client *client, bool test_mode) {
        assert(client);

        client->test_mode = test_mode;
}

int sd_dhcp6_client_set_fqdn(
                sd_dhcp6_client *client,
                const char *fqdn) {

        assert_return(client, -EINVAL);
        assert_return(!sd_dhcp6_client_is_running(client), -EBUSY);

        /* Make sure FQDN qualifies as DNS and as Linux hostname */
        if (fqdn &&
            !(hostname_is_valid(fqdn, 0) && dns_name_is_valid(fqdn) > 0))
                return -EINVAL;

        return free_and_strdup(&client->fqdn, fqdn);
}

int sd_dhcp6_client_set_information_request(sd_dhcp6_client *client, int enabled) {
        assert_return(client, -EINVAL);
        assert_return(!sd_dhcp6_client_is_running(client), -EBUSY);

        client->information_request = enabled;

        return 0;
}

int sd_dhcp6_client_get_information_request(sd_dhcp6_client *client, int *enabled) {
        assert_return(client, -EINVAL);
        assert_return(enabled, -EINVAL);

        *enabled = client->information_request;

        return 0;
}

int sd_dhcp6_client_set_request_option(sd_dhcp6_client *client, uint16_t option) {
        size_t t;

        assert_return(client, -EINVAL);
        assert_return(!sd_dhcp6_client_is_running(client), -EBUSY);

        if (!dhcp6_option_can_request(option))
                return -EINVAL;

        for (t = 0; t < client->req_opts_len; t++)
                if (client->req_opts[t] == htobe16(option))
                        return -EEXIST;

        if (!GREEDY_REALLOC(client->req_opts, client->req_opts_len + 1))
                return -ENOMEM;

        client->req_opts[client->req_opts_len++] = htobe16(option);

        return 0;
}

int sd_dhcp6_client_set_request_mud_url(sd_dhcp6_client *client, const char *mudurl) {
        assert_return(client, -EINVAL);
        assert_return(!sd_dhcp6_client_is_running(client), -EBUSY);
        assert_return(mudurl, -EINVAL);
        assert_return(strlen(mudurl) <= UINT8_MAX, -EINVAL);
        assert_return(http_url_is_valid(mudurl), -EINVAL);

        return free_and_strdup(&client->mudurl, mudurl);
}

int sd_dhcp6_client_set_request_user_class(sd_dhcp6_client *client, char * const *user_class) {
        char * const *p;
        char **s;

        assert_return(client, -EINVAL);
        assert_return(!sd_dhcp6_client_is_running(client), -EBUSY);
        assert_return(!strv_isempty(user_class), -EINVAL);

        STRV_FOREACH(p, user_class) {
                size_t len = strlen(*p);

                if (len > UINT16_MAX || len == 0)
                        return -EINVAL;
        }

        s = strv_copy(user_class);
        if (!s)
                return -ENOMEM;

        return strv_free_and_replace(client->user_class, s);
}

int sd_dhcp6_client_set_request_vendor_class(sd_dhcp6_client *client, char * const *vendor_class) {
        char * const *p;
        char **s;

        assert_return(client, -EINVAL);
        assert_return(!sd_dhcp6_client_is_running(client), -EBUSY);
        assert_return(!strv_isempty(vendor_class), -EINVAL);

        STRV_FOREACH(p, vendor_class) {
                size_t len = strlen(*p);

                if (len > UINT16_MAX || len == 0)
                        return -EINVAL;
        }

        s = strv_copy(vendor_class);
        if (!s)
                return -ENOMEM;

        return strv_free_and_replace(client->vendor_class, s);
}

int sd_dhcp6_client_get_prefix_delegation(sd_dhcp6_client *client, int *delegation) {
        assert_return(client, -EINVAL);
        assert_return(delegation, -EINVAL);

        *delegation = FLAGS_SET(client->request_ia, DHCP6_REQUEST_IA_PD);

        return 0;
}

int sd_dhcp6_client_set_prefix_delegation(sd_dhcp6_client *client, int delegation) {
        assert_return(client, -EINVAL);
        assert_return(!sd_dhcp6_client_is_running(client), -EBUSY);

        SET_FLAG(client->request_ia, DHCP6_REQUEST_IA_PD, delegation);

        return 0;
}

int sd_dhcp6_client_get_address_request(sd_dhcp6_client *client, int *request) {
        assert_return(client, -EINVAL);
        assert_return(request, -EINVAL);

        *request = FLAGS_SET(client->request_ia, DHCP6_REQUEST_IA_NA);

        return 0;
}

int sd_dhcp6_client_set_address_request(sd_dhcp6_client *client, int request) {
        assert_return(client, -EINVAL);
        assert_return(!sd_dhcp6_client_is_running(client), -EBUSY);

        SET_FLAG(client->request_ia, DHCP6_REQUEST_IA_NA, request);

        return 0;
}

int dhcp6_client_set_transaction_id(sd_dhcp6_client *client, uint32_t transaction_id) {
        assert(client);
        assert(client->test_mode);

        /* This is for tests or fuzzers. */

        client->transaction_id = transaction_id & htobe32(0x00ffffff);

        return 0;
}

int sd_dhcp6_client_get_lease(sd_dhcp6_client *client, sd_dhcp6_lease **ret) {
        assert_return(client, -EINVAL);

        if (!client->lease)
                return -ENOMSG;

        if (ret)
                *ret = client->lease;

        return 0;
}

int sd_dhcp6_client_add_option(sd_dhcp6_client *client, sd_dhcp6_option *v) {
        int r;

        assert_return(client, -EINVAL);
        assert_return(v, -EINVAL);

        r = ordered_hashmap_ensure_put(&client->extra_options, &dhcp6_option_hash_ops, UINT_TO_PTR(v->option), v);
        if (r < 0)
                return r;

        sd_dhcp6_option_ref(v);
        return 0;
}

static void client_set_state(sd_dhcp6_client *client, DHCP6State state) {
        assert(client);

        if (client->state == state)
                return;

        log_dhcp6_client(client, "State changed: %s -> %s",
                         dhcp6_state_to_string(client->state), dhcp6_state_to_string(state));

        client->state = state;
}

static void client_notify(sd_dhcp6_client *client, int event) {
        assert(client);

        if (client->callback)
                client->callback(client, event, client->userdata);
}

static void client_stop(sd_dhcp6_client *client, int error) {
        DHCP6_CLIENT_DONT_DESTROY(client);

        assert(client);

        client_notify(client, error);

        client->lease = sd_dhcp6_lease_unref(client->lease);

        /* Reset IRT here. Otherwise, we cannot restart the client in the information requesting mode,
         * even though the lease is freed below. */
        client->information_request_time_usec = 0;
        client->information_refresh_time_usec = 0;

        (void) event_source_disable(client->receive_message);
        (void) event_source_disable(client->timeout_resend);
        (void) event_source_disable(client->timeout_expire);
        (void) event_source_disable(client->timeout_t1);
        (void) event_source_disable(client->timeout_t2);

        client_set_state(client, DHCP6_STATE_STOPPED);
}

static int client_append_common_options_in_managed_mode(
                sd_dhcp6_client *client,
                uint8_t **opt,
                size_t *optlen,
                const DHCP6IA *ia_na,
                const DHCP6IA *ia_pd) {

        int r;

        assert(client);
        assert(IN_SET(client->state,
                      DHCP6_STATE_SOLICITATION,
                      DHCP6_STATE_REQUEST,
                      DHCP6_STATE_RENEW,
                      DHCP6_STATE_REBIND));
        assert(opt);
        assert(optlen);

        if (FLAGS_SET(client->request_ia, DHCP6_REQUEST_IA_NA) && ia_na) {
                r = dhcp6_option_append_ia(opt, optlen, ia_na);
                if (r < 0)
                        return r;
        }

        if (FLAGS_SET(client->request_ia, DHCP6_REQUEST_IA_PD) && ia_pd) {
                r = dhcp6_option_append_ia(opt, optlen, ia_pd);
                if (r < 0)
                        return r;
        }

        if (client->fqdn) {
                r = dhcp6_option_append_fqdn(opt, optlen, client->fqdn);
                if (r < 0)
                        return r;
        }

        if (client->user_class) {
                r = dhcp6_option_append_user_class(opt, optlen, client->user_class);
                if (r < 0)
                        return r;
        }

        if (client->vendor_class) {
                r = dhcp6_option_append_vendor_class(opt, optlen, client->vendor_class);
                if (r < 0)
                        return r;
        }

        if (!ordered_set_isempty(client->vendor_options)) {
                r = dhcp6_option_append_vendor_option(opt, optlen, client->vendor_options);
                if (r < 0)
                        return r;
        }

        return 0;
}

static DHCP6MessageType client_message_type_from_state(sd_dhcp6_client *client) {
        assert(client);

        switch (client->state) {
        case DHCP6_STATE_INFORMATION_REQUEST:
                return DHCP6_MESSAGE_INFORMATION_REQUEST;
        case DHCP6_STATE_SOLICITATION:
                return DHCP6_MESSAGE_SOLICIT;
        case DHCP6_STATE_REQUEST:
                return DHCP6_MESSAGE_REQUEST;
        case DHCP6_STATE_RENEW:
                return DHCP6_MESSAGE_RENEW;
        case DHCP6_STATE_REBIND:
                return DHCP6_MESSAGE_REBIND;
        default:
                assert_not_reached();
        }
}

int dhcp6_client_send_message(sd_dhcp6_client *client) {
        _cleanup_free_ DHCP6Message *message = NULL;
        struct in6_addr all_servers =
                IN6ADDR_ALL_DHCP6_RELAY_AGENTS_AND_SERVERS_INIT;
        struct sd_dhcp6_option *j;
        size_t len, optlen = 512;
        uint8_t *opt;
        usec_t elapsed_usec, time_now;
        be16_t elapsed_time;
        int r;

        assert(client);
        assert(client->event);

        r = sd_event_now(client->event, clock_boottime_or_monotonic(), &time_now);
        if (r < 0)
                return r;

        len = sizeof(DHCP6Message) + optlen;

        message = malloc0(len);
        if (!message)
                return -ENOMEM;

        opt = (uint8_t *)(message + 1);

        message->transaction_id = client->transaction_id;
        message->type = client_message_type_from_state(client);

        switch (client->state) {
        case DHCP6_STATE_INFORMATION_REQUEST:
                break;

        case DHCP6_STATE_SOLICITATION:
                r = dhcp6_option_append(&opt, &optlen, SD_DHCP6_OPTION_RAPID_COMMIT, 0, NULL);
                if (r < 0)
                        return r;

                r = client_append_common_options_in_managed_mode(client, &opt, &optlen,
                                                                 &client->ia_na, &client->ia_pd);
                if (r < 0)
                        return r;
                break;

        case DHCP6_STATE_REQUEST:
        case DHCP6_STATE_RENEW:

                r = dhcp6_option_append(&opt, &optlen, SD_DHCP6_OPTION_SERVERID,
                                        client->lease->serverid_len,
                                        client->lease->serverid);
                if (r < 0)
                        return r;

                _fallthrough_;
        case DHCP6_STATE_REBIND:

                assert(client->lease);

                r = client_append_common_options_in_managed_mode(client, &opt, &optlen,
                                                                 client->lease->ia_na, client->lease->ia_pd);
                if (r < 0)
                        return r;
                break;

        case DHCP6_STATE_STOPPED:
        case DHCP6_STATE_BOUND:
        default:
                assert_not_reached();
        }

        if (client->mudurl) {
                r = dhcp6_option_append(&opt, &optlen, SD_DHCP6_OPTION_MUD_URL_V6,
                                        strlen(client->mudurl), client->mudurl);
                if (r < 0)
                        return r;
        }

        r = dhcp6_option_append(&opt, &optlen, SD_DHCP6_OPTION_ORO,
                                client->req_opts_len * sizeof(be16_t),
                                client->req_opts);
        if (r < 0)
                return r;

        assert(client->duid_len);
        r = dhcp6_option_append(&opt, &optlen, SD_DHCP6_OPTION_CLIENTID,
                                client->duid_len, &client->duid);
        if (r < 0)
                return r;

        ORDERED_HASHMAP_FOREACH(j, client->extra_options) {
                r = dhcp6_option_append(&opt, &optlen, j->option, j->length, j->data);
                if (r < 0)
                        return r;
        }

        /* RFC 8415 Section 21.9.
         * A client MUST include an Elapsed Time option in messages to indicate how long the client has
         * been trying to complete a DHCP message exchange. */
        elapsed_usec = MIN(usec_sub_unsigned(time_now, client->transaction_start) / USEC_PER_MSEC / 10, (usec_t) UINT16_MAX);
        elapsed_time = htobe16(elapsed_usec);
        r = dhcp6_option_append(&opt, &optlen, SD_DHCP6_OPTION_ELAPSED_TIME, sizeof(elapsed_time), &elapsed_time);
        if (r < 0)
                return r;

        r = dhcp6_network_send_udp_socket(client->fd, &all_servers, message,
                                          len - optlen);
        if (r < 0)
                return r;

        log_dhcp6_client(client, "Sent %s",
                         dhcp6_message_type_to_string(message->type));

        return 0;
}

static usec_t client_timeout_compute_random(usec_t val) {
        return usec_sub_unsigned(val, random_u64_range(val / 10));
}

static int client_timeout_resend(sd_event_source *s, uint64_t usec, void *userdata) {
        sd_dhcp6_client *client = ASSERT_PTR(userdata);
        usec_t init_retransmit_time, max_retransmit_time;
        int r;

        assert(client->event);

        switch (client->state) {
        case DHCP6_STATE_INFORMATION_REQUEST:
                init_retransmit_time = DHCP6_INF_TIMEOUT;
                max_retransmit_time = DHCP6_INF_MAX_RT;
                break;

        case DHCP6_STATE_SOLICITATION:

                if (client->retransmit_count > 0 && client->lease) {
                        (void) client_start_transaction(client, DHCP6_STATE_REQUEST);
                        return 0;
                }

                init_retransmit_time = DHCP6_SOL_TIMEOUT;
                max_retransmit_time = DHCP6_SOL_MAX_RT;
                break;

        case DHCP6_STATE_REQUEST:

                if (client->retransmit_count >= DHCP6_REQ_MAX_RC) {
                        client_stop(client, SD_DHCP6_CLIENT_EVENT_RETRANS_MAX);
                        return 0;
                }

                init_retransmit_time = DHCP6_REQ_TIMEOUT;
                max_retransmit_time = DHCP6_REQ_MAX_RT;
                break;

        case DHCP6_STATE_RENEW:
                init_retransmit_time = DHCP6_REN_TIMEOUT;
                max_retransmit_time = DHCP6_REN_MAX_RT;

                /* RFC 3315, section 18.1.3. says max retransmit duration will
                   be the remaining time until T2. Instead of setting MRD,
                   wait for T2 to trigger with the same end result */
                break;

        case DHCP6_STATE_REBIND:
                init_retransmit_time = DHCP6_REB_TIMEOUT;
                max_retransmit_time = DHCP6_REB_MAX_RT;

                /* Also, instead of setting MRD, the expire timer is already set in client_enter_bound_state(). */
                break;

        case DHCP6_STATE_STOPPED:
        case DHCP6_STATE_BOUND:
        default:
                assert_not_reached();
        }

        r = dhcp6_client_send_message(client);
        if (r >= 0)
                client->retransmit_count++;

        if (client->retransmit_time == 0) {
                client->retransmit_time = client_timeout_compute_random(init_retransmit_time);

                if (client->state == DHCP6_STATE_SOLICITATION)
                        client->retransmit_time += init_retransmit_time / 10;

        } else if (client->retransmit_time > max_retransmit_time / 2)
                client->retransmit_time = client_timeout_compute_random(max_retransmit_time);
        else
                client->retransmit_time += client_timeout_compute_random(client->retransmit_time);

        log_dhcp6_client(client, "Next retransmission in %s",
                         FORMAT_TIMESPAN(client->retransmit_time, USEC_PER_SEC));

        r = event_reset_time_relative(client->event, &client->timeout_resend,
                                      clock_boottime_or_monotonic(),
                                      client->retransmit_time, 10 * USEC_PER_MSEC,
                                      client_timeout_resend, client,
                                      client->event_priority, "dhcp6-resend-timer", true);
        if (r < 0)
                client_stop(client, r);

        return 0;
}

static int client_start_transaction(sd_dhcp6_client *client, DHCP6State state) {
        int r;

        assert(client);
        assert(client->event);

        switch (state) {
        case DHCP6_STATE_INFORMATION_REQUEST:
        case DHCP6_STATE_SOLICITATION:
                assert(client->state == DHCP6_STATE_STOPPED);
                break;
        case DHCP6_STATE_REQUEST:
                assert(client->state == DHCP6_STATE_SOLICITATION);
                break;
        case DHCP6_STATE_RENEW:
                assert(client->state == DHCP6_STATE_BOUND);
                break;
        case DHCP6_STATE_REBIND:
                assert(IN_SET(client->state, DHCP6_STATE_BOUND, DHCP6_STATE_RENEW));
                break;
        case DHCP6_STATE_STOPPED:
        case DHCP6_STATE_BOUND:
        default:
                assert_not_reached();
        }

        client_set_state(client, state);

        client->retransmit_time = 0;
        client->retransmit_count = 0;
        client->transaction_id = random_u32() & htobe32(0x00ffffff);

        r = sd_event_now(client->event, clock_boottime_or_monotonic(), &client->transaction_start);
        if (r < 0)
                goto error;

        r = event_reset_time(client->event, &client->timeout_resend,
                             clock_boottime_or_monotonic(),
                             0, 0,
                             client_timeout_resend, client,
                             client->event_priority, "dhcp6-resend-timeout", true);
        if (r < 0)
                goto error;

        r = sd_event_source_set_enabled(client->receive_message, SD_EVENT_ON);
        if (r < 0)
                goto error;

        return 0;

error:
        client_stop(client, r);
        return r;
}

static int client_timeout_expire(sd_event_source *s, uint64_t usec, void *userdata) {
        sd_dhcp6_client *client = ASSERT_PTR(userdata);
        DHCP6_CLIENT_DONT_DESTROY(client);
        DHCP6State state;

        (void) event_source_disable(client->timeout_expire);
        (void) event_source_disable(client->timeout_t2);
        (void) event_source_disable(client->timeout_t1);

        state = client->state;

        client_stop(client, SD_DHCP6_CLIENT_EVENT_RESEND_EXPIRE);

        /* RFC 3315, section 18.1.4., says that "...the client may choose to
           use a Solicit message to locate a new DHCP server..." */
        if (state == DHCP6_STATE_REBIND)
                (void) client_start_transaction(client, DHCP6_STATE_SOLICITATION);

        return 0;
}

static int client_timeout_t2(sd_event_source *s, uint64_t usec, void *userdata) {
        sd_dhcp6_client *client = ASSERT_PTR(userdata);

        (void) event_source_disable(client->timeout_t2);
        (void) event_source_disable(client->timeout_t1);

        log_dhcp6_client(client, "Timeout T2");

        (void) client_start_transaction(client, DHCP6_STATE_REBIND);

        return 0;
}

static int client_timeout_t1(sd_event_source *s, uint64_t usec, void *userdata) {
        sd_dhcp6_client *client = ASSERT_PTR(userdata);

        (void) event_source_disable(client->timeout_t1);

        log_dhcp6_client(client, "Timeout T1");

        (void) client_start_transaction(client, DHCP6_STATE_RENEW);

        return 0;
}

static int client_enter_bound_state(sd_dhcp6_client *client) {
        usec_t lifetime_t1, lifetime_t2, lifetime_valid;
        int r;

        assert(client);
        assert(client->lease);
        assert(IN_SET(client->state,
                      DHCP6_STATE_SOLICITATION,
                      DHCP6_STATE_REQUEST,
                      DHCP6_STATE_RENEW,
                      DHCP6_STATE_REBIND));

        (void) event_source_disable(client->receive_message);
        (void) event_source_disable(client->timeout_resend);

        r = dhcp6_lease_get_lifetime(client->lease, &lifetime_t1, &lifetime_t2, &lifetime_valid);
        if (r < 0)
                goto error;

        lifetime_t2 = client_timeout_compute_random(lifetime_t2);
        lifetime_t1 = client_timeout_compute_random(MIN(lifetime_t1, lifetime_t2));

        if (lifetime_t1 == USEC_INFINITY) {
                log_dhcp6_client(client, "Infinite T1");
                event_source_disable(client->timeout_t1);
        } else {
                log_dhcp6_client(client, "T1 expires in %s", FORMAT_TIMESPAN(lifetime_t1, USEC_PER_SEC));
                r = event_reset_time_relative(client->event, &client->timeout_t1,
                                              clock_boottime_or_monotonic(),
                                              lifetime_t1, 10 * USEC_PER_SEC,
                                              client_timeout_t1, client,
                                              client->event_priority, "dhcp6-t1-timeout", true);
                if (r < 0)
                        goto error;
        }

        if (lifetime_t2 == USEC_INFINITY) {
                log_dhcp6_client(client, "Infinite T2");
                event_source_disable(client->timeout_t2);
        } else {
                log_dhcp6_client(client, "T2 expires in %s", FORMAT_TIMESPAN(lifetime_t2, USEC_PER_SEC));
                r = event_reset_time_relative(client->event, &client->timeout_t2,
                                              clock_boottime_or_monotonic(),
                                              lifetime_t2, 10 * USEC_PER_SEC,
                                              client_timeout_t2, client,
                                              client->event_priority, "dhcp6-t2-timeout", true);
                if (r < 0)
                        goto error;
        }

        if (lifetime_valid == USEC_INFINITY) {
                log_dhcp6_client(client, "Infinite valid lifetime");
                event_source_disable(client->timeout_expire);
        } else {
                log_dhcp6_client(client, "Valid lifetime expires in %s", FORMAT_TIMESPAN(lifetime_valid, USEC_PER_SEC));

                r = event_reset_time_relative(client->event, &client->timeout_expire,
                                              clock_boottime_or_monotonic(),
                                              lifetime_valid, USEC_PER_SEC,
                                              client_timeout_expire, client,
                                              client->event_priority, "dhcp6-lease-expire", true);
                if (r < 0)
                        goto error;
        }

        client_set_state(client, DHCP6_STATE_BOUND);
        client_notify(client, SD_DHCP6_CLIENT_EVENT_IP_ACQUIRE);
        return 0;

error:
        client_stop(client, r);
        return r;
}

static int log_invalid_message_type(sd_dhcp6_client *client, const DHCP6Message *message) {
        const char *type_str;

        assert(client);
        assert(message);

        type_str = dhcp6_message_type_to_string(message->type);
        if (type_str)
                return log_dhcp6_client_errno(client, SYNTHETIC_ERRNO(EINVAL),
                                              "Received unexpected %s message, ignoring.", type_str);
        else
                return log_dhcp6_client_errno(client, SYNTHETIC_ERRNO(EINVAL),
                                              "Received unsupported message type %u, ignoring.", message->type);
}

static int client_process_information(
                sd_dhcp6_client *client,
                DHCP6Message *message,
                size_t len,
                const triple_timestamp *timestamp,
                const struct in6_addr *server_address) {

        _cleanup_(sd_dhcp6_lease_unrefp) sd_dhcp6_lease *lease = NULL;
        int r;

        assert(client);
        assert(message);

        if (message->type != DHCP6_MESSAGE_REPLY)
                return log_invalid_message_type(client, message);

        r = dhcp6_lease_new_from_message(client, message, len, timestamp, server_address, &lease);
        if (r < 0)
                return log_dhcp6_client_errno(client, r, "Failed to process received reply message, ignoring: %m");

        log_dhcp6_client(client, "Processed %s message", dhcp6_message_type_to_string(message->type));

        sd_dhcp6_lease_unref(client->lease);
        client->lease = TAKE_PTR(lease);

        /* Do not call client_stop() here, as it frees the acquired lease. */
        (void) event_source_disable(client->receive_message);
        (void) event_source_disable(client->timeout_resend);
        client_set_state(client, DHCP6_STATE_STOPPED);

        client_notify(client, SD_DHCP6_CLIENT_EVENT_INFORMATION_REQUEST);
        return 0;
}

static int client_process_reply(
                sd_dhcp6_client *client,
                DHCP6Message *message,
                size_t len,
                const triple_timestamp *timestamp,
                const struct in6_addr *server_address) {

        _cleanup_(sd_dhcp6_lease_unrefp) sd_dhcp6_lease *lease = NULL;
        int r;

        assert(client);
        assert(message);

        if (message->type != DHCP6_MESSAGE_REPLY)
                return log_invalid_message_type(client, message);

        r = dhcp6_lease_new_from_message(client, message, len, timestamp, server_address, &lease);
        if (r < 0)
                return log_dhcp6_client_errno(client, r, "Failed to process received reply message, ignoring: %m");

        log_dhcp6_client(client, "Processed %s message", dhcp6_message_type_to_string(message->type));

        sd_dhcp6_lease_unref(client->lease);
        client->lease = TAKE_PTR(lease);

        return client_enter_bound_state(client);
}

static int client_process_advertise_or_rapid_commit_reply(
                sd_dhcp6_client *client,
                DHCP6Message *message,
                size_t len,
                const triple_timestamp *timestamp,
                const struct in6_addr *server_address) {

        _cleanup_(sd_dhcp6_lease_unrefp) sd_dhcp6_lease *lease = NULL;
        uint8_t pref_advertise, pref_lease = 0;
        int r;

        assert(client);
        assert(message);

        if (!IN_SET(message->type, DHCP6_MESSAGE_ADVERTISE, DHCP6_MESSAGE_REPLY))
                return log_invalid_message_type(client, message);

        r = dhcp6_lease_new_from_message(client, message, len, timestamp, server_address, &lease);
        if (r < 0)
                return log_dhcp6_client_errno(client, r, "Failed to process received %s message, ignoring: %m",
                                              dhcp6_message_type_to_string(message->type));

        if (message->type == DHCP6_MESSAGE_REPLY) {
                bool rapid_commit;

                r = dhcp6_lease_get_rapid_commit(lease, &rapid_commit);
                if (r < 0)
                        return r;

                if (!rapid_commit)
                        return log_dhcp6_client_errno(client, SYNTHETIC_ERRNO(EINVAL),
                                                      "Received reply message without rapid commit flag, ignoring.");

                log_dhcp6_client(client, "Processed %s message", dhcp6_message_type_to_string(message->type));

                sd_dhcp6_lease_unref(client->lease);
                client->lease = TAKE_PTR(lease);

                return client_enter_bound_state(client);
        }

        r = dhcp6_lease_get_preference(lease, &pref_advertise);
        if (r < 0)
                return r;

        if (client->lease) {
                r = dhcp6_lease_get_preference(client->lease, &pref_lease);
                if (r < 0)
                        return r;
        }

        log_dhcp6_client(client, "Processed %s message", dhcp6_message_type_to_string(message->type));

        if (!client->lease || pref_advertise > pref_lease) {
                /* If this is the first advertise message or has higher preference, then save the lease. */
                sd_dhcp6_lease_unref(client->lease);
                client->lease = TAKE_PTR(lease);
        }

        if (pref_advertise == 255 || client->retransmit_count > 1)
                (void) client_start_transaction(client, DHCP6_STATE_REQUEST);

        return 0;
}

static int client_receive_message(
                sd_event_source *s,
                int fd, uint32_t
                revents,
                void *userdata) {

        sd_dhcp6_client *client = ASSERT_PTR(userdata);
        DHCP6_CLIENT_DONT_DESTROY(client);
        /* This needs to be initialized with zero. See #20741. */
        CMSG_BUFFER_TYPE(CMSG_SPACE_TIMEVAL) control = {};
        struct iovec iov;
        union sockaddr_union sa = {};
        struct msghdr msg = {
                .msg_name = &sa.sa,
                .msg_namelen = sizeof(sa),
                .msg_iov = &iov,
                .msg_iovlen = 1,
                .msg_control = &control,
                .msg_controllen = sizeof(control),
        };
        struct cmsghdr *cmsg;
        triple_timestamp t = {};
        _cleanup_free_ DHCP6Message *message = NULL;
        struct in6_addr *server_address = NULL;
        ssize_t buflen, len;

        buflen = next_datagram_size_fd(fd);
        if (buflen < 0) {
                if (ERRNO_IS_TRANSIENT(buflen) || ERRNO_IS_DISCONNECT(buflen))
                        return 0;

                log_dhcp6_client_errno(client, buflen, "Failed to determine datagram size to read, ignoring: %m");
                return 0;
        }

        message = malloc(buflen);
        if (!message)
                return -ENOMEM;

        iov = IOVEC_MAKE(message, buflen);

        len = recvmsg_safe(fd, &msg, MSG_DONTWAIT);
        if (len < 0) {
                if (ERRNO_IS_TRANSIENT(len) || ERRNO_IS_DISCONNECT(len))
                        return 0;

                log_dhcp6_client_errno(client, len, "Could not receive message from UDP socket, ignoring: %m");
                return 0;
        }
        if ((size_t) len < sizeof(DHCP6Message)) {
                log_dhcp6_client(client, "Too small to be DHCP6 message: ignoring");
                return 0;
        }

        /* msg_namelen == 0 happens when running the test-suite over a socketpair */
        if (msg.msg_namelen > 0) {
                if (msg.msg_namelen != sizeof(struct sockaddr_in6) || sa.in6.sin6_family != AF_INET6) {
                        log_dhcp6_client(client, "Received message from invalid source, ignoring.");
                        return 0;
                }

                server_address = &sa.in6.sin6_addr;
        }

        CMSG_FOREACH(cmsg, &msg) {
                if (cmsg->cmsg_level == SOL_SOCKET &&
                    cmsg->cmsg_type == SO_TIMESTAMP &&
                    cmsg->cmsg_len == CMSG_LEN(sizeof(struct timeval)))
                        triple_timestamp_from_realtime(&t, timeval_load((struct timeval*) CMSG_DATA(cmsg)));
        }

        if (client->transaction_id != (message->transaction_id & htobe32(0x00ffffff)))
                return 0;

        switch (client->state) {
        case DHCP6_STATE_INFORMATION_REQUEST:
                if (client_process_information(client, message, len, &t, server_address) < 0)
                        return 0;
                break;

        case DHCP6_STATE_SOLICITATION:
                if (client_process_advertise_or_rapid_commit_reply(client, message, len, &t, server_address) < 0)
                        return 0;
                break;

        case DHCP6_STATE_REQUEST:
        case DHCP6_STATE_RENEW:
        case DHCP6_STATE_REBIND:
                if (client_process_reply(client, message, len, &t, server_address) < 0)
                        return 0;
                break;

        case DHCP6_STATE_BOUND:
        case DHCP6_STATE_STOPPED:
        default:
                assert_not_reached();
        }

        return 0;
}

int sd_dhcp6_client_stop(sd_dhcp6_client *client) {
        if (!client)
                return 0;

        client_stop(client, SD_DHCP6_CLIENT_EVENT_STOP);

        client->receive_message = sd_event_source_unref(client->receive_message);
        client->fd = safe_close(client->fd);

        return 0;
}

int sd_dhcp6_client_is_running(sd_dhcp6_client *client) {
        assert_return(client, -EINVAL);

        return client->state != DHCP6_STATE_STOPPED;
}

int sd_dhcp6_client_start(sd_dhcp6_client *client) {
        DHCP6State state = DHCP6_STATE_SOLICITATION;
        int r;

        assert_return(client, -EINVAL);
        assert_return(client->event, -EINVAL);
        assert_return(client->ifindex > 0, -EINVAL);
        assert_return(in6_addr_is_link_local(&client->local_address) > 0, -EINVAL);
        assert_return(!sd_dhcp6_client_is_running(client), -EBUSY);
        assert_return(client->information_request || client->request_ia != 0, -EINVAL);

        /* Even if the client is in the STOPPED state, the lease acquired in the previous information
         * request may be stored. */
        client->lease = sd_dhcp6_lease_unref(client->lease);

        r = client_ensure_iaid(client);
        if (r < 0)
                return r;

        r = client_ensure_duid(client);
        if (r < 0)
                return r;

        if (client->fd < 0) {
                r = dhcp6_network_bind_udp_socket(client->ifindex, &client->local_address);
                if (r < 0) {
                        _cleanup_free_ char *p = NULL;

                        (void) in6_addr_to_string(&client->local_address, &p);
                        return log_dhcp6_client_errno(client, r,
                                                      "Failed to bind to UDP socket at address %s: %m", strna(p));
                }

                client->fd = r;
        }

        if (!client->receive_message) {
                _cleanup_(sd_event_source_disable_unrefp) sd_event_source *s = NULL;

                r = sd_event_add_io(client->event, &s, client->fd, EPOLLIN, client_receive_message, client);
                if (r < 0)
                        return r;

                r = sd_event_source_set_priority(s, client->event_priority);
                if (r < 0)
                        return r;

                r = sd_event_source_set_description(s, "dhcp6-receive-message");
                if (r < 0)
                        return r;

                client->receive_message = TAKE_PTR(s);
        }

        if (client->information_request) {
                usec_t t = now(CLOCK_MONOTONIC);

                if (t < usec_add(client->information_request_time_usec, client->information_refresh_time_usec))
                        return 0;

                client->information_request_time_usec = t;
                state = DHCP6_STATE_INFORMATION_REQUEST;
        }

        log_dhcp6_client(client, "Starting in %s mode",
                         client->information_request ? "Information request" : "Managed");

        return client_start_transaction(client, state);
}

int sd_dhcp6_client_attach_event(sd_dhcp6_client *client, sd_event *event, int64_t priority) {
        int r;

        assert_return(client, -EINVAL);
        assert_return(!client->event, -EBUSY);
        assert_return(!sd_dhcp6_client_is_running(client), -EBUSY);

        if (event)
                client->event = sd_event_ref(event);
        else {
                r = sd_event_default(&client->event);
                if (r < 0)
                        return 0;
        }

        client->event_priority = priority;

        return 0;
}

int sd_dhcp6_client_detach_event(sd_dhcp6_client *client) {
        assert_return(client, -EINVAL);
        assert_return(!sd_dhcp6_client_is_running(client), -EBUSY);

        client->event = sd_event_unref(client->event);

        return 0;
}

sd_event *sd_dhcp6_client_get_event(sd_dhcp6_client *client) {
        assert_return(client, NULL);

        return client->event;
}

static sd_dhcp6_client *dhcp6_client_free(sd_dhcp6_client *client) {
        if (!client)
                return NULL;

        sd_dhcp6_lease_unref(client->lease);

        sd_event_source_disable_unref(client->receive_message);
        sd_event_source_disable_unref(client->timeout_resend);
        sd_event_source_disable_unref(client->timeout_expire);
        sd_event_source_disable_unref(client->timeout_t1);
        sd_event_source_disable_unref(client->timeout_t2);
        sd_event_unref(client->event);

        client->fd = safe_close(client->fd);

        free(client->req_opts);
        free(client->fqdn);
        free(client->mudurl);
        dhcp6_ia_clear_addresses(&client->ia_pd);
        ordered_hashmap_free(client->extra_options);
        ordered_set_free(client->vendor_options);
        strv_free(client->user_class);
        strv_free(client->vendor_class);
        free(client->ifname);

        return mfree(client);
}

DEFINE_TRIVIAL_REF_UNREF_FUNC(sd_dhcp6_client, sd_dhcp6_client, dhcp6_client_free);

int sd_dhcp6_client_new(sd_dhcp6_client **ret) {
        _cleanup_(sd_dhcp6_client_unrefp) sd_dhcp6_client *client = NULL;
        _cleanup_free_ be16_t *req_opts = NULL;
        size_t t;

        assert_return(ret, -EINVAL);

        req_opts = new(be16_t, ELEMENTSOF(default_req_opts));
        if (!req_opts)
                return -ENOMEM;

        for (t = 0; t < ELEMENTSOF(default_req_opts); t++)
                req_opts[t] = htobe16(default_req_opts[t]);

        client = new(sd_dhcp6_client, 1);
        if (!client)
                return -ENOMEM;

        *client = (sd_dhcp6_client) {
                .n_ref = 1,
                .ia_na.type = SD_DHCP6_OPTION_IA_NA,
                .ia_pd.type = SD_DHCP6_OPTION_IA_PD,
                .ifindex = -1,
                .request_ia = DHCP6_REQUEST_IA_NA | DHCP6_REQUEST_IA_PD,
                .fd = -1,
                .req_opts_len = ELEMENTSOF(default_req_opts),
                .req_opts = TAKE_PTR(req_opts),
        };

        *ret = TAKE_PTR(client);

        return 0;
}