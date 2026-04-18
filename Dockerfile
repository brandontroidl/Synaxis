FROM debian:12

ENV GID=1234
ENV UID=1234

RUN DEBIAN_FRONTEND=noninteractive apt-get update && \
    apt-get -y install build-essential libssl-dev autoconf automake \
    libtre-dev libgeoip-dev libsodium-dev libargon2-dev \
    python3-dev gawk git procps net-tools && \
    apt-get clean

RUN mkdir -p /synaxis/src /synaxis/services
COPY . /synaxis/src

RUN groupadd -g ${GID} synaxis && \
    useradd -u ${UID} -g ${GID} synaxis && \
    chown -R synaxis:synaxis /synaxis

USER synaxis
WORKDIR /synaxis/src

RUN autoreconf -fi && \
    ./configure --prefix=/synaxis/services \
      --enable-modules=blacklist,botserv,helpserv,hostserv,memoserv,\
no,python,qserver,snoop,sockcheck,track,webtv && \
    make -j$(nproc) && \
    make install

WORKDIR /synaxis/services
COPY docker/dockerentrypoint.sh /synaxis/dockerentrypoint.sh
ENTRYPOINT ["/synaxis/dockerentrypoint.sh"]
CMD ["./bin/x3", "-f"]

EXPOSE 7702
