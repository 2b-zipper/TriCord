#pragma once

#include <mbedtls/gcm.h>

#include "cryptor.h"

namespace discord {
namespace dave {

class MbedtlsCryptor : public ICryptor {
public:
    MbedtlsCryptor(const EncryptionKey& encryptionKey);
    ~MbedtlsCryptor();

    bool IsValid() const { return valid_; }

    bool Encrypt(ArrayView<uint8_t> ciphertextBufferOut,
                 ArrayView<const uint8_t> plaintextBuffer,
                 ArrayView<const uint8_t> nonceBuffer,
                 ArrayView<const uint8_t> additionalData,
                 ArrayView<uint8_t> tagBufferOut) override;
    bool Decrypt(ArrayView<uint8_t> plaintextBufferOut,
                 ArrayView<const uint8_t> ciphertextBuffer,
                 ArrayView<const uint8_t> tagBuffer,
                 ArrayView<const uint8_t> nonceBuffer,
                 ArrayView<const uint8_t> additionalData) override;

private:
    mbedtls_gcm_context ctx_;
    bool valid_ = false;
};

} // namespace dave
} // namespace discord
