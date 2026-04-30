# CMocka Unit Tests Makefile
# Copyright (c) TrueAsync

CC = gcc
CFLAGS = -g -O0 -Wall -Wextra -std=gnu11 -fPIC
DEFINES = -D_GNU_SOURCE -DHAVE_LLHTTP=1
INCLUDES = -I../../include -I../../include/http1 -I../../deps/llhttp
LDFLAGS = -lcmocka
PHP_CFLAGS = $(shell php-config --includes)
PHP_LDFLAGS = -L$(shell php-config --prefix)/lib -lphp $(shell php-config --libs)
PHP_RPATH = -Wl,-rpath,$(shell php-config --prefix)/lib

# Source files to test
SRC_DIR = ../../src
HTTP1_SRC = $(SRC_DIR)/http1/http_parser.c
LLHTTP_SRC = ../../deps/llhttp/llhttp.c ../../deps/llhttp/api.c ../../deps/llhttp/http.c
TEST_HELPERS = php_runtime_init.c

# Test files
TEST_SOURCES = test_http_parser.c
TEST_BINS = $(TEST_SOURCES:.c=)

# Combined flags
ALL_CFLAGS = $(CFLAGS) $(DEFINES) $(INCLUDES) $(PHP_CFLAGS)
ALL_LDFLAGS = $(PHP_LDFLAGS) $(LDFLAGS) $(PHP_RPATH)

.PHONY: all clean run test

all: $(TEST_BINS)

# Pattern rule for building test binaries
%: %.c
	@echo "Building $@..."
	$(CC) $(ALL_CFLAGS) -o $@ $< $(HTTP1_SRC) $(LLHTTP_SRC) $(TEST_HELPERS) $(ALL_LDFLAGS)

# Run all tests
test: all
	@echo "===================="
	@echo "Running Unit Tests"
	@echo "===================="
	@for test in $(TEST_BINS); do \
		echo ""; \
		echo "Running $$test..."; \
		./$$test || exit 1; \
	done
	@echo ""
	@echo "===================="
	@echo "✅ All tests passed"
	@echo "===================="

# Alias for test
run: test

# Clean build artifacts
clean:
	rm -f $(TEST_BINS)
	rm -f *.o
	rm -f core
	rm -f vgcore.*

# Help
help:
	@echo "CMocka Unit Tests"
	@echo ""
	@echo "Targets:"
	@echo "  all    - Build all tests"
	@echo "  test   - Build and run all tests"
	@echo "  clean  - Remove build artifacts"
	@echo "  help   - Show this help"
	@echo ""
	@echo "Tests:"
	@for test in $(TEST_SOURCES); do \
		echo "  - $$test"; \
	done
