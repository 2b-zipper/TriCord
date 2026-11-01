#include "network/tls.h"
#include "log.h"

#include <mutex>

namespace Network {

namespace {
constexpr const char *CA_BUNDLE_PATH = "romfs:/cacert-2025-12-02.pem";

std::mutex chainMutex;
mbedtls_x509_crt chain;
bool chainLoaded = false;
} // namespace

mbedtls_x509_crt *sharedCaChain() {
	std::lock_guard<std::mutex> lock(chainMutex);
	if (!chainLoaded) {
		mbedtls_x509_crt_init(&chain);
		const int ret = mbedtls_x509_crt_parse_file(&chain, CA_BUNDLE_PATH);
		if (ret < 0) {
			Logger::log("[TLS] Failed to load CA bundle: -0x%04x", -ret);
		}
		chainLoaded = true;
	}
	return &chain;
}

} // namespace Network
