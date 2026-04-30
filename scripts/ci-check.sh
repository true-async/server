#!/bin/bash
# Comprehensive CI check before commit/push
# Runs all checks: style, build, tests, memory

set -e

COLOR_RED='\033[0;31m'
COLOR_GREEN='\033[0;32m'
COLOR_YELLOW='\033[1;33m'
COLOR_BLUE='\033[0;34m'
COLOR_RESET='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

FAILED_CHECKS=0

echo -e "${COLOR_BLUE}═══════════════════════════════════════${COLOR_RESET}"
echo -e "${COLOR_BLUE}    CI Check - Pre-commit Validation${COLOR_RESET}"
echo -e "${COLOR_BLUE}═══════════════════════════════════════${COLOR_RESET}"
echo

# Check 1: Code Style
echo -e "${COLOR_BLUE}[1/6] Checking code style...${COLOR_RESET}"
if "$SCRIPT_DIR/check-style.sh"; then
	echo -e "${COLOR_GREEN}✓ Code style check passed${COLOR_RESET}"
else
	echo -e "${COLOR_RED}✗ Code style check failed${COLOR_RESET}"
	FAILED_CHECKS=$((FAILED_CHECKS + 1))
fi
echo

# Check 2: Static Analysis (cppcheck)
echo -e "${COLOR_BLUE}[2/6] Running static analysis...${COLOR_RESET}"
if command -v cppcheck &> /dev/null; then
	if [ -d "$PROJECT_DIR/src" ]; then
		if cppcheck --enable=warning,style,performance,portability \
		            --error-exitcode=1 \
		            --quiet \
		            "$PROJECT_DIR/src" 2>&1 | grep -v "^Checking"; then
			echo -e "${COLOR_RED}✗ Static analysis found issues${COLOR_RESET}"
			FAILED_CHECKS=$((FAILED_CHECKS + 1))
		else
			echo -e "${COLOR_GREEN}✓ Static analysis passed${COLOR_RESET}"
		fi
	else
		echo -e "${COLOR_YELLOW}⚠ No src/ directory found, skipping${COLOR_RESET}"
	fi
else
	echo -e "${COLOR_YELLOW}⚠ cppcheck not installed, skipping${COLOR_RESET}"
	echo "  Install: sudo apt-get install cppcheck"
fi
echo

# Check 3: Build
echo -e "${COLOR_BLUE}[3/6] Building project...${COLOR_RESET}"
if [ -f "$PROJECT_DIR/Makefile" ]; then
	cd "$PROJECT_DIR"
	if make clean && make; then
		echo -e "${COLOR_GREEN}✓ Build successful${COLOR_RESET}"
	else
		echo -e "${COLOR_RED}✗ Build failed${COLOR_RESET}"
		FAILED_CHECKS=$((FAILED_CHECKS + 1))
	fi
else
	echo -e "${COLOR_YELLOW}⚠ No Makefile found, skipping build${COLOR_RESET}"
fi
echo

# Check 4: Unit Tests
echo -e "${COLOR_BLUE}[4/6] Running unit tests...${COLOR_RESET}"
if [ -f "$PROJECT_DIR/Makefile" ] && grep -q "test-unit" "$PROJECT_DIR/Makefile" 2>/dev/null; then
	cd "$PROJECT_DIR"
	if make test-unit; then
		echo -e "${COLOR_GREEN}✓ Unit tests passed${COLOR_RESET}"
	else
		echo -e "${COLOR_RED}✗ Unit tests failed${COLOR_RESET}"
		FAILED_CHECKS=$((FAILED_CHECKS + 1))
	fi
else
	echo -e "${COLOR_YELLOW}⚠ No unit tests found, skipping${COLOR_RESET}"
fi
echo

# Check 5: PHPT Tests
echo -e "${COLOR_BLUE}[5/6] Running PHPT tests...${COLOR_RESET}"
if [ -d "$PROJECT_DIR/tests/phpt" ] && [ -n "$(ls -A "$PROJECT_DIR/tests/phpt"/*.phpt 2>/dev/null)" ]; then
	cd "$PROJECT_DIR"
	if make test-phpt 2>/dev/null || php run-tests.php tests/phpt/ 2>/dev/null; then
		echo -e "${COLOR_GREEN}✓ PHPT tests passed${COLOR_RESET}"
	else
		echo -e "${COLOR_RED}✗ PHPT tests failed${COLOR_RESET}"
		FAILED_CHECKS=$((FAILED_CHECKS + 1))
	fi
else
	echo -e "${COLOR_YELLOW}⚠ No PHPT tests found, skipping${COLOR_RESET}"
fi
echo

# Check 6: Memory Check (Valgrind)
echo -e "${COLOR_BLUE}[6/6] Running memory check...${COLOR_RESET}"
if command -v valgrind &> /dev/null; then
	if [ -f "$PROJECT_DIR/Makefile" ] && grep -q "test-valgrind" "$PROJECT_DIR/Makefile" 2>/dev/null; then
		cd "$PROJECT_DIR"
		if make test-valgrind; then
			echo -e "${COLOR_GREEN}✓ Memory check passed (0 leaks)${COLOR_RESET}"
		else
			echo -e "${COLOR_RED}✗ Memory check failed (leaks detected)${COLOR_RESET}"
			FAILED_CHECKS=$((FAILED_CHECKS + 1))
		fi
	else
		echo -e "${COLOR_YELLOW}⚠ No valgrind target in Makefile, skipping${COLOR_RESET}"
	fi
else
	echo -e "${COLOR_YELLOW}⚠ valgrind not installed, skipping${COLOR_RESET}"
	echo "  Install: sudo apt-get install valgrind"
fi
echo

# Summary
echo -e "${COLOR_BLUE}═══════════════════════════════════════${COLOR_RESET}"
if [ $FAILED_CHECKS -eq 0 ]; then
	echo -e "${COLOR_GREEN}✓ All checks passed! Ready to commit.${COLOR_RESET}"
	echo -e "${COLOR_BLUE}═══════════════════════════════════════${COLOR_RESET}"
	exit 0
else
	echo -e "${COLOR_RED}✗ $FAILED_CHECKS check(s) failed!${COLOR_RESET}"
	echo -e "${COLOR_RED}  Please fix the issues before committing.${COLOR_RESET}"
	echo -e "${COLOR_BLUE}═══════════════════════════════════════${COLOR_RESET}"
	exit 1
fi
