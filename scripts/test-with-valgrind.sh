#!/bin/bash
# Run PHPT tests with Valgrind memory leak detection

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

cd "$PROJECT_ROOT"

echo "==================================="
echo "Running Tests with Valgrind"
echo "==================================="
echo ""
echo "Valgrind will detect:"
echo "  - Memory leaks"
echo "  - Invalid memory access"
echo "  - Use of uninitialized memory"
echo ""

# Check if extension is built
if [ ! -f "modules/http_server.so" ]; then
    echo "❌ Error: Extension not built. Run 'make' first."
    exit 1
fi

# Run tests with Valgrind
php run-tests.php \
    -m \
    -d extension_dir=modules \
    -d extension=http_server.so \
    tests/phpt/http1parser/ \
    "$@"

echo ""
echo "==================================="
echo "✅ Valgrind tests completed"
echo "==================================="
