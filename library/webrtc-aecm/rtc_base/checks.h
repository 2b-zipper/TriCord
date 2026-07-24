#ifndef RTC_BASE_CHECKS_H_
#define RTC_BASE_CHECKS_H_

// Stand-in for WebRTC's checks.h. The real one drags in WebRTC's logging and
// abseil; AECM only uses the DCHECK family, which upstream compiles out in
// release builds anyway. sizeof keeps the expression "used" so that variables
// referenced only by a check do not warn.

#define RTC_UNUSED_EXPR_(expr) \
	do {                       \
		(void)sizeof(expr);    \
	} while (0)

#define RTC_CHECK(condition) RTC_UNUSED_EXPR_(condition)
#define RTC_CHECK_EQ(a, b) RTC_UNUSED_EXPR_((a) == (b))
#define RTC_CHECK_NE(a, b) RTC_UNUSED_EXPR_((a) != (b))
#define RTC_CHECK_LE(a, b) RTC_UNUSED_EXPR_((a) <= (b))
#define RTC_CHECK_LT(a, b) RTC_UNUSED_EXPR_((a) < (b))
#define RTC_CHECK_GE(a, b) RTC_UNUSED_EXPR_((a) >= (b))
#define RTC_CHECK_GT(a, b) RTC_UNUSED_EXPR_((a) > (b))

#define RTC_DCHECK(condition) RTC_UNUSED_EXPR_(condition)
#define RTC_DCHECK_EQ(a, b) RTC_UNUSED_EXPR_((a) == (b))
#define RTC_DCHECK_NE(a, b) RTC_UNUSED_EXPR_((a) != (b))
#define RTC_DCHECK_LE(a, b) RTC_UNUSED_EXPR_((a) <= (b))
#define RTC_DCHECK_LT(a, b) RTC_UNUSED_EXPR_((a) < (b))
#define RTC_DCHECK_GE(a, b) RTC_UNUSED_EXPR_((a) >= (b))
#define RTC_DCHECK_GT(a, b) RTC_UNUSED_EXPR_((a) > (b))

#endif // RTC_BASE_CHECKS_H_
