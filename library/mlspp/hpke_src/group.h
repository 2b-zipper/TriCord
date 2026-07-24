#pragma once

#include <hpke/hpke.h>
#include <hpke/signature.h>
#include <namespace.h>

#include "mbedtls_common.h"

#include <mbedtls/ecp.h>

namespace MLS_NAMESPACE::hpke {

struct Group
{
  enum struct ID : uint8_t
  {
    P256,
    P384,
    P521,
    X25519,
    X448,
    Ed25519,
    Ed448,
  };

  struct PublicKey
    : public KEM::PublicKey
    , public Signature::PublicKey
  {
    virtual ~PublicKey() = default;
  };

  struct PrivateKey
  {
    virtual ~PrivateKey() = default;
    virtual std::unique_ptr<PublicKey> public_key() const = 0;
  };

  template<Group::ID id>
  static const Group& get();

  virtual ~Group() = default;

  const ID id;
  const size_t seed_size;
  const size_t dh_size;
  const size_t pk_size;
  const size_t sk_size;
  const std::string jwk_key_type;
  const std::string jwk_curve_name;

  virtual std::unique_ptr<PrivateKey> generate_key_pair() const = 0;
  virtual std::unique_ptr<PrivateKey> derive_key_pair(
    const bytes& suite_id,
    const bytes& ikm) const = 0;
  virtual std::unique_ptr<PrivateKey> random_scalar(
    const bytes& seed) const = 0;

  virtual bytes serialize(const PublicKey& pk) const = 0;
  virtual std::unique_ptr<PublicKey> deserialize(const bytes& enc) const = 0;

  virtual bytes serialize_private(const PrivateKey& sk) const = 0;
  virtual std::unique_ptr<PrivateKey> deserialize_private(
    const bytes& skm) const = 0;
  virtual std::unique_ptr<PrivateKey> deserialize_private_der(
    const bytes& der) const = 0;

  virtual bytes dh(const PrivateKey& sk, const PublicKey& pk) const = 0;

  virtual bytes sign(const bytes& data, const PrivateKey& sk) const = 0;
  virtual bool verify(const bytes& data,
                      const bytes& sig,
                      const PublicKey& pk) const = 0;

  virtual std::tuple<bytes, bytes> coordinates(const PublicKey& pk) const = 0;
  virtual std::unique_ptr<PublicKey> public_key_from_coordinates(
    const bytes& x,
    const bytes& y) const = 0;

protected:
  const KDF& kdf;

  friend struct DHKEM;

  Group(ID group_id_in, const KDF& kdf_in);
};

// Only short-Weierstrass curves are provided. DAVE's ciphersuite is P-256, and
// mbedtls has no Ed25519 signatures, so the montgomery/edwards groups are gone.
struct ECGroup : public Group
{
  ECGroup(Group::ID group_id, const KDF& kdf);

  struct PublicKey : public Group::PublicKey
  {
    PublicKey();
    PublicKey(const PublicKey& other);
    PublicKey& operator=(const PublicKey& other) = delete;
    ~PublicKey() override;

    // NOLINTNEXTLINE(misc-non-private-member-variables-in-classes)
    mbedtls_ecp_point point;
  };

  struct PrivateKey : public Group::PrivateKey
  {
    PrivateKey();
    PrivateKey(const PrivateKey& other) = delete;
    PrivateKey& operator=(const PrivateKey& other) = delete;
    ~PrivateKey() override;

    std::unique_ptr<Group::PublicKey> public_key() const override;

    // NOLINTNEXTLINE(misc-non-private-member-variables-in-classes)
    mbedtls_mpi d;
    // NOLINTNEXTLINE(misc-non-private-member-variables-in-classes)
    mbedtls_ecp_point pub;
  };

  std::unique_ptr<Group::PrivateKey> generate_key_pair() const override;
  std::unique_ptr<Group::PrivateKey> derive_key_pair(
    const bytes& suite_id,
    const bytes& ikm) const override;
  std::unique_ptr<Group::PrivateKey> random_scalar(
    const bytes& seed) const override;

  bytes serialize(const Group::PublicKey& pk) const override;
  std::unique_ptr<Group::PublicKey> deserialize(
    const bytes& enc) const override;

  bytes serialize_private(const Group::PrivateKey& sk) const override;
  std::unique_ptr<Group::PrivateKey> deserialize_private(
    const bytes& skm) const override;
  std::unique_ptr<Group::PrivateKey> deserialize_private_der(
    const bytes& der) const override;

  bytes dh(const Group::PrivateKey& sk,
           const Group::PublicKey& pk) const override;

  bytes sign(const bytes& data, const Group::PrivateKey& sk) const override;
  bool verify(const bytes& data,
              const bytes& sig,
              const Group::PublicKey& pk) const override;

  std::tuple<bytes, bytes> coordinates(
    const Group::PublicKey& pk) const override;
  std::unique_ptr<Group::PublicKey> public_key_from_coordinates(
    const bytes& x,
    const bytes& y) const override;

private:
  mbedtls_ecp_group_id curve_id;

  void load_group(mbedtls_ecp_group& grp) const;
  uint8_t bitmask() const;
  std::unique_ptr<Group::PrivateKey> key_from_scalar(mbedtls_mpi& scalar) const;
};

} // namespace MLS_NAMESPACE::hpke
