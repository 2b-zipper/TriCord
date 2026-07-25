#include "mbedtls_common.h"

#include <cstring>
#include <mutex>
#include <string>

#if defined(__3DS__)
#include <3ds.h>
#endif

namespace MLS_NAMESPACE::hpke {

std::runtime_error
openssl_error()
{
  return std::runtime_error("mbedtls error");
}

std::runtime_error
mbedtls_error(int rc, const char* where)
{
  return std::runtime_error(std::string(where) + " failed: " +
                            std::to_string(rc));
}

static std::mutex&
drbg_mutex()
{
  static std::mutex instance;
  return instance;
}

#if defined(__3DS__)
// The 3DS mbedtls build does not agree with its installed headers about where
// entropy comes from, so seed straight from the console's PS service instead of
// mbedtls_entropy_func. MLS key generation depends on this being real
// randomness, not a fallback.
static int
platform_entropy(void* /* ctx */,
                 unsigned char* out,
                 size_t len,
                 size_t* olen)
{
  if (R_FAILED(PS_GenerateRandomBytes(out, len))) {
    return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
  }
  *olen = len;
  return 0;
}
#endif

mbedtls_ctr_drbg_context*
drbg()
{
  static mbedtls_entropy_context entropy;
  static mbedtls_ctr_drbg_context ctr_drbg;
  static bool ready = false;

  if (!ready) {
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);

#if defined(__3DS__)
    const auto add_rc = mbedtls_entropy_add_source(&entropy,
                                                   platform_entropy,
                                                   nullptr,
                                                   MBEDTLS_ENTROPY_MAX_GATHER,
                                                   MBEDTLS_ENTROPY_SOURCE_STRONG);
    if (add_rc != 0) {
      throw mbedtls_error(add_rc, "entropy_add_source");
    }
#endif

    static const char* pers = "mlspp-mbedtls";
    const auto rc = mbedtls_ctr_drbg_seed(
      &ctr_drbg,
      mbedtls_entropy_func,
      &entropy,
      reinterpret_cast<const unsigned char*>(pers),
      strlen(pers));
    if (rc != 0) {
      throw mbedtls_error(rc, "ctr_drbg_seed");
    }
    ready = true;
  }

  return &ctr_drbg;
}

int
drbg_random(void* ctx, unsigned char* out, size_t len)
{
  (void)ctx;
  const std::lock_guard<std::mutex> lock(drbg_mutex());
  return mbedtls_ctr_drbg_random(drbg(), out, len);
}

} // namespace MLS_NAMESPACE::hpke
