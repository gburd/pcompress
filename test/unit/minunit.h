/*
 * MinUnit - Minimal unit testing framework for C.
 * Based on http://www.jera.com/techinfo/jtns/jtn002.html
 * Extended with test suite support, setup/teardown, and reporting.
 *
 * Usage:
 *   MU_TEST(test_name) { mu_assert(...); }
 *   MU_TEST_SUITE(suite_name) { MU_RUN_TEST(test_name); }
 *   int main() { MU_RUN_SUITE(suite_name); MU_REPORT(); return MU_EXIT_CODE; }
 */

#ifndef MINUNIT_H
#define MINUNIT_H

#include <stdio.h>
#include <string.h>

static int minunit_run = 0;
static int minunit_fail = 0;
static int minunit_status = 0;
static char minunit_last_message[1024];

#define MU_TEST(name) static void name(void)

#define MU_TEST_SUITE(name) static void name(void)

#define mu_assert(test, message) \
	do { \
		if (!(test)) { \
			snprintf(minunit_last_message, sizeof(minunit_last_message), \
			    "%s:%d: %s", __FILE__, __LINE__, message); \
			minunit_status = 1; \
			return; \
		} \
	} while (0)

#define mu_assert_int_eq(expected, result) \
	do { \
		int _exp = (expected); \
		int _res = (result); \
		if (_exp != _res) { \
			snprintf(minunit_last_message, sizeof(minunit_last_message), \
			    "%s:%d: expected %d but got %d", \
			    __FILE__, __LINE__, _exp, _res); \
			minunit_status = 1; \
			return; \
		} \
	} while (0)

#define mu_assert_uint64_eq(expected, result) \
	do { \
		uint64_t _exp = (expected); \
		uint64_t _res = (result); \
		if (_exp != _res) { \
			snprintf(minunit_last_message, sizeof(minunit_last_message), \
			    "%s:%d: expected %llu but got %llu", \
			    __FILE__, __LINE__, \
			    (unsigned long long)_exp, (unsigned long long)_res); \
			minunit_status = 1; \
			return; \
		} \
	} while (0)

#define mu_assert_mem_eq(expected, result, size) \
	do { \
		if (memcmp((expected), (result), (size)) != 0) { \
			snprintf(minunit_last_message, sizeof(minunit_last_message), \
			    "%s:%d: memory comparison failed (%zu bytes)", \
			    __FILE__, __LINE__, (size_t)(size)); \
			minunit_status = 1; \
			return; \
		} \
	} while (0)

#define MU_RUN_TEST(test) \
	do { \
		minunit_status = 0; \
		test(); \
		minunit_run++; \
		if (minunit_status) { \
			minunit_fail++; \
			printf("  FAIL: %s\n", minunit_last_message); \
		} else { \
			printf("  PASS: %s\n", #test); \
		} \
	} while (0)

#define MU_RUN_SUITE(suite) \
	do { \
		printf("--- %s ---\n", #suite); \
		suite(); \
	} while (0)

#define MU_REPORT() \
	do { \
		printf("\n%d tests, %d failures\n", minunit_run, minunit_fail); \
	} while (0)

#define MU_EXIT_CODE (minunit_fail != 0)

#endif /* MINUNIT_H */
