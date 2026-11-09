/* Minimal assertion helpers for CTest unit tests. */
#ifndef SIL_TEST_H
#define SIL_TEST_H

#include <stdio.h>

static int sil_test_failures;
static int sil_test_checks;

#define CHECK(cond)                                                                     \
	do {                                                                            \
		sil_test_checks++;                                                      \
		if (!(cond)) {                                                          \
			sil_test_failures++;                                            \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
		}                                                                       \
	} while (0)

#define CHECK_EQ_INT(a, b)                                                              \
	do {                                                                            \
		long long va_ = (long long)(a), vb_ = (long long)(b);                   \
		sil_test_checks++;                                                      \
		if (va_ != vb_) {                                                       \
			sil_test_failures++;                                            \
			fprintf(stderr, "%s:%d: %s == %s failed: %lld != %lld\n", __FILE__, __LINE__, \
				#a, #b, va_, vb_);                                      \
		}                                                                       \
	} while (0)

#define TEST_REPORT()                                                                   \
	(printf("%d checks, %d failures\n", sil_test_checks, sil_test_failures), sil_test_failures ? 1 : 0)

#endif
