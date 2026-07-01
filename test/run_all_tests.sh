#!/bin/sh
#
# Master test runner for pcompress.
#
# Runs all test categories and produces a summary report.
#
# Usage:
#   sh run_all_tests.sh [category]
#
# Categories:
#   unit         - C unit tests (fast)
#   integration  - Shell-based integration tests
#   legacy       - Original t1-t9 test suite
#   bench        - Performance benchmarks
#   fuzz         - Quick fuzz smoke test (60s per target)
#   all          - Everything except benchmarks and fuzz (default)
#   full         - Everything including benchmarks
#

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
BUILD_DIR="${SCRIPT_DIR}/.."
category="${1:-all}"

total_pass=0
total_fail=0
total_skip=0

run_section() {
	name="$1"
	shift
	echo ""
	echo "================================================================="
	echo " $name"
	echo "================================================================="
	"$@"
	rv=$?
	if [ $rv -eq 0 ]; then
		echo "${name}: PASSED"
		total_pass=$((total_pass + 1))
	else
		echo "${name}: FAILED"
		total_fail=$((total_fail + 1))
	fi
	return $rv
}

# --- Unit Tests ---
run_unit_tests() {
	if [ -x "${BUILD_DIR}/buildtmp/test_compress" ]; then
		run_section "Unit: Compression Round-Trip" \
		    "${BUILD_DIR}/buildtmp/test_compress"
	else
		echo "SKIP: test_compress not built (run 'make test-unit-build')"
		total_skip=$((total_skip + 1))
	fi

	if [ -x "${BUILD_DIR}/buildtmp/test_xxhash" ]; then
		run_section "Unit: xxHash" \
		    "${BUILD_DIR}/buildtmp/test_xxhash"
	else
		echo "SKIP: test_xxhash not built"
		total_skip=$((total_skip + 1))
	fi

	if [ -x "${BUILD_DIR}/buildtmp/test_crc" ]; then
		run_section "Unit: CRC32/CRC64" \
		    "${BUILD_DIR}/buildtmp/test_crc"
	else
		echo "SKIP: test_crc not built"
		total_skip=$((total_skip + 1))
	fi

	if [ -x "${BUILD_DIR}/buildtmp/test_slab" ]; then
		run_section "Unit: Slab Allocator" \
		    "${BUILD_DIR}/buildtmp/test_slab"
	else
		echo "SKIP: test_slab not built"
		total_skip=$((total_skip + 1))
	fi

	if [ -x "${BUILD_DIR}/buildtmp/test_blake2" ]; then
		run_section "Unit: BLAKE2b Hashing" \
		    "${BUILD_DIR}/buildtmp/test_blake2"
	else
		echo "SKIP: test_blake2 not built"
		total_skip=$((total_skip + 1))
	fi
}

# --- Integration Tests ---
run_integration_tests() {
	cd "${SCRIPT_DIR}"
	for test_script in integration/test_*.sh; do
		if [ -f "$test_script" ]; then
			tname=$(basename "$test_script" .sh)
			run_section "Integration: ${tname}" sh "$test_script"
		fi
	done
	cd "${SCRIPT_DIR}"
}

# --- Legacy Tests (original t1-t9) ---
run_legacy_tests() {
	cd "${SCRIPT_DIR}"
	run_section "Legacy: Original test suite" sh run_test.sh
	cd "${SCRIPT_DIR}"
}

# --- Benchmarks ---
run_benchmarks() {
	if [ -x "${BUILD_DIR}/buildtmp/bench_algos" ]; then
		run_section "Benchmark: Algorithm Comparison" \
		    "${BUILD_DIR}/buildtmp/bench_algos" 5
	else
		echo "SKIP: bench_algos not built (run 'make bench-build')"
		total_skip=$((total_skip + 1))
	fi
}

# --- Fuzz Smoke Test ---
run_fuzz_smoke() {
	fuzz_time="${FUZZ_SECONDS:-60}"
	for target in fuzz_decompress fuzz_compress fuzz_crc; do
		if [ -x "${BUILD_DIR}/buildtmp/${target}" ]; then
			run_section "Fuzz: ${target} (${fuzz_time}s)" \
			    "${BUILD_DIR}/buildtmp/${target}" \
			    "-max_total_time=${fuzz_time}" "-print_final_stats=1"
		else
			echo "SKIP: ${target} not built (run 'make fuzz-build')"
			total_skip=$((total_skip + 1))
		fi
	done
}

# --- Dispatch ---
case "$category" in
	unit)
		run_unit_tests
		;;
	integration)
		run_integration_tests
		;;
	legacy)
		run_legacy_tests
		;;
	bench)
		run_benchmarks
		;;
	fuzz)
		run_fuzz_smoke
		;;
	all)
		run_unit_tests
		run_integration_tests
		;;
	full)
		run_unit_tests
		run_integration_tests
		run_legacy_tests
		run_benchmarks
		;;
	*)
		echo "Unknown category: $category"
		echo "Usage: $0 [unit|integration|legacy|bench|fuzz|all|full]"
		exit 1
		;;
esac

# --- Summary ---
echo ""
echo "================================================================="
echo " SUMMARY"
echo "================================================================="
echo " Sections passed:  ${total_pass}"
echo " Sections failed:  ${total_fail}"
echo " Sections skipped: ${total_skip}"
echo "================================================================="

if [ $total_fail -gt 0 ]; then
	exit 1
fi
exit 0
