#include <hpke/signature.h>
#include <namespace.h>

#include "group.h"
#include "mbedtls_common.h"

#include <stdexcept>

namespace MLS_NAMESPACE::hpke {

// A thin delegation onto Group. JWK, PEM and RSA support are dropped: DAVE
// exchanges raw keys inside MLS credentials and never uses them.
struct GroupSignature : public Signature
{
  struct PrivateKey : public Signature::PrivateKey
  {
    explicit PrivateKey(Group::PrivateKey* group_priv_in)
      : group_priv(group_priv_in)
    {
    }

    std::unique_ptr<Signature::PublicKey> public_key() const override
    {
      return group_priv->public_key();
    }

    std::unique_ptr<Group::PrivateKey> group_priv;
  };

  static Signature::ID group_to_sig(Group::ID group_id)
  {
    switch (group_id) {
      case Group::ID::P256:
        return Signature::ID::P256_SHA256;
      case Group::ID::P384:
        return Signature::ID::P384_SHA384;
      case Group::ID::P521:
        return Signature::ID::P521_SHA512;
      default:
        throw std::runtime_error("Unsupported group");
    }
  }

  explicit GroupSignature(const Group& group_in)
    : Signature(group_to_sig(group_in.id))
    , group(group_in)
  {
  }

  std::unique_ptr<Signature::PrivateKey> generate_key_pair() const override
  {
    return std::make_unique<PrivateKey>(group.generate_key_pair().release());
  }

  std::unique_ptr<Signature::PrivateKey> derive_key_pair(
    const bytes& ikm) const override
  {
    return std::make_unique<PrivateKey>(
      group.derive_key_pair({}, ikm).release());
  }

  bytes serialize(const Signature::PublicKey& pk) const override
  {
    const auto& rpk = dynamic_cast<const Group::PublicKey&>(pk);
    return group.serialize(rpk);
  }

  std::unique_ptr<Signature::PublicKey> deserialize(
    const bytes& enc) const override
  {
    return group.deserialize(enc);
  }

  bytes serialize_private(const Signature::PrivateKey& sk) const override
  {
    const auto& rsk = dynamic_cast<const PrivateKey&>(sk);
    return group.serialize_private(*rsk.group_priv);
  }

  std::unique_ptr<Signature::PrivateKey> deserialize_private(
    const bytes& skm) const override
  {
    return std::make_unique<PrivateKey>(
      group.deserialize_private(skm).release());
  }

  std::unique_ptr<Signature::PrivateKey> deserialize_private_der(
    const bytes& der) const override
  {
    return std::make_unique<PrivateKey>(
      group.deserialize_private_der(der).release());
  }

  bytes sign(const bytes& data, const Signature::PrivateKey& sk) const override
  {
    const auto& rsk = dynamic_cast<const PrivateKey&>(sk);
    return group.sign(data, *rsk.group_priv);
  }

  bool verify(const bytes& data,
              const bytes& sig,
              const Signature::PublicKey& pk) const override
  {
    const auto& rpk = dynamic_cast<const Group::PublicKey&>(pk);
    return group.verify(data, sig, rpk);
  }

  std::unique_ptr<Signature::PrivateKey> import_jwk_private(
    const std::string& /* jwk_json */) const override
  {
    throw std::runtime_error("JWK not supported by the mbedtls backend");
  }

  std::unique_ptr<Signature::PublicKey> import_jwk(
    const std::string& /* jwk_json */) const override
  {
    throw std::runtime_error("JWK not supported by the mbedtls backend");
  }

  std::string export_jwk(const Signature::PublicKey& /* pk */) const override
  {
    throw std::runtime_error("JWK not supported by the mbedtls backend");
  }

  std::string export_jwk_private(
    const Signature::PrivateKey& /* sk */) const override
  {
    throw std::runtime_error("JWK not supported by the mbedtls backend");
  }

private:
  const Group& group;
};

template<>
const Signature&
Signature::get<Signature::ID::P256_SHA256>()
{
  static const auto instance = GroupSignature(Group::get<Group::ID::P256>());
  return instance;
}

template<>
const Signature&
Signature::get<Signature::ID::P384_SHA384>()
{
  static const auto instance = GroupSignature(Group::get<Group::ID::P384>());
  return instance;
}

template<>
const Signature&
Signature::get<Signature::ID::P521_SHA512>()
{
  static const auto instance = GroupSignature(Group::get<Group::ID::P521>());
  return instance;
}

// Defined so that ciphersuite tables still link, but unusable: mbedtls has no
// Ed25519/Ed448 signatures and RSA was dropped with the certificate code.
template<>
const Signature&
Signature::get<Signature::ID::Ed25519>()
{
  throw std::runtime_error("Ed25519 not supported by the mbedtls backend");
}

template<>
const Signature&
Signature::get<Signature::ID::Ed448>()
{
  throw std::runtime_error("Ed448 not supported by the mbedtls backend");
}

template<>
const Signature&
Signature::get<Signature::ID::RSA_SHA256>()
{
  throw std::runtime_error("RSA not supported by the mbedtls backend");
}

template<>
const Signature&
Signature::get<Signature::ID::RSA_SHA384>()
{
  throw std::runtime_error("RSA not supported by the mbedtls backend");
}

template<>
const Signature&
Signature::get<Signature::ID::RSA_SHA512>()
{
  throw std::runtime_error("RSA not supported by the mbedtls backend");
}

Signature::Signature(Signature::ID id_in)
  : id(id_in)
{
}

std::unique_ptr<Signature::PrivateKey>
Signature::deserialize_private_der(const bytes&) const
{
  throw std::runtime_error("Not implemented");
}

std::unique_ptr<Signature::PrivateKey>
Signature::generate_rsa(size_t /* bits */)
{
  throw std::runtime_error("RSA not supported by the mbedtls backend");
}

} // namespace MLS_NAMESPACE::hpke
