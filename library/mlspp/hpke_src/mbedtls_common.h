#pragma once

#include <hpke/hpke.h>
#include <memory>
#include <namespace.h>
#include <stdexcept>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>

namespace MLS_NAMESPACE::hpke {

// Kept so files shared with the OpenSSL backend still compile; nothing in the
// mbedtls path needs a custom deleter.
template<typename T>
void
typed_delete(T* ptr);

template<typename T>
using typed_unique_ptr = std::unique_ptr<T, decltype(&typed_delete<T>)>;

template<typename T>
typed_unique_ptr<T>
make_typed_unique(T* ptr)
{
  return typed_unique_ptr<T>(ptr, typed_delete<T>);
}

std::runtime_error
openssl_error();

std::runtime_error
mbedtls_error(int rc, const char* where);

// One process-wide DRBG; mbedtls contexts are not thread-safe, so callers must
// hold the lock for the duration of a call that consumes randomness.
mbedtls_ctr_drbg_context*
drbg();

int
drbg_random(void* ctx, unsigned char* out, size_t len);

} // namespace MLS_NAMESPACE::hpke
