/* Output of the Opus 1.4 configure script under devkitARM, minus the autotools
   boilerplate. Options: --enable-fixed-point --disable-float-api
   --disable-rtcd --disable-intrinsics */

#ifndef OPUS_VENDORED_CONFIG_H
#define OPUS_VENDORED_CONFIG_H

#define DISABLE_FLOAT_API 1
#define ENABLE_HARDENING 1
#define FIXED_POINT 1
#define HAVE_LRINT 1
#define HAVE_LRINTF 1
#define OPUS_ARM_ASM
#define OPUS_ARM_INLINE_ASM 1
#define OPUS_ARM_INLINE_EDSP 1
#define OPUS_ARM_INLINE_MEDIA 1
#define OPUS_ARM_MAY_HAVE_EDSP 1
#define OPUS_ARM_MAY_HAVE_MEDIA 1
#define OPUS_ARM_PRESUME_EDSP 1
#define OPUS_ARM_PRESUME_MEDIA 1
#define OPUS_BUILD
#define PACKAGE_VERSION "1.4"
#define VAR_ARRAYS 1
#define restrict __restrict

#endif
