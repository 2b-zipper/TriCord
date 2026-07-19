#include "aead_cipher.h"
#include "mbedtls_common.h"

#include <namespace.h>

#include <mbedtls/chachapoly.h>
#include <mbedtls/gcm.h>

namespace MLS_NAMESPACE::hpke {

///
/// ExportOnlyCipher
///
bytes
ExportOnlyCipher::seal(const bytes& /* key */,
                       const bytes& /* nonce */,
                       const bytes& /* aad */,
                       const bytes& /* pt */) const
{
  throw std::runtime_error("seal() on export-only context");
}

std::optional<bytes>
ExportOnlyCipher::open(const bytes& /* key */,
                       const bytes& /* nonce */,
                       const bytes& /* aad */,
                       const bytes& /* ct */) const
{
  throw std::runtime_error("open() on export-only context");
}

ExportOnlyCipher::ExportOnlyCipher()
  : AEAD(AEAD::ID::export_only, 0, 0)
{
}

///
/// AEADCipher
///
AEADCipher
make_aead(AEAD::ID cipher_in)
{
  return { cipher_in };
}

template<>
const AEADCipher&
AEADCipher::get<AEAD::ID::AES_128_GCM>()
{
  static const auto instance = make_aead(AEAD::ID::AES_128_GCM);
  return instance;
}

template<>
const AEADCipher&
AEADCipher::get<AEAD::ID::AES_256_GCM>()
{
  static const auto instance = make_aead(AEAD::ID::AES_256_GCM);
  return instance;
}

template<>
const AEADCipher&
AEADCipher::get<AEAD::ID::CHACHA20_POLY1305>()
{
  static const auto instance = make_aead(AEAD::ID::CHACHA20_POLY1305);
  return instance;
}

static size_t
cipher_key_size(AEAD::ID cipher)
{
  switch (cipher) {
    case AEAD::ID::AES_128_GCM:
      return 16;

    case AEAD::ID::AES_256_GCM:
    case AEAD::ID::CHACHA20_POLY1305:
      return 32;

    default:
      throw std::runtime_error("Unsupported algorithm");
  }
}

static size_t
cipher_nonce_size(AEAD::ID cipher)
{
  switch (cipher) {
    case AEAD::ID::AES_128_GCM:
    case AEAD::ID::AES_256_GCM:
    case AEAD::ID::CHACHA20_POLY1305:
      return 12;

    default:
      throw std::runtime_error("Unsupported algorithm");
  }
}

static size_t
cipher_tag_size(AEAD::ID cipher)
{
  switch (cipher) {
    case AEAD::ID::AES_128_GCM:
    case AEAD::ID::AES_256_GCM:
    case AEAD::ID::CHACHA20_POLY1305:
      return 16;

    default:
      throw std::runtime_error("Unsupported algorithm");
  }
}

AEADCipher::AEADCipher(AEAD::ID id_in)
  : AEAD(id_in, cipher_key_size(id_in), cipher_nonce_size(id_in))
  , tag_size(cipher_tag_size(id_in))
{
}

// mbedtls keeps GCM and ChaCha20-Poly1305 behind separate APIs rather than one
// generic AEAD interface, so each is driven directly.
bytes
AEADCipher::seal(const bytes& key,
                 const bytes& nonce,
                 const bytes& aad,
                 const bytes& pt) const
{
  if (key.size() != key_size || nonce.size() != nonce_size) {
    throw std::runtime_error("Invalid key or nonce size");
  }

  auto ct = bytes(pt.size() + tag_size);
  auto* tag = ct.data() + pt.size();

  if (id == AEAD::ID::CHACHA20_POLY1305) {
    mbedtls_chachapoly_context ctx;
    mbedtls_chachapoly_init(&ctx);

    auto rc = mbedtls_chachapoly_setkey(&ctx, key.data());
    if (rc == 0) {
      rc = mbedtls_chachapoly_encrypt_and_tag(&ctx,
                                              pt.size(),
                                              nonce.data(),
                                              aad.data(),
                                              aad.size(),
                                              pt.data(),
                                              ct.data(),
                                              tag);
    }
    mbedtls_chachapoly_free(&ctx);

    if (rc != 0) {
      throw mbedtls_error(rc, "chachapoly_encrypt_and_tag");
    }
    return ct;
  }

  mbedtls_gcm_context ctx;
  mbedtls_gcm_init(&ctx);

  auto rc = mbedtls_gcm_setkey(
    &ctx, MBEDTLS_CIPHER_ID_AES, key.data(), static_cast<unsigned int>(key.size() * 8));
  if (rc == 0) {
    rc = mbedtls_gcm_crypt_and_tag(&ctx,
                                   MBEDTLS_GCM_ENCRYPT,
                                   pt.size(),
                                   nonce.data(),
                                   nonce.size(),
                                   aad.data(),
                                   aad.size(),
                                   pt.data(),
                                   ct.data(),
                                   tag_size,
                                   tag);
  }
  mbedtls_gcm_free(&ctx);

  if (rc != 0) {
    throw mbedtls_error(rc, "gcm_crypt_and_tag");
  }
  return ct;
}

std::optional<bytes>
AEADCipher::open(const bytes& key,
                 const bytes& nonce,
                 const bytes& aad,
                 const bytes& ct) const
{
  if (key.size() != key_size || nonce.size() != nonce_size) {
    throw std::runtime_error("Invalid key or nonce size");
  }

  if (ct.size() < tag_size) {
    throw std::runtime_error("AEAD ciphertext smaller than tag size");
  }

  const auto inner_size = ct.size() - tag_size;
  auto pt = bytes(inner_size);
  const auto* tag = ct.data() + inner_size;

  if (id == AEAD::ID::CHACHA20_POLY1305) {
    mbedtls_chachapoly_context ctx;
    mbedtls_chachapoly_init(&ctx);

    auto rc = mbedtls_chachapoly_setkey(&ctx, key.data());
    if (rc == 0) {
      rc = mbedtls_chachapoly_auth_decrypt(&ctx,
                                           inner_size,
                                           nonce.data(),
                                           aad.data(),
                                           aad.size(),
                                           tag,
                                           ct.data(),
                                           pt.data());
    }
    mbedtls_chachapoly_free(&ctx);

    // A failed tag check is an expected outcome, not an error.
    if (rc != 0) {
      return std::nullopt;
    }
    return pt;
  }

  mbedtls_gcm_context ctx;
  mbedtls_gcm_init(&ctx);

  auto rc = mbedtls_gcm_setkey(
    &ctx, MBEDTLS_CIPHER_ID_AES, key.data(), static_cast<unsigned int>(key.size() * 8));
  if (rc == 0) {
    rc = mbedtls_gcm_auth_decrypt(&ctx,
                                  inner_size,
                                  nonce.data(),
                                  nonce.size(),
                                  aad.data(),
                                  aad.size(),
                                  tag,
                                  tag_size,
                                  ct.data(),
                                  pt.data());
  }
  mbedtls_gcm_free(&ctx);

  if (rc != 0) {
    return std::nullopt;
  }
  return pt;
}

} // namespace MLS_NAMESPACE::hpke
