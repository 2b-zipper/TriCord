// X.509 certificates and UserInfo verifiable credentials are referenced by the
// MLS credential code but never used by DAVE, which relies on basic
// credentials. Dropping the OpenSSL implementations leaves these symbols, so
// they are provided here and fail loudly if a group ever asks for them.
#include <hpke/certificate.h>
#include <hpke/signature.h>
#include <hpke/userinfo_vc.h>
#include <namespace.h>

#include <stdexcept>

namespace MLS_NAMESPACE::hpke {

static std::runtime_error
unsupported(const char* what)
{
  return std::runtime_error(std::string(what) +
                            " is not supported by the mbedtls backend");
}

///
/// Certificate
///

struct Certificate::ParsedCertificate
{
};

Certificate::Certificate(const bytes& /* der */)
{
  throw unsupported("X.509 certificates");
}

Certificate::Certificate(std::unique_ptr<ParsedCertificate>&& /* parsed */)
{
  throw unsupported("X.509 certificates");
}

Certificate::Certificate(const Certificate& /* other */)
{
  throw unsupported("X.509 certificates");
}

Certificate::~Certificate() = default;

bool
Certificate::valid_from(const Certificate& /* parent */) const
{
  throw unsupported("X.509 certificates");
}

Signature::ID
Certificate::public_key_algorithm() const
{
  throw unsupported("X.509 certificates");
}

///
/// UserInfoVC
///

struct UserInfoVC::ParsedCredential
{
};

UserInfoVC::UserInfoVC(std::string /* jwt */)
{
  throw unsupported("UserInfo verifiable credentials");
}

const Signature&
UserInfoVC::signature_algorithm() const
{
  throw unsupported("UserInfo verifiable credentials");
}

const Signature::PublicJWK&
UserInfoVC::public_key() const
{
  throw unsupported("UserInfo verifiable credentials");
}

bool
UserInfoVC::valid_from(const Signature::PublicKey& /* pub */) const
{
  throw unsupported("UserInfo verifiable credentials");
}

///
/// JWK parsing
///

Signature::PublicJWK
Signature::parse_jwk(const std::string& /* jwk_json */)
{
  throw unsupported("JWK parsing");
}

Signature::PrivateJWK
Signature::parse_jwk_private(const std::string& /* jwk_json */)
{
  throw unsupported("JWK parsing");
}

} // namespace MLS_NAMESPACE::hpke
