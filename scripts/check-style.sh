#!/bin/bash
# Check code style compliance

set -e

COLOR_RED='\033[0;31m'
COLOR_GREEN='\033[0;32m'
COLOR_YELLOW='\033[1;33m'
COLOR_RESET='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

echo "Checking code style..."
echo

# Check if clang-format is installed
if ! command -v clang-format &> /dev/null; then
	echo -e "${COLOR_RED}✗ clang-format not found${COLOR_RESET}"
	echo "  Install: sudo apt-get install clang-format"
	exit 1
fi

# Find all C source and header files
C_FILES=$(find "$PROJECT_DIR/src" "$PROJECT_DIR/include" -type f \( -name "*.c" -o -name "*.h" \) 2>/dev/null || true)

if [ -z "$C_FILES" ]; then
	echo -e "${COLOR_YELLOW}⚠ No C files found to check${COLOR_RESET}"
	exit 0
fi

VIOLATIONS=0

echo "Checking files with clang-format..."
for file in $C_FILES; do
	# Check if file would be changed by clang-format
	if ! clang-format --dry-run --Werror "$file" 2>/dev/null; then
		echo -e "${COLOR_RED}✗ ${file}${COLOR_RESET}"
		echo "  Run: clang-format -i $file"
		VIOLATIONS=$((VIOLATIONS + 1))
	else
		echo -e "${COLOR_GREEN}✓ ${file}${COLOR_RESET}"
	fi
done

echo
if [ $VIOLATIONS -eq 0 ]; then
	echo -e "${COLOR_GREEN}✓ All files comply with code style${COLOR_RESET}"
	exit 0
else
	echo -e "${COLOR_RED}✗ Found $VIOLATIONS file(s) with style violations${COLOR_RESET}"
	echo
	echo "To fix all files automatically:"
	echo "  ./scripts/format-code.sh"
	exit 1
fi
