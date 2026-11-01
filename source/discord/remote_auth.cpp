#include "discord/remote_auth.h"
#include "core/i18n.h"
#include "log.h"
#include "utils/base64_utils.h"
#include "utils/json_utils.h"
#include <3ds.h>
#include <sys/stat.h>

#include <cstring>
#include <mbedtls/base64.h>
#include <mbedtls/bignum.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <vector>

namespace Discord {

namespace {

constexpr const char *GATEWAY_URL = "wss://remote-auth-gateway.discord.gg/?v=2";
constexpr int CLOSE_SESSION_TIMEOUT = 4003;

constexpr int RSA_PUBLIC_EXPONENT = 65537;
constexpr int RSA_MODULUS_BITS = 2048;
constexpr int RSA_PRIME_COUNT = 3;
constexpr int RSA_PRIME_BITS[RSA_PRIME_COUNT] = {683, 683, 682};
constexpr int PRIME_TEST_ROUNDS = 4;
constexpr uint32_t SIEVE_PRIME_LIMIT = 65536;
constexpr size_t SIEVE_SPAN = 4096;

struct Mpi {
	mbedtls_mpi value;

	Mpi() { mbedtls_mpi_init(&value); }
	~Mpi() { mbedtls_mpi_free(&value); }
	operator mbedtls_mpi *() { return &value; }
};

int consoleEntropy(void *, unsigned char *output, size_t length, size_t *written) {
	if (R_FAILED(PS_GenerateRandomBytes(output, length))) {
		return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
	}
	*written = length;
	return 0;
}

std::vector<uint16_t> oddPrimesBelow(uint32_t limit) {
	std::vector<bool> composite(limit);
	std::vector<uint16_t> primes;
	for (uint32_t i = 3; i < limit; i += 2) {
		if (composite[i]) {
			continue;
		}
		primes.push_back(i);
		for (uint32_t j = i * i; j < limit && i < limit / i; j += 2 * i) {
			composite[j] = true;
		}
	}
	return primes;
}

int findPrime(mbedtls_mpi *prime, int bits, const std::vector<uint16_t> &sievePrimes, mbedtls_ctr_drbg_context *drbg) {
	const size_t bytes = (bits + 7) / 8;
	Mpi start;
	std::vector<bool> composite;
	int ret = 0;

	while (true) {
		MBEDTLS_MPI_CHK(mbedtls_mpi_fill_random(start, bytes, mbedtls_ctr_drbg_random, drbg));
		MBEDTLS_MPI_CHK(mbedtls_mpi_shift_r(start, bytes * 8 - bits));
		for (int bit : {0, bits - 3, bits - 2, bits - 1}) {
			MBEDTLS_MPI_CHK(mbedtls_mpi_set_bit(start, bit, 1));
		}

		composite.assign(SIEVE_SPAN, false);
		for (uint16_t divisor : sievePrimes) {
			mbedtls_mpi_uint remainder = 0;
			MBEDTLS_MPI_CHK(mbedtls_mpi_mod_int(&remainder, start, divisor));
			size_t offset = remainder == 0 ? 0 : remainder % 2 ? (divisor - remainder) / 2 : divisor - remainder / 2;
			for (; offset < SIEVE_SPAN; offset += divisor) {
				composite[offset] = true;
			}
		}

		for (size_t i = 0; i < SIEVE_SPAN; i++) {
			if (composite[i]) {
				continue;
			}
			MBEDTLS_MPI_CHK(mbedtls_mpi_add_int(prime, start, 2 * i));

			mbedtls_mpi_uint remainder = 0;
			MBEDTLS_MPI_CHK(mbedtls_mpi_mod_int(&remainder, prime, RSA_PUBLIC_EXPONENT));
			if (remainder == 1) {
				continue;
			}

			ret = mbedtls_mpi_is_prime_ext(prime, PRIME_TEST_ROUNDS, mbedtls_ctr_drbg_random, drbg);
			if (ret != MBEDTLS_ERR_MPI_NOT_ACCEPTABLE) {
				return ret;
			}
		}
	}

cleanup:
	return ret;
}

// Three primes take an eighth of the time two do, and the server only ever sees N. mbedTLS's
// CRT needs just two coprime factors, so P holds the product of two primes.
int generateKey(mbedtls_rsa_context *rsa, mbedtls_ctr_drbg_context *drbg) {
	const std::vector<uint16_t> sievePrimes = oddPrimesBelow(SIEVE_PRIME_LIMIT);
	Mpi primes[RSA_PRIME_COUNT];
	Mpi minusOne[RSA_PRIME_COUNT];
	Mpi e, p, n, phiP;
	int ret = 0;

	do {
		for (int i = 0; i < RSA_PRIME_COUNT; i++) {
			MBEDTLS_MPI_CHK(findPrime(primes[i], RSA_PRIME_BITS[i], sievePrimes, drbg));
			MBEDTLS_MPI_CHK(mbedtls_mpi_sub_int(minusOne[i], primes[i], 1));
		}
		MBEDTLS_MPI_CHK(mbedtls_mpi_mul_mpi(p, primes[0], primes[1]));
		MBEDTLS_MPI_CHK(mbedtls_mpi_mul_mpi(n, p, primes[2]));
	} while (mbedtls_mpi_bitlen(n) != RSA_MODULUS_BITS);

	MBEDTLS_MPI_CHK(mbedtls_mpi_lset(e, RSA_PUBLIC_EXPONENT));
	MBEDTLS_MPI_CHK(mbedtls_mpi_mul_mpi(phiP, minusOne[0], minusOne[1]));
	MBEDTLS_MPI_CHK(mbedtls_rsa_import(rsa, n, p, primes[2], nullptr, e));
	MBEDTLS_MPI_CHK(mbedtls_mpi_inv_mod(&rsa->DP, e, phiP));
	MBEDTLS_MPI_CHK(mbedtls_mpi_inv_mod(&rsa->DQ, e, minusOne[2]));
	MBEDTLS_MPI_CHK(mbedtls_mpi_inv_mod(&rsa->QP, primes[2], p));
	rsa->len = mbedtls_mpi_size(&rsa->N);

cleanup:
	return ret;
}

} // namespace

RemoteAuth::RemoteAuth()
    : state(RemoteAuthState::IDLE), heartbeatInterval(0), lastHeartbeat(0), retryAt(0), rsaContext(nullptr),
      keyReady(false), sessionWanted(false), workerRunning(false), retryImmediately(false) {}

RemoteAuth::~RemoteAuth() {
	cancel();
	if (workerThread.joinable()) {
		workerThread.join();
	}
	cleanupRSA();
}

bool RemoteAuth::start() {
	if (sessionWanted && (state == RemoteAuthState::CONNECTING || state == RemoteAuthState::WAITING_FOR_SCAN ||
	                      state == RemoteAuthState::WAITING_FOR_CONFIRM)) {
		Logger::log("[RemoteAuth] Already in progress (state: %d)", (int)state.load());
		return false;
	}

	Logger::log("[RemoteAuth] Starting remote auth");
	fingerprint = "";
	sessionWanted = true;
	setState(RemoteAuthState::CONNECTING,
	         Core::I18n::getInstance().get(keyReady ? "login.status.connecting_auth" : "login.status.generating_keys"));
	launchWorker();
	return true;
}

void RemoteAuth::launchWorker() {
	if (!workerRunning.exchange(true)) {
		workerThread.start([this] { runSession(); }, 2, 32 * 1024, true);
	}
}

void RemoteAuth::runSession() {
	if (!keyReady) {
		keyReady = initRSA();
		if (keyReady && sessionWanted) {
			setState(RemoteAuthState::CONNECTING, Core::I18n::getInstance().get("login.status.connecting_auth"));
		}
	}

	if (!keyReady) {
		setState(RemoteAuthState::FAILED, Core::I18n::getInstance().get("login.status.init_rsa_failed"));
	} else if (sessionWanted) {
		serve();
	}

	workerRunning = false;
}

void RemoteAuth::serve() {
	heartbeatInterval = 0;

	ws.setOnMessage([this](std::string &message) { handleMessage(message); });

	ws.setOnError([this](const std::string &error) {
		Logger::log("[RemoteAuth] Error: %s", error.c_str());
		setState(RemoteAuthState::FAILED, Core::I18n::getInstance().get("login.status.connection_error") + error);
	});

	ws.setOnClose([this](int code, const std::string &reason) {
		Logger::log("[RemoteAuth] Connection closed: %d - %s", code, reason.c_str());
		if (state != RemoteAuthState::COMPLETED && state != RemoteAuthState::CANCELLED) {
			retryImmediately = code == CLOSE_SESSION_TIMEOUT;
			setState(RemoteAuthState::FAILED, Core::I18n::getInstance().get("login.status.connection_closed"));
		}
	});

	if (!ws.connect(GATEWAY_URL)) {
		setState(RemoteAuthState::FAILED, Core::I18n::getInstance().get("login.status.failed_connect"));
		return;
	}

	while (sessionWanted && ws.isConnected()) {
		const bool received = ws.poll();

		const uint64_t now = osGetTime();
		if (heartbeatInterval > 0 && now - lastHeartbeat >= heartbeatInterval) {
			sendHeartbeat();
			lastHeartbeat = now;
		}

		svcSleepThread(received ? Network::WebSocketClient::BUSY_POLL_NS : Network::WebSocketClient::IDLE_POLL_NS);
	}

	ws.disconnect();
}

void RemoteAuth::cancel() {
	Logger::log("[RemoteAuth] cancel() called, state: %d", (int)state.load());
	sessionWanted = false;

	if (state == RemoteAuthState::IDLE || state == RemoteAuthState::COMPLETED || state == RemoteAuthState::FAILED) {
		return;
	}

	fingerprint = "";
	setState(RemoteAuthState::CANCELLED, Core::I18n::getInstance().get("login.status.mobile_cancelled"));
	ws.disconnect();
}

void RemoteAuth::poll() {
	std::deque<std::function<void()>> pending;
	{
		std::lock_guard<std::mutex> lock(eventMutex);
		pending.swap(events);
	}
	for (const std::function<void()> &event : pending) {
		event();
	}

	if (state == RemoteAuthState::CONNECTING && sessionWanted) {
		launchWorker();
	}

	if (state != RemoteAuthState::FAILED && state != RemoteAuthState::CANCELLED) {
		retryAt = 0;
		return;
	}

	const uint64_t now = osGetTime();
	if (retryAt == 0) {
		retryAt = now + (retryImmediately.exchange(false) ? 0 : retryDelay);
	}
	if (now >= retryAt) {
		Logger::log("[RemoteAuth] Auto-retrying...");
		start();
	}
}

std::string RemoteAuth::getQRCodeUrl() const {
	if (fingerprint.empty()) {
		return "";
	}
	return "https://discord.com/ra/" + fingerprint;
}

void RemoteAuth::setOnStateChange(std::function<void(RemoteAuthState, const std::string &)> callback) {
	onStateChange = callback;
}

void RemoteAuth::setOnUserScanned(std::function<void(const RemoteAuthUser &)> callback) { onUserScanned = callback; }

void RemoteAuth::setOnTokenReceived(std::function<void(const std::string &)> callback) { onTokenReceived = callback; }

void RemoteAuth::handleMessage(std::string &message) {
	rapidjson::Document doc;
	doc.ParseInsitu<rapidjson::kParseDefaultFlags | rapidjson::kParseInsituFlag>(&message[0]);

	if (doc.HasParseError() || !doc.IsObject()) {
		Logger::log("[RemoteAuth] Failed to parse message");
		return;
	}

	std::string op = Utils::Json::getString(doc, "op");

	if (op == "hello") {
		if (doc.HasMember("heartbeat_interval") && doc["heartbeat_interval"].IsUint64()) {
			heartbeatInterval = doc["heartbeat_interval"].GetUint64();
			Logger::log("[RemoteAuth] Heartbeat interval: %llu ms", heartbeatInterval);
		}

		lastHeartbeat = osGetTime();
		sendHeartbeat();

		Logger::log("[RemoteAuth] Sending init message with public key");
		rapidjson::Document initDoc;
		initDoc.SetObject();
		rapidjson::Document::AllocatorType &allocator = initDoc.GetAllocator();
		initDoc.AddMember("op", "init", allocator);
		initDoc.AddMember("encoded_public_key", rapidjson::Value(publicKeyBase64.c_str(), allocator), allocator);

		rapidjson::StringBuffer buffer;
		rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
		initDoc.Accept(writer);
		ws.send(buffer.GetString());

	} else if (op == "nonce_proof") {
		std::string encryptedNonce = Utils::Json::getString(doc, "encrypted_nonce");
		std::string decryptedNonce = decryptNonce(encryptedNonce);
		if (decryptedNonce.empty()) {
			setState(RemoteAuthState::FAILED, "Nonce decryption failed");
			return;
		}

		rapidjson::Document proofDoc;
		proofDoc.SetObject();
		rapidjson::Document::AllocatorType &proofAllocator = proofDoc.GetAllocator();
		proofDoc.AddMember("op", "nonce_proof", proofAllocator);
		proofDoc.AddMember("nonce", rapidjson::Value(decryptedNonce.c_str(), proofAllocator), proofAllocator);

		rapidjson::StringBuffer proofBuffer;
		rapidjson::Writer<rapidjson::StringBuffer> proofWriter(proofBuffer);
		proofDoc.Accept(proofWriter);
		ws.send(proofBuffer.GetString());

	} else if (op == "pending_remote_init") {
		const std::string received = Utils::Json::getString(doc, "fingerprint");
		if (!received.empty()) {
			post([this, received] { fingerprint = received; });
			setState(RemoteAuthState::WAITING_FOR_SCAN, Core::I18n::getInstance().get("login.status.login"));
		}

	} else if (op == "pending_ticket") {
		if (doc.HasMember("encrypted_user_payload") && doc["encrypted_user_payload"].IsString()) {
			setState(RemoteAuthState::WAITING_FOR_CONFIRM, Core::I18n::getInstance().get("login.status.user_scanned"));

			post([this] {
				if (onUserScanned) {
					Discord::RemoteAuthUser user;
					user.username = "User";
					user.discriminator = "";
					user.id = "";
					onUserScanned(user);
				}
			});
		}

	} else if (op == "pending_login") {
		const std::string ticket = Utils::Json::getString(doc, "ticket");
		if (!ticket.empty()) {
			finish(ticket);
		}

	} else if (op == "pending_finish") {
		if (doc.HasMember("encrypted_user_payload") && doc["encrypted_user_payload"].IsString()) {
			setState(RemoteAuthState::WAITING_FOR_CONFIRM, "Confirming...");
		}

		std::string ticket = Utils::Json::getString(doc, "encrypted_token");
		if (ticket.empty()) {
			ticket = Utils::Json::getString(doc, "ticket");
		}

		if (!ticket.empty()) {
			finish(ticket);
		}

	} else if (op == "cancel") {
		retryImmediately = true;
		setState(RemoteAuthState::CANCELLED, Core::I18n::getInstance().get("login.status.mobile_cancelled"));
		ws.disconnect();
	}
}

void RemoteAuth::finish(const std::string &ticket) {
	setState(RemoteAuthState::COMPLETED, Core::I18n::getInstance().get("login.status.auth_completed"));
	post([this, ticket] {
		if (onTokenReceived) {
			onTokenReceived(ticket);
		}
	});
}

void RemoteAuth::sendHeartbeat() {
	rapidjson::Document doc;
	doc.SetObject();
	rapidjson::Document::AllocatorType &allocator = doc.GetAllocator();
	doc.AddMember("op", "heartbeat", allocator);

	rapidjson::StringBuffer buffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
	doc.Accept(writer);
	ws.send(buffer.GetString());
}

void RemoteAuth::post(std::function<void()> event) {
	std::lock_guard<std::mutex> lock(eventMutex);
	events.push_back(std::move(event));
}

void RemoteAuth::setState(RemoteAuthState newState, const std::string &info) {
	state = newState;
	post([this, newState, info] {
		if (onStateChange) {
			onStateChange(newState, info);
		}
	});
}

bool RemoteAuth::initRSA() {
	Logger::log("[RemoteAuth] Generating new RSA-2048 key...");

	rsaContext = new mbedtls_pk_context();
	mbedtls_pk_context *pk = (mbedtls_pk_context *)rsaContext;
	mbedtls_pk_init(pk);

	mbedtls_ctr_drbg_context ctr_drbg;
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_init(&ctr_drbg);
	mbedtls_entropy_init(&entropy);

	const std::string pers = "discord_remote_auth" + std::to_string(osGetTime());
	int ret = mbedtls_entropy_add_source(&entropy, consoleEntropy, nullptr, MBEDTLS_ENTROPY_MAX_GATHER,
	                                     MBEDTLS_ENTROPY_SOURCE_STRONG);
	if (ret == 0) {
		ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, (const unsigned char *)pers.c_str(),
		                            pers.length());
	}
	if (ret == 0) {
		ret = mbedtls_pk_setup(pk, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
	}
	if (ret == 0) {
		ret = generateKey(mbedtls_pk_rsa(*pk), &ctr_drbg);
	}

	mbedtls_ctr_drbg_free(&ctr_drbg);
	mbedtls_entropy_free(&entropy);

	if (ret != 0) {
		Logger::log("[RemoteAuth] RSA key generation failed: -0x%x", -ret);
		cleanupRSA();
		return false;
	}

	mbedtls_rsa_set_padding(mbedtls_pk_rsa(*pk), MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA256);

	publicKeyBase64 = exportPublicKeyBase64();
	if (publicKeyBase64.empty()) {
		cleanupRSA();
		return false;
	}

	return true;
}

void RemoteAuth::cleanupRSA() {
	if (rsaContext) {
		mbedtls_pk_free((mbedtls_pk_context *)rsaContext);
		delete (mbedtls_pk_context *)rsaContext;
		rsaContext = nullptr;
	}
}

std::string RemoteAuth::exportPublicKeyBase64() {
	if (!rsaContext) {
		return "";
	}

	mbedtls_pk_context *pk = (mbedtls_pk_context *)rsaContext;
	unsigned char buf[4096];
	int ret = mbedtls_pk_write_pubkey_der(pk, buf, sizeof(buf));
	if (ret < 0) {
		return "";
	}

	unsigned char *der_start = buf + sizeof(buf) - ret;
	return Utils::Base64::encode(der_start, ret);
}

std::string RemoteAuth::decrypt(const std::string &encryptedBase64) {
	if (!rsaContext) {
		return "";
	}

	mbedtls_rsa_context *rsa = mbedtls_pk_rsa(*(mbedtls_pk_context *)rsaContext);
	std::vector<unsigned char> encrypted = Utils::Base64::decode(encryptedBase64);
	if (encrypted.size() != mbedtls_rsa_get_len(rsa)) {
		return "";
	}

	unsigned char decrypted[256];
	size_t olen = 0;

	// No RNG: exponent blinding assumes P is prime, and here it is not.
	int ret = mbedtls_rsa_rsaes_oaep_decrypt(rsa, nullptr, nullptr, MBEDTLS_RSA_PRIVATE, nullptr, 0, &olen,
	                                         encrypted.data(), decrypted, sizeof(decrypted));
	if (ret != 0) {
		return "";
	}

	return std::string((char *)decrypted, olen);
}

std::string RemoteAuth::decryptNonce(const std::string &encryptedNonceBase64) {
	const std::string nonce = decrypt(encryptedNonceBase64);
	if (nonce.empty()) {
		return "";
	}

	std::string encoded = Utils::Base64::encode((const unsigned char *)nonce.data(), nonce.size());
	for (char &c : encoded) {
		if (c == '+') {
			c = '-';
		} else if (c == '/') {
			c = '_';
		}
	}
	size_t padPos = encoded.find('=');
	if (padPos != std::string::npos) {
		encoded = encoded.substr(0, padPos);
	}

	return encoded;
}

std::string RemoteAuth::decryptToken(const std::string &encryptedTokenBase64) { return decrypt(encryptedTokenBase64); }

} // namespace Discord
