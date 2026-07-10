# From-scratch build of PHP (true-async fork) + ext/async + the
# true_async_server extension, mirroring .github/workflows/build-linux.yml.
# Final image runs the Autobahn echo server (e2e/autobahn/server.php).
#
# Build context is the repo root:
#   docker build -f e2e/autobahn/Dockerfile -t trueasync-ws-echo .
#
# Native stack versions are pinned to the CI's known-good set.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive \
    OPENSSL_VERSION=3.5.0 \
    NGHTTP2_VERSION=1.59.0 \
    NGHTTP3_VERSION=1.15.0 \
    NGTCP2_VERSION=1.22.1 \
    LIBUV_VERSION=1.52.1 \
    PHP_SRC_BRANCH=true-async \
    PHP_ASYNC_BRANCH=main \
    PKG_CONFIG_PATH=/usr/local/lib/pkgconfig

RUN apt-get update && apt-get install -y --no-install-recommends \
        autoconf automake libtool autoconf-archive bison build-essential re2c pkg-config \
        libxml2-dev libsqlite3-dev libonig-dev zlib1g-dev \
        wget ca-certificates git cmake ninja-build \
    && rm -rf /var/lib/apt/lists/*

# ---- OpenSSL 3.5 (QUIC API) -------------------------------------------
RUN set -eux; \
    wget -q https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VERSION}/openssl-${OPENSSL_VERSION}.tar.gz; \
    tar -xzf openssl-${OPENSSL_VERSION}.tar.gz; cd openssl-${OPENSSL_VERSION}; \
    ./Configure --prefix=/usr/local --libdir=lib --openssldir=/etc/ssl enable-tls1_3 shared; \
    make -j"$(nproc)"; make install_sw; cd /; rm -rf openssl-${OPENSSL_VERSION}*; ldconfig

# ---- nghttp2 ----------------------------------------------------------
RUN set -eux; \
    git clone --depth=1 --branch=v${NGHTTP2_VERSION} https://github.com/nghttp2/nghttp2.git; \
    cd nghttp2; autoreconf -i; ./configure --prefix=/usr/local --enable-lib-only; \
    make -j"$(nproc)"; make install; cd /; rm -rf nghttp2; ldconfig

# ---- nghttp3 ----------------------------------------------------------
RUN set -eux; \
    git clone --depth=1 --branch=v${NGHTTP3_VERSION} --recurse-submodules https://github.com/ngtcp2/nghttp3.git; \
    cd nghttp3; autoreconf -i; ./configure --prefix=/usr/local --enable-lib-only; \
    make -j"$(nproc)"; make install; cd /; rm -rf nghttp3; ldconfig

# ---- ngtcp2 (+ crypto_ossl, which http_server's config.m4 wants) ------
RUN set -eux; \
    git clone --depth=1 --branch=v${NGTCP2_VERSION} --recurse-submodules https://github.com/ngtcp2/ngtcp2.git; \
    cd ngtcp2; autoreconf -i; \
    ./configure --prefix=/usr/local --with-openssl --enable-lib-only; \
    make -j"$(nproc)"; make install; cd /; rm -rf ngtcp2; ldconfig

# ---- libuv ------------------------------------------------------------
RUN set -eux; \
    git clone --depth=1 --branch=v${LIBUV_VERSION} https://github.com/libuv/libuv.git; \
    cd libuv; mkdir build; cd build; \
    cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF; \
    ninja; ninja install; cd /; rm -rf libuv; ldconfig

# ---- PHP (true-async fork) + ext/async --------------------------------
RUN set -eux; \
    git clone --depth=1 --branch=${PHP_SRC_BRANCH} https://github.com/true-async/php-src.git php-src; \
    git clone --depth=1 --branch=${PHP_ASYNC_BRANCH} https://github.com/true-async/php-async.git php-src/ext/async

RUN set -eux; cd php-src; \
    ./buildconf --force; \
    ./configure \
        --prefix=/usr/local \
        --disable-all --without-pear \
        --enable-async \
        --enable-pdo --with-pdo-sqlite \
        --enable-sockets --enable-posix --enable-pcntl --enable-opcache \
        --with-openssl \
        --with-config-file-path=/etc \
        --with-config-file-scan-dir=/etc/php.d \
        --disable-debug --enable-zts; \
    make -j"$(nproc)" >/dev/null; make install; \
    mkdir -p /etc/php.d; echo "opcache.enable_cli=1" > /etc/php.d/opcache.ini; \
    ldconfig

# ---- true_async_server extension (cloned from git — already pushed) ---
ARG SERVER_REPO=https://github.com/true-async/server.git
ARG SERVER_REF=2-websocket-support-rfc-6455
# fetch FETCH_HEAD form accepts a branch OR a commit SHA (CI passes the
# PR head sha), and a changed SERVER_REF busts this layer's cache.
RUN set -eux; mkdir -p /http-server; cd /http-server; \
    git init -q; git remote add origin ${SERVER_REPO}; \
    git fetch --depth=1 origin ${SERVER_REF}; \
    git checkout -q FETCH_HEAD
RUN set -eux; cd /http-server; \
    /usr/local/bin/phpize; \
    ./configure \
        --with-php-config=/usr/local/bin/php-config \
        --enable-http-server --enable-http2 --enable-http3 --enable-websocket \
        --with-openssl --with-nghttp2=/usr/local \
        --with-nghttp3=/usr/local --with-ngtcp2=/usr/local; \
    make -j"$(nproc)"; make install; \
    echo "extension=true_async_server.so" > /etc/php.d/true_async_server.ini

EXPOSE 9001
CMD ["/usr/local/bin/php", "/http-server/e2e/autobahn/server.php", "9001"]
