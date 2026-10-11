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
#   matrix       - Full feature round-trip matrix (all algorithms/levels)
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
	for t in test_compress test_xxhash test_crc test_slab test_blake2 test_sha2 test_api test_filters test_crypto test_algos; do
		if [ -x "${BUILD_DIR}/buildtmp/$t" ]; then
			run_section "Unit: $t" "${BUILD_DIR}/buildtmp/$t"
		else
			echo "FAIL: $t not built (run 'make test-unit-build')"
			total_fail=$((total_fail + 1))
		fi
	done
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

# --- Feature matrix (every CLI feature, round-trip verified) ---
run_feature_matrix() {
	run_section "Feature matrix" sh "${SCRIPT_DIR}/feature_matrix.sh" \
	    "${BUILD_DIR}/pcompress" "" ${MATRIX_MODE:-quick}
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
	matrix)
		MATRIX_MODE=full run_feature_matrix
		;;
	all)
		run_unit_tests
		run_integration_tests
		run_feature_matrix
		;;
	full)
		run_unit_tests
		run_integration_tests
		MATRIX_MODE=full run_feature_matrix
		run_legacy_tests
		run_benchmarks
		;;
	*)
		echo "Unknown category: $category"
		echo "Usage: $0 [unit|integration|matrix|legacy|bench|fuzz|all|full]"
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
