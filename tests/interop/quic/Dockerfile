# QUIC interop-runner endpoint for the TrueAsync HTTP/3 server.
#
# Layers the published TrueAsync runtime (PHP 8.6 + OpenSSL 3.5 + nghttp3 +
# ngtcp2 + true_async_server.so, all under /usr/local) onto the
# quic-network-simulator endpoint base, which provides the netns/route setup the
# runner drives. Both are Ubuntu 24.04, so /usr/local copies cleanly.
#
#   docker build -t trueasync/quic-interop-endpoint:latest \
#     --build-arg TAS_IMAGE=trueasync/php-true-async:0.7.2-php8.6 \
#     -f tests/interop/quic/Dockerfile tests/interop/quic
#
# Then add the implementations.json entry (see README.md) and run the suite from
# a checkout of https://github.com/quic-interop/quic-interop-runner.

ARG TAS_IMAGE=trueasync/php-true-async:0.7.2-php8.6

FROM ${TAS_IMAGE} AS tas

FROM martenseemann/quic-network-simulator-endpoint:latest

ENV DEBIAN_FRONTEND=noninteractive

# System runtime libs PHP links against (the COPY below only brings /usr/local).
# Same set as the proven release runtime stage (releases/docker/Dockerfile.debian).
RUN apt-get update && apt-get install -y --no-install-recommends \
        libxml2 libargon2-1 libedit2 libreadline8 \
        libsodium23 libsqlite3-0 libonig5 libzip4 \
        libpng16-16 libjpeg8 libwebp7 libfreetype6 \
        libgmp10 libldap2 libsasl2-2 libpq5 \
        libmysqlclient21 libbz2-1.0 libenchant-2-2 \
        libffi8 libgdbm6 liblmdb0 libsnmp40 \
        libtidy5deb1 libxslt1.1 libicu74 ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=tas /usr/local /usr/local
COPY --from=tas /etc/php.d  /etc/php.d

RUN ldconfig && php -v && php -m | grep -i true_async_server

COPY entry.php        /interop/entry.php
COPY run_endpoint.sh  /run_endpoint.sh
RUN chmod +x /run_endpoint.sh
