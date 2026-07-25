#include <hpke/random.h>
#include <namespace.h>

#include "mbedtls_common.h"

namespace MLS_NAMESPACE::hpke {

bytes
random_bytes(size_t size)
{
  auto rand = bytes(size);
  const auto rc = drbg_random(nullptr, rand.data(), size);
  if (rc != 0) {
    throw mbedtls_error(rc, "ctr_drbg_random");
  }
  return rand;
}

} // namespace MLS_NAMESPACE::hpke
