#ifndef RTC_BASE_SYSTEM_ARCH_H_
#define RTC_BASE_SYSTEM_ARCH_H_

// Trimmed arch detection. The 3DS is an ARM11 (ARMv6k): no NEON and not ARMv7,
// so WEBRTC_ARCH_ARM_V7 and WEBRTC_HAS_NEON are deliberately never defined and
// the portable C paths are taken. The host build used for testing lands in the
// x86 branch and takes the same C paths.

#if defined(__x86_64__) || defined(_M_X64)
#define WEBRTC_ARCH_X86_FAMILY
#define WEBRTC_ARCH_64_BITS
#define WEBRTC_ARCH_LITTLE_ENDIAN
#elif defined(__i386__) || defined(_M_IX86)
#define WEBRTC_ARCH_X86_FAMILY
#define WEBRTC_ARCH_32_BITS
#define WEBRTC_ARCH_LITTLE_ENDIAN
#elif defined(__ARMEL__) || defined(_M_ARM)
#define WEBRTC_ARCH_ARM_FAMILY
#define WEBRTC_ARCH_32_BITS
#define WEBRTC_ARCH_LITTLE_ENDIAN
#elif defined(__aarch64__)
#define WEBRTC_ARCH_ARM64_FAMILY
#define WEBRTC_ARCH_64_BITS
#define WEBRTC_ARCH_LITTLE_ENDIAN
#else
#error "Unsupported architecture for the trimmed WebRTC AECM build"
#endif

#endif // RTC_BASE_SYSTEM_ARCH_H_
