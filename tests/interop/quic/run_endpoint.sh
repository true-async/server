#!/bin/bash
# QUIC interop-runner endpoint hook (server role).
#
# The quic-network-simulator base image configures the network and then execs
# this script. Per the interface, an unsupported test case MUST exit 127 so new
# cases can be added without breaking existing implementations.
set -e

if [ "$ROLE" != "server" ]; then
    echo "true-async endpoint implements the server role only" >&2
    exit 127
fi

# Cases a correct HTTP/3 file server passes via downloaded-file integrity (the
# runner compares the bytes the client pulled from /www). Cases that need
# server-side packet-trace inspection (SSLKEYLOGFILE / qlog export — not wired
# yet) or special configuration (retry, resumption, zerortt, chacha20, keyupdate,
# ecn, v2, amplificationlimit, multiconnect) are screened out until supported.
case "$TESTCASE" in
    handshake|transfer|http3|multiplexing|longrtt|goodput|crosstraffic|transferloss|transfercorruption|blackhole|handshakeloss)
        ;;
    *)
        echo "unsupported test case: $TESTCASE" >&2
        exit 127
        ;;
esac

exec env \
    INTEROP_WWW=/www \
    INTEROP_CERT=/certs/cert.pem \
    INTEROP_KEY=/certs/priv.key \
    INTEROP_PORT=443 \
    WORKERS="${WORKERS:-1}" \
    php -d extension=true_async_server /interop/entry.php
