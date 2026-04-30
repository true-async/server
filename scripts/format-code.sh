#!/bin/bash
# Format all C source files with clang-format

set -e

COLOR_GREEN='\033[0;32m'
COLOR_YELLOW='\033[1;33m'
COLOR_RESET='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

echo "Formatting code with clang-format..."
echo

# Check if clang-format is installed
if ! command -v clang-format &> /dev/null; then
	echo -e "${COLOR_YELLOW}⚠ clang-format not found${COLOR_RESET}"
	echo "  Install: sudo apt-get install clang-format"
	exit 1
fi

# Find all C source and header files
C_FILES=$(find "$PROJECT_DIR/src" "$PROJECT_DIR/include" -type f \( -name "*.c" -o -name "*.h" \) 2>/dev/null || true)

if [ -z "$C_FILES" ]; then
	echo -e "${COLOR_YELLOW}⚠ No C files found to format${COLOR_RESET}"
	exit 0
fi

FORMATTED=0

for file in $C_FILES; do
	echo "Formatting: $file"
	clang-format -i "$file"
	FORMATTED=$((FORMATTED + 1))
done

echo
echo -e "${COLOR_GREEN}✓ Formatted $FORMATTED file(s)${COLOR_RESET}"
