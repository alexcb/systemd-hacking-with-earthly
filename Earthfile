deps:
    FROM ubuntu:21.04
    ENV DEBIAN_FRONTEND noninteractive
    ENV DEBCONF_NONINTERACTIVE_SEEN true
    RUN sed -Ei 's/^# deb-src /deb-src /' /etc/apt/sources.list
    RUN apt-get update && apt-get -y build-dep systemd python3 jinja2

systemd:
    FROM +deps
    COPY --dir systemd /code
    WORKDIR /code
    RUN meson build
    RUN meson compile -C build
    RUN DESTDIR=/systemd_install make install
    SAVE ARTIFACT /systemd_install

ubuntu-systemd:
    FROM ubuntu:21.04
    RUN apt-get update && apt-get install -y libkmod-dev libapparmor-dev libip4tc2
    RUN test ! -d /usr/lib/systemd/system/runlevel5.target.wants # sanity check to ensure systemd isn't in base image
    COPY +systemd/systemd_install /
    RUN test -d /usr/lib/systemd/system/runlevel5.target.wants # sanity check to ensure systemd is correctly copied
    RUN ldd /lib/systemd/systemd
    RUN /lib/systemd/systemd --help

test-ubuntu-systemd:
    FROM earthly/dind:alpine
    #COPY on-host.sh .
    WITH DOCKER --load usd:latest=+ubuntu-systemd
        RUN docker run --privileged usd /sbin/init
        #RUN --no-cache KEEP=1 IMG=sd EXTRA="-e SYSTEMD_LOG_LEVEL=debug" ./on-host.sh
    END

kind-systemd:
    FROM kindest/node:v1.21.1
    COPY +systemd/systemd_install /

test-kind-systemd:
    FROM earthly/dind:alpine
    COPY on-host.sh .
    WITH DOCKER --load ksd:latest=+kind-systemd
        RUN --no-cache KEEP=1 IMG=ksd EXTRA="-e SYSTEMD_LOG_LEVEL=info" ./on-host.sh
        #RUN --no-cache KEEP=1 IMG=ksd EXTRA="-e SYSTEMD_LOG_LEVEL=debug" ./on-host.sh
    END

test-cgroup-img:
    FROM ubuntu:21.04
    COPY create-and-move-into-cgroup.sh .

test-cgroup:
    FROM earthly/dind:alpine
    WITH DOCKER --load tci:latest=+test-cgroup-img
        RUN --no-cache docker run tci:latest ./create-and-move-into-cgroup.sh
    END
