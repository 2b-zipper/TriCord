#ifndef RTC_BASE_NUMERICS_SAFE_CONVERSIONS_H_
#define RTC_BASE_NUMERICS_SAFE_CONVERSIONS_H_

#ifdef __cplusplus

#include <limits>

namespace webrtc {

// Upstream also asserts the value fits; the assert is compiled out in release
// builds, which is the only configuration this port ships.
template <typename Dst, typename Src> constexpr Dst dchecked_cast(Src value) {
	return static_cast<Dst>(value);
}

template <typename Dst, typename Src> constexpr Dst saturated_cast(Src value) {
	return value > static_cast<Src>(std::numeric_limits<Dst>::max())   ? std::numeric_limits<Dst>::max()
	       : value < static_cast<Src>(std::numeric_limits<Dst>::min()) ? std::numeric_limits<Dst>::min()
	                                                                  : static_cast<Dst>(value);
}

} // namespace webrtc

#endif // __cplusplus

#endif // RTC_BASE_NUMERICS_SAFE_CONVERSIONS_H_
