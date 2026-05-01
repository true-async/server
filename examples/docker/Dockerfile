# Multi-worker TrueAsync server smoke test, built on the official image.
#
# The base image ships php-true-async + true_async_server + ngtcp2/nghttp3
# libraries. We add curl/wrk/nc/gcc, build the h3client harness, drop the
# entrypoint and certs in, and run.
#
#   docker build -t tas-smoke -f examples/docker/Dockerfile .
#   docker run --rm -p 18080:8080 -p 18443:8443 -p 18443:8443/udp tas-smoke
#   docker run --rm tas-smoke /app/smoke-test.sh

FROM trueasync/php-true-async:0.7.0-alpha.2-php8.6

WORKDIR /app

RUN apt-get update && apt-get install -y --no-install-recommends \
        curl wrk bash netcat-openbsd nghttp2-client \
        gcc libc6-dev pkg-config libssl-dev \
    && rm -rf /var/lib/apt/lists/*

COPY examples/multi-worker.php /app/multi-worker.php
COPY examples/docker/smoke-test.sh /app/smoke-test.sh
COPY examples/certs/ /certs/
COPY tests/h3client/h3client.c /tmp/h3client.c

# Build h3client against the base image's ngtcp2/nghttp3 + ossl_crypto.
RUN gcc /tmp/h3client.c -o /app/h3client \
        $(pkg-config --cflags --libs libngtcp2 libnghttp3 openssl) \
        -lngtcp2_crypto_ossl \
    && rm /tmp/h3client.c \
    && chmod +x /app/smoke-test.sh

# Long-running CLI server -> opcache + JIT.
RUN { \
      echo 'opcache.enable=1'; \
      echo 'opcache.enable_cli=1'; \
      echo 'opcache.jit=1255'; \
      echo 'opcache.jit_buffer_size=128M'; \
      echo 'opcache.memory_consumption=256'; \
      echo 'opcache.validate_timestamps=0'; \
      echo 'memory_limit=512M'; \
    } > /etc/php.d/99-benchmark.ini

ENV PORT=8080 TLS_PORT=8443 H3_PORT=8443 \
    TLS_CERT=/certs/server.crt TLS_KEY=/certs/server.key \
    WORKERS=0
EXPOSE 8080 8443 8443/udp

CMD ["php", "/app/multi-worker.php"]
