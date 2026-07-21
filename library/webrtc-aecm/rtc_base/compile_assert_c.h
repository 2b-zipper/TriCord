#ifndef RTC_BASE_COMPILE_ASSERT_C_H_
#define RTC_BASE_COMPILE_ASSERT_C_H_

// Upstream's definition: a duplicate case label fails to compile when the
// expression is false. Works in both C and C++.
#define RTC_COMPILE_ASSERT(expr) \
	switch (0) {                 \
	case 0:                      \
	case expr:;                  \
	}

#endif // RTC_BASE_COMPILE_ASSERT_C_H_
