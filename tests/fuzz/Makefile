# Fuzz harness build rules — Step 10.
#
# Run from the repo root:
#   make -C tests/fuzz
#
# Then either short smoke (default 30s) or the 24h ship gate:
#   ./tests/fuzz/h3_datagram_fuzz -max_total_time=30
#   ./tests/fuzz/h3_datagram_fuzz -max_total_time=86400 -jobs=$(nproc)

CC      ?= clang
CFLAGS  ?= -g -O1 -fsanitize=fuzzer,address,undefined
INCLUDE  = -I/usr/local/include
LIBPATH  = -L/usr/local/lib -Wl,-rpath,/usr/local/lib
LIBS     = -lngtcp2 -lngtcp2_crypto_ossl -lssl -lcrypto

TARGETS  = h3_datagram_fuzz

.PHONY: all clean smoke

all: $(TARGETS)

h3_datagram_fuzz: h3_datagram_fuzz.c
	$(CC) $(CFLAGS) $(INCLUDE) $(LIBPATH) -o $@ $< $(LIBS)

smoke: h3_datagram_fuzz
	./h3_datagram_fuzz -max_total_time=30 -print_pcs=0

clean:
	rm -f $(TARGETS) crash-* leak-* slow-unit-* default*.profraw
