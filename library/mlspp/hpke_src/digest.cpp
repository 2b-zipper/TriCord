#include <hpke/digest.h>
#include <namespace.h>

#include <mbedtls/md.h>

#include "common.h"
#include "mbedtls_common.h"

namespace MLS_NAMESPACE::hpke {

static mbedtls_md_type_t
mbedtls_digest_type(Digest::ID digest)
{
  switch (digest) {
    case Digest::ID::SHA256:
      return MBEDTLS_MD_SHA256;

    case Digest::ID::SHA384:
      return MBEDTLS_MD_SHA384;

    case Digest::ID::SHA512:
      return MBEDTLS_MD_SHA512;

    default:
      // mbedtls has no SHA3; DAVE's ciphersuite never selects it.
      throw std::runtime_error("Unsupported digest algorithm");
  }
}

static const mbedtls_md_info_t*
digest_info(Digest::ID digest)
{
  const auto* info = mbedtls_md_info_from_type(mbedtls_digest_type(digest));
  if (info == nullptr) {
    throw std::runtime_error("Unsupported digest algorithm");
  }
  return info;
}

template<>
const Digest&
Digest::get<Digest::ID::SHA256>()
{
  static const Digest instance(Digest::ID::SHA256);
  return instance;
}

template<>
const Digest&
Digest::get<Digest::ID::SHA384>()
{
  static const Digest instance(Digest::ID::SHA384);
  return instance;
}

template<>
const Digest&
Digest::get<Digest::ID::SHA512>()
{
  static const Digest instance(Digest::ID::SHA512);
  return instance;
}

template<>
const Digest&
Digest::get<Digest::ID::SHA3_256>()
{
  static const Digest instance(Digest::ID::SHA3_256);
  return instance;
}

Digest::Digest(Digest::ID id_in)
  : id(id_in)
  , hash_size(mbedtls_md_get_size(digest_info(id_in)))
{
}

bytes
Digest::hash(const bytes& data) const
{
  auto md = bytes(hash_size);
  const auto rc =
    mbedtls_md(digest_info(id), data.data(), data.size(), md.data());
  if (rc != 0) {
    throw mbedtls_error(rc, "mbedtls_md");
  }
  return md;
}

bytes
Digest::hmac(const bytes& key, const bytes& data) const
{
  auto md = bytes(hash_size);

  // mbedtls dereferences the key pointer even when the length is zero.
  static const uint8_t empty = 0;
  const auto* key_data = key.data() != nullptr ? key.data() : &empty;

  const auto rc = mbedtls_md_hmac(
    digest_info(id), key_data, key.size(), data.data(), data.size(), md.data());
  if (rc != 0) {
    throw mbedtls_error(rc, "mbedtls_md_hmac");
  }
  return md;
}

// The OpenSSL backend only differs here to relax FIPS key-length enforcement,
// which mbedtls does not apply.
bytes
Digest::hmac_for_hkdf_extract(const bytes& key, const bytes& data) const
{
  return hmac(key, data);
}

bytes
SHAKE256::derive(const bytes& /* ikm */, size_t /* length */)
{
  throw std::runtime_error("SHAKE256 not supported by the mbedtls backend");
}

bytes
SHAKE256::labeled_derive(KEM::ID /* kem_id */,
                         const bytes& /* ikm */,
                         const std::string& /* label */,
                         const bytes& /* context */,
                         size_t /* length */)
{
  throw std::runtime_error("SHAKE256 not supported by the mbedtls backend");
}

} // namespace MLS_NAMESPACE::hpke
