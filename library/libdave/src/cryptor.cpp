#include "cryptor.h"

#include "mbedtls_cryptor.h"

namespace discord {
namespace dave {

std::unique_ptr<ICryptor> CreateCryptor(const EncryptionKey& encryptionKey)
{
    auto cryptor = std::make_unique<MbedtlsCryptor>(encryptionKey);

    return cryptor->IsValid() ? std::move(cryptor) : nullptr;
}

} // namespace dave
} // namespace discord
