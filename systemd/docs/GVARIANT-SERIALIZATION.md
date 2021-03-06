---
title: GVariant D-Bus Message Serialization
category: Interfaces
layout: default
SPDX-License-Identifier: LGPL-2.1-or-later
---

# GVariant D-Bus Message Serialization

We stay as close to the original dbus1 framing as possible, but make
certain changes to adapt for GVariant. dbus1 has the following
framing:

    1. A fixed header of "yyyyuu"
    2. Additional header fields of "a(yv)"
    3. Padding with NUL bytes to pad up to next 8byte boundary
    4. The body

Note that the body is not padded at the end, the complete message
hence might have a non-aligned size. Reading multiple messages at once
will hence result in possibly unaligned messages in memory.

The header consists of the following:

    y  Endianness, 'l' or 'B'
    y  Message Type
    y  Flags
    y  Protocol version, '1'
    u  Length of the body, i.e. the length of part 4 above
    u  32bit Serial number

    = 12 bytes

This header is then followed by the fields array, whose first value is
a 32bit array size.

When using GVariant we keep the basic structure in place, only
slightly alter the header, and define protocol version '2'. The new
header:

    y  Endianness, 'l' or 'B'
    y  Message Type
    y  Flags
    y  Protocol version, '2'
    u  Reserved, must be 0
    t  64bit Cookie

    = 16 bytes

This is then followed by the GVariant fields array ("a{tv}"), and
finally the actual body as variant (v). Putting this altogether a
packet on dbus2 hence qualifies as a fully compliant GVariant
structure of (yyyyuta{tv}v).

For details on gvariant, see:

https://people.gnome.org/~desrt/gvariant-serialisation.pdf

Regarding the framing of dbus2, also see:

https://wiki.gnome.org/Projects/GLib/GDBus/Version2

The first four bytes of the header are defined the same way for dbus1
and dbus2. The first bytes contain the endianness field and the
protocol version, so that the remainder of the message can be safely
made sense of just by looking at the first 32bit.

Note that the length of the body is no longer included in the header
on dbus2! In fact, the message size must be known in advance, from the
underlying transport in order to parse dbus2 messages, while it is
directly included in dbus1 message headers. This change of semantics
is an effect of GVariant's basic design.

The serial number has been renamed cookie and has been extended from
32bit to 64bit. It is recommended to avoid the higher 32bit of the
cookie field though, to simplify compatibility with dbus1 peers. Note
that not only the cookie/serial field in the fixed header, but also
the reply_cookie/reply_serial additional header field has been
increased from 32bit to 64bit, too!

The header field identifiers have been extended from 8bit to
64bit. This has been done to simplify things, and has no effect
on the serialization size, as due to alignment for each 8bit
header field identifier 56 bits of padding had to be added.

Note that the header size changed, due to these changes. However,
consider that on dbus1 the beginning of the fields array contains the
32bit array size (since that is how arrays are encoded on dbus1),
thus, if one considers that size part of the header, instead of the
array, the size of the header on dbus1 and dbus2 stays identical, at
16 bytes.

              0               4               8               12              16
      Common: | E | T | F | V | ...

       dbus1: |  (as above)   | Body Length   | Serial        | Fields Length | Fields array ...

    gvariant: |  (as above)   | Reserved      | Cookie                        | Fields array ...

And that's already it.

Note: To simplify parsing, valid dbus2 messages must include the entire
      fixed header and additional header fields in a single non-memfd
      message part. Also, the signature string of the body variant all the
      way to the end of the message must be in a single non-memfd part
      too. The parts for this extended header and footer can be the same
      one, and can also continue any amount of additional body bytes.

Note: The GVariant "MAYBE" type is not supported, so that messages can
      be fully converted forth and back between dbus1 and gvariant
      representations.
