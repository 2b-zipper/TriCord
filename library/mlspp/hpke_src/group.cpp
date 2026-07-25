#include "group.h"
#include "common.h"

#include <hpke/digest.h>
#include <hpke/random.h>
#include <namespace.h>

#include <mbedtls/ecdsa.h>

namespace MLS_NAMESPACE::hpke {

///
/// Size and naming tables (mirrors the OpenSSL backend)
///

static inline size_t
group_seed_size(Group::ID group_id)
{
  switch (group_id) {
    case Group::ID::P256:
      return 128;
    case Group::ID::P384:
      return 48;
    case Group::ID::P521:
      return 66;
    default:
      throw std::runtime_error("Unsupported group");
  }
}

static inline size_t
group_dh_size(Group::ID group_id)
{
  switch (group_id) {
    case Group::ID::P256:
      return 32;
    case Group::ID::P384:
      return 48;
    case Group::ID::P521:
      return 66;
    default:
      throw std::runtime_error("Unsupported group");
  }
}

static inline size_t
group_pk_size(Group::ID group_id)
{
  switch (group_id) {
    case Group::ID::P256:
      return 65;
    case Group::ID::P384:
      return 97;
    case Group::ID::P521:
      return 133;
    default:
      throw std::runtime_error("Unsupported group");
  }
}

static inline size_t
group_sk_size(Group::ID group_id)
{
  switch (group_id) {
    case Group::ID::P256:
      return 32;
    case Group::ID::P384:
      return 48;
    case Group::ID::P521:
      return 66;
    default:
      throw std::runtime_error("Unsupported group");
  }
}

static inline std::string
group_jwk_curve_name(Group::ID group_id)
{
  switch (group_id) {
    case Group::ID::P256:
      return "P-256";
    case Group::ID::P384:
      return "P-384";
    case Group::ID::P521:
      return "P-521";
    default:
      throw std::runtime_error("Unsupported group");
  }
}

static inline std::string
group_jwk_key_type(Group::ID group_id)
{
  switch (group_id) {
    case Group::ID::P256:
    case Group::ID::P384:
    case Group::ID::P521:
      return "EC";
    default:
      throw std::runtime_error("Unsupported group");
  }
}

static inline mbedtls_ecp_group_id
group_to_curve(Group::ID group_id)
{
  switch (group_id) {
    case Group::ID::P256:
      return MBEDTLS_ECP_DP_SECP256R1;
    case Group::ID::P384:
      return MBEDTLS_ECP_DP_SECP384R1;
    case Group::ID::P521:
      return MBEDTLS_ECP_DP_SECP521R1;
    default:
      throw std::runtime_error("Unsupported group");
  }
}

static inline mbedtls_md_type_t
group_sig_digest(Group::ID group_id)
{
  switch (group_id) {
    case Group::ID::P256:
      return MBEDTLS_MD_SHA256;
    case Group::ID::P384:
      return MBEDTLS_MD_SHA384;
    case Group::ID::P521:
      return MBEDTLS_MD_SHA512;
    default:
      throw std::runtime_error("Unsupported group");
  }
}

Group::Group(ID group_id_in, const KDF& kdf_in)
  : id(group_id_in)
  , seed_size(group_seed_size(group_id_in))
  , dh_size(group_dh_size(group_id_in))
  , pk_size(group_pk_size(group_id_in))
  , sk_size(group_sk_size(group_id_in))
  , jwk_key_type(group_jwk_key_type(group_id_in))
  , jwk_curve_name(group_jwk_curve_name(group_id_in))
  , kdf(kdf_in)
{
}

///
/// Key material
///

ECGroup::PublicKey::PublicKey()
{
  mbedtls_ecp_point_init(&point);
}

ECGroup::PublicKey::PublicKey(const PublicKey& other)
  : Group::PublicKey(other)
{
  mbedtls_ecp_point_init(&point);
  const auto rc = mbedtls_ecp_copy(&point, &other.point);
  if (rc != 0) {
    throw mbedtls_error(rc, "ecp_copy");
  }
}

ECGroup::PublicKey::~PublicKey()
{
  mbedtls_ecp_point_free(&point);
}

ECGroup::PrivateKey::PrivateKey()
{
  mbedtls_mpi_init(&d);
  mbedtls_ecp_point_init(&pub);
}

ECGroup::PrivateKey::~PrivateKey()
{
  mbedtls_mpi_free(&d);
  mbedtls_ecp_point_free(&pub);
}

std::unique_ptr<Group::PublicKey>
ECGroup::PrivateKey::public_key() const
{
  auto pk = std::make_unique<ECGroup::PublicKey>();
  const auto rc = mbedtls_ecp_copy(&pk->point, &pub);
  if (rc != 0) {
    throw mbedtls_error(rc, "ecp_copy");
  }
  return pk;
}

///
/// ECGroup
///

ECGroup::ECGroup(Group::ID group_id, const KDF& kdf_in)
  : Group(group_id, kdf_in)
  , curve_id(group_to_curve(group_id))
{
}

void
ECGroup::load_group(mbedtls_ecp_group& grp) const
{
  const auto rc = mbedtls_ecp_group_load(&grp, curve_id);
  if (rc != 0) {
    throw mbedtls_error(rc, "ecp_group_load");
  }
}

uint8_t
ECGroup::bitmask() const
{
  switch (id) {
    case Group::ID::P256:
    case Group::ID::P384:
      return 0xff;
    case Group::ID::P521:
      return 0x01;
    default:
      throw std::runtime_error("Unsupported group");
  }
}

// Builds the matching public point for a scalar and hands back a private key.
std::unique_ptr<Group::PrivateKey>
ECGroup::key_from_scalar(mbedtls_mpi& scalar) const
{
  mbedtls_ecp_group grp;
  mbedtls_ecp_group_init(&grp);
  load_group(grp);

  auto key = std::make_unique<ECGroup::PrivateKey>();
  auto rc = mbedtls_mpi_copy(&key->d, &scalar);
  if (rc == 0) {
    rc = mbedtls_ecp_mul(&grp, &key->pub, &key->d, &grp.G, drbg_random, nullptr);
  }
  mbedtls_ecp_group_free(&grp);

  if (rc != 0) {
    throw mbedtls_error(rc, "ecp_mul");
  }
  return key;
}

std::unique_ptr<Group::PrivateKey>
ECGroup::generate_key_pair() const
{
  return derive_key_pair({}, random_bytes(sk_size));
}

// RFC 9180 DeriveKeyPair for the NIST curves: expand candidates until one lands
// in [1, order).
std::unique_ptr<Group::PrivateKey>
ECGroup::derive_key_pair(const bytes& suite_id, const bytes& ikm) const
{
  static const int retry_limit = 255;
  static const auto label_dkp_prk = from_ascii("dkp_prk");
  static const auto label_candidate = from_ascii("candidate");

  const auto dkp_prk = kdf.labeled_extract(suite_id, {}, label_dkp_prk, ikm);

  mbedtls_ecp_group grp;
  mbedtls_ecp_group_init(&grp);
  load_group(grp);

  mbedtls_mpi sk;
  mbedtls_mpi_init(&sk);

  auto counter = 0;
  while (true) {
    const auto ctr = i2osp(counter, 1);
    auto candidate =
      kdf.labeled_expand(suite_id, dkp_prk, label_candidate, ctr, sk_size);
    candidate.at(0) &= bitmask();

    const auto rc =
      mbedtls_mpi_read_binary(&sk, candidate.data(), candidate.size());
    if (rc != 0) {
      mbedtls_mpi_free(&sk);
      mbedtls_ecp_group_free(&grp);
      throw mbedtls_error(rc, "mpi_read_binary");
    }

    const auto in_range =
      mbedtls_mpi_cmp_int(&sk, 0) != 0 && mbedtls_mpi_cmp_mpi(&sk, &grp.N) < 0;
    if (in_range) {
      break;
    }

    counter += 1;
    if (counter > retry_limit) {
      mbedtls_mpi_free(&sk);
      mbedtls_ecp_group_free(&grp);
      throw std::runtime_error("DeriveKeyPair iteration limit exceeded");
    }
  }

  mbedtls_ecp_group_free(&grp);
  auto key = key_from_scalar(sk);
  mbedtls_mpi_free(&sk);
  return key;
}

// Consumes the seed sk_size bytes at a time until a valid scalar appears.
std::unique_ptr<Group::PrivateKey>
ECGroup::random_scalar(const bytes& seed) const
{
  mbedtls_ecp_group grp;
  mbedtls_ecp_group_init(&grp);
  load_group(grp);

  mbedtls_mpi sk;
  mbedtls_mpi_init(&sk);

  auto start = size_t(0);
  auto end = sk_size;
  while (true) {
    if (end > seed.size()) {
      mbedtls_mpi_free(&sk);
      mbedtls_ecp_group_free(&grp);
      throw std::runtime_error("Failed to generate scalar from seed");
    }

    const auto candidate = seed.slice(start, end);
    const auto rc =
      mbedtls_mpi_read_binary(&sk, candidate.data(), candidate.size());
    if (rc != 0) {
      mbedtls_mpi_free(&sk);
      mbedtls_ecp_group_free(&grp);
      throw mbedtls_error(rc, "mpi_read_binary");
    }

    const auto in_range =
      mbedtls_mpi_cmp_int(&sk, 0) != 0 && mbedtls_mpi_cmp_mpi(&sk, &grp.N) < 0;
    if (in_range) {
      break;
    }

    start = end;
    end = start + sk_size;
  }

  mbedtls_ecp_group_free(&grp);
  auto key = key_from_scalar(sk);
  mbedtls_mpi_free(&sk);
  return key;
}

bytes
ECGroup::serialize(const Group::PublicKey& pk) const
{
  const auto& rpk = dynamic_cast<const ECGroup::PublicKey&>(pk);

  mbedtls_ecp_group grp;
  mbedtls_ecp_group_init(&grp);
  load_group(grp);

  auto out = bytes(pk_size);
  size_t olen = 0;
  const auto rc = mbedtls_ecp_point_write_binary(&grp,
                                                 &rpk.point,
                                                 MBEDTLS_ECP_PF_UNCOMPRESSED,
                                                 &olen,
                                                 out.data(),
                                                 out.size());
  mbedtls_ecp_group_free(&grp);

  if (rc != 0 || olen != pk_size) {
    throw mbedtls_error(rc, "ecp_point_write_binary");
  }
  return out;
}

std::unique_ptr<Group::PublicKey>
ECGroup::deserialize(const bytes& enc) const
{
  mbedtls_ecp_group grp;
  mbedtls_ecp_group_init(&grp);
  load_group(grp);

  auto pk = std::make_unique<ECGroup::PublicKey>();
  auto rc = mbedtls_ecp_point_read_binary(&grp, &pk->point, enc.data(), enc.size());

  // Reject points that are not actually on the curve.
  if (rc == 0) {
    rc = mbedtls_ecp_check_pubkey(&grp, &pk->point);
  }
  mbedtls_ecp_group_free(&grp);

  if (rc != 0) {
    throw mbedtls_error(rc, "ecp_point_read_binary");
  }
  return pk;
}

bytes
ECGroup::serialize_private(const Group::PrivateKey& sk) const
{
  const auto& rsk = dynamic_cast<const ECGroup::PrivateKey&>(sk);

  auto out = bytes(sk_size);
  const auto rc = mbedtls_mpi_write_binary(&rsk.d, out.data(), out.size());
  if (rc != 0) {
    throw mbedtls_error(rc, "mpi_write_binary");
  }
  return out;
}

std::unique_ptr<Group::PrivateKey>
ECGroup::deserialize_private(const bytes& skm) const
{
  mbedtls_mpi sk;
  mbedtls_mpi_init(&sk);

  const auto rc = mbedtls_mpi_read_binary(&sk, skm.data(), skm.size());
  if (rc != 0) {
    mbedtls_mpi_free(&sk);
    throw mbedtls_error(rc, "mpi_read_binary");
  }

  auto key = key_from_scalar(sk);
  mbedtls_mpi_free(&sk);
  return key;
}

std::unique_ptr<Group::PrivateKey>
ECGroup::deserialize_private_der(const bytes& /* der */) const
{
  // Only used for X.509 handling, which DAVE does not exercise.
  throw std::runtime_error("DER private keys not supported by this backend");
}

bytes
ECGroup::dh(const Group::PrivateKey& sk, const Group::PublicKey& pk) const
{
  const auto& rsk = dynamic_cast<const ECGroup::PrivateKey&>(sk);
  const auto& rpk = dynamic_cast<const ECGroup::PublicKey&>(pk);

  mbedtls_ecp_group grp;
  mbedtls_ecp_group_init(&grp);
  load_group(grp);

  mbedtls_ecp_point shared;
  mbedtls_ecp_point_init(&shared);

  auto out = bytes(dh_size);
  auto rc = mbedtls_ecp_mul(&grp, &shared, &rsk.d, &rpk.point, drbg_random, nullptr);

  // The DH output is the x-coordinate, left-padded to the field size.
  if (rc == 0) {
    rc = mbedtls_mpi_write_binary(&shared.X, out.data(), out.size());
  }

  mbedtls_ecp_point_free(&shared);
  mbedtls_ecp_group_free(&grp);

  if (rc != 0) {
    throw mbedtls_error(rc, "ecdh");
  }
  return out;
}

bytes
ECGroup::sign(const bytes& data, const Group::PrivateKey& sk) const
{
  const auto& rsk = dynamic_cast<const ECGroup::PrivateKey&>(sk);

  const auto digest_type = group_sig_digest(id);
  const auto* md_info = mbedtls_md_info_from_type(digest_type);
  auto hash = bytes(mbedtls_md_get_size(md_info));
  auto rc = mbedtls_md(md_info, data.data(), data.size(), hash.data());
  if (rc != 0) {
    throw mbedtls_error(rc, "mbedtls_md");
  }

  mbedtls_ecdsa_context ctx;
  mbedtls_ecdsa_init(&ctx);
  load_group(ctx.grp);

  auto sig = bytes(MBEDTLS_ECDSA_MAX_LEN);
  size_t siglen = 0;

  rc = mbedtls_mpi_copy(&ctx.d, &rsk.d);
  if (rc == 0) {
    rc = mbedtls_ecdsa_write_signature(&ctx,
                                       digest_type,
                                       hash.data(),
                                       hash.size(),
                                       sig.data(),
                                       &siglen,
                                       drbg_random,
                                       nullptr);
  }
  mbedtls_ecdsa_free(&ctx);

  if (rc != 0) {
    throw mbedtls_error(rc, "ecdsa_write_signature");
  }

  return sig.slice(0, siglen);
}

bool
ECGroup::verify(const bytes& data,
                const bytes& sig,
                const Group::PublicKey& pk) const
{
  const auto& rpk = dynamic_cast<const ECGroup::PublicKey&>(pk);

  const auto digest_type = group_sig_digest(id);
  const auto* md_info = mbedtls_md_info_from_type(digest_type);
  auto hash = bytes(mbedtls_md_get_size(md_info));
  if (mbedtls_md(md_info, data.data(), data.size(), hash.data()) != 0) {
    return false;
  }

  mbedtls_ecdsa_context ctx;
  mbedtls_ecdsa_init(&ctx);
  load_group(ctx.grp);

  auto rc = mbedtls_ecp_copy(&ctx.Q, &rpk.point);
  if (rc == 0) {
    rc = mbedtls_ecdsa_read_signature(
      &ctx, hash.data(), hash.size(), sig.data(), sig.size());
  }
  mbedtls_ecdsa_free(&ctx);

  return rc == 0;
}

std::tuple<bytes, bytes>
ECGroup::coordinates(const Group::PublicKey& pk) const
{
  const auto& rpk = dynamic_cast<const ECGroup::PublicKey&>(pk);

  auto x = bytes(dh_size);
  auto y = bytes(dh_size);

  auto rc = mbedtls_mpi_write_binary(
    &rpk.point.X, x.data(), x.size());
  if (rc == 0) {
    rc = mbedtls_mpi_write_binary(
      &rpk.point.Y, y.data(), y.size());
  }
  if (rc != 0) {
    throw mbedtls_error(rc, "mpi_write_binary");
  }

  return { x, y };
}

std::unique_ptr<Group::PublicKey>
ECGroup::public_key_from_coordinates(const bytes& x, const bytes& y) const
{
  // Reuse the uncompressed-point path so curve validation still applies.
  auto enc = bytes{ 0x04 } + x + y;
  return deserialize(enc);
}

///
/// Group instances
///

template<>
const Group&
Group::get<Group::ID::P256>()
{
  static const ECGroup instance(Group::ID::P256,
                                KDF::get<KDF::ID::HKDF_SHA256>());
  return instance;
}

template<>
const Group&
Group::get<Group::ID::P384>()
{
  static const ECGroup instance(Group::ID::P384,
                                KDF::get<KDF::ID::HKDF_SHA384>());
  return instance;
}

template<>
const Group&
Group::get<Group::ID::P521>()
{
  static const ECGroup instance(Group::ID::P521,
                                KDF::get<KDF::ID::HKDF_SHA512>());
  return instance;
}

// Defined so the DHKEM and signature tables link. mbedtls exposes Curve25519
// only through ECDH, with no Ed25519 signatures and no X448 at all, and DAVE
// selects none of them.
template<>
const Group&
Group::get<Group::ID::X25519>()
{
  throw std::runtime_error("X25519 not supported by the mbedtls backend");
}

template<>
const Group&
Group::get<Group::ID::X448>()
{
  throw std::runtime_error("X448 not supported by the mbedtls backend");
}

template<>
const Group&
Group::get<Group::ID::Ed25519>()
{
  throw std::runtime_error("Ed25519 not supported by the mbedtls backend");
}

template<>
const Group&
Group::get<Group::ID::Ed448>()
{
  throw std::runtime_error("Ed448 not supported by the mbedtls backend");
}

} // namespace MLS_NAMESPACE::hpke
