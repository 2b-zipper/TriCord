#ifndef RTC_BASE_SANITIZER_H_
#define RTC_BASE_SANITIZER_H_

#include <stddef.h>

// Nothing here runs under a sanitizer, so the annotations expand to nothing and
// the checks become no-ops. downsample_fast.c calls the check directly, so it
// has to exist as a real symbol rather than a macro.
#define RTC_NO_SANITIZE(what)

static inline void rtc_MsanCheckInitialized(const volatile void *ptr, size_t element_size, size_t num_elements) {
	(void)ptr;
	(void)element_size;
	(void)num_elements;
}

static inline void rtc_MsanMarkUninitialized(const volatile void *ptr, size_t element_size, size_t num_elements) {
	(void)ptr;
	(void)element_size;
	(void)num_elements;
}

#endif // RTC_BASE_SANITIZER_H_
