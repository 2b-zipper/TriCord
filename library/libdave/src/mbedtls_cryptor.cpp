#include "mbedtls_cryptor.h"

#include <cstring>

#include <bytes/bytes.h>
#include <dave/logger.h>

#include "common.h"

namespace discord {
namespace dave {

MbedtlsCryptor::MbedtlsCryptor(const EncryptionKey& encryptionKey)
{
    mbedtls_gcm_init(&ctx_);

    const auto rc = mbedtls_gcm_setkey(
      &ctx_, MBEDTLS_CIPHER_ID_AES, encryptionKey.data(), kAesGcm128KeyBytes * 8);
    if (rc != 0) {
        DISCORD_LOG(LS_ERROR) << "mbedtls_gcm_setkey failed: " << rc;
        return;
    }

    valid_ = true;
}

MbedtlsCryptor::~MbedtlsCryptor()
{
    mbedtls_gcm_free(&ctx_);
}

bool MbedtlsCryptor::Encrypt(ArrayView<uint8_t> ciphertextBufferOut,
                             ArrayView<const uint8_t> plaintextBuffer,
                             ArrayView<const uint8_t> nonceBuffer,
                             ArrayView<const uint8_t> additionalData,
                             ArrayView<uint8_t> tagBufferOut)
{
    if (!valid_) {
        return false;
    }

    // DAVE truncates the authentication tag to 64 bits to keep frame overhead
    // down; mbedtls accepts the shorter length directly.
    const auto rc = mbedtls_gcm_crypt_and_tag(&ctx_,
                                              MBEDTLS_GCM_ENCRYPT,
                                              plaintextBuffer.size(),
                                              nonceBuffer.data(),
                                              nonceBuffer.size(),
                                              additionalData.data(),
                                              additionalData.size(),
                                              plaintextBuffer.data(),
                                              ciphertextBufferOut.data(),
                                              tagBufferOut.size(),
                                              tagBufferOut.data());
    if (rc != 0) {
        DISCORD_LOG(LS_ERROR) << "mbedtls_gcm_crypt_and_tag failed: " << rc;
        return false;
    }

    return true;
}

bool MbedtlsCryptor::Decrypt(ArrayView<uint8_t> plaintextBufferOut,
                             ArrayView<const uint8_t> ciphertextBuffer,
                             ArrayView<const uint8_t> tagBuffer,
                             ArrayView<const uint8_t> nonceBuffer,
                             ArrayView<const uint8_t> additionalData)
{
    if (!valid_) {
        return false;
    }

    // A rejected tag is an ordinary outcome here, so it is not logged as an
    // error: senders ratchet keys and receivers routinely try stale ones.
    const auto rc = mbedtls_gcm_auth_decrypt(&ctx_,
                                             ciphertextBuffer.size(),
                                             nonceBuffer.data(),
                                             nonceBuffer.size(),
                                             additionalData.data(),
                                             additionalData.size(),
                                             tagBuffer.data(),
                                             tagBuffer.size(),
                                             ciphertextBuffer.data(),
                                             plaintextBufferOut.data());
    return rc == 0;
}

} // namespace dave
} // namespace discord
