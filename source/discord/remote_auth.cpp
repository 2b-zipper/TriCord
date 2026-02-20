#include "discord/remote_auth.h"
#include "core/config.h"
#include "core/i18n.h"
#include "log.h"
#include "utils/base64_utils.h"
#include "utils/file_utils.h"
#include "utils/json_utils.h"
#include <3ds.h>
#include <sys/stat.h>

#include <cstring>
#include <mbedtls/base64.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace Discord {

RemoteAuth::RemoteAuth()
    : state(RemoteAuthState::IDLE), heartbeatInterval(41250), lastHeartbeat(0),
      lastRetryTime(0), rsaContext(nullptr), ctrDrbgContext(nullptr),
      entropyContext(nullptr), isInitializing(false), initSuccess(false) {}

RemoteAuth::~RemoteAuth() {
  cancel();
  cleanupRSA();
}

bool RemoteAuth::start() {
  if (state != RemoteAuthState::IDLE && state != RemoteAuthState::FAILED &&
      state != RemoteAuthState::CANCELLED &&
      state != RemoteAuthState::COMPLETED) {
    Logger::log("[RemoteAuth] Already in progress (state: %d)", (int)state);
    return false;
  }

  Logger::log("[RemoteAuth] Starting remote auth");
  fingerprint = "";
  ticket = "";

  if (initSuccess) {
    Logger::log("[RemoteAuth] Keys already generated, proceeding to connect");
    setState(RemoteAuthState::CONNECTING,
             Core::I18n::getInstance().get("login.status.connecting_auth"));
  }

  if (!initSuccess) {
    setState(RemoteAuthState::CONNECTING,
             Core::I18n::getInstance().get("login.status.generating_keys"));
    prepare();
  } else {
    setState(RemoteAuthState::CONNECTING,
             Core::I18n::getInstance().get("login.status.connecting_auth"));
  }

  return true;
}

void RemoteAuth::prepare() {
  if (isInitializing) {
    Logger::log("[RemoteAuth] Setup already in progress");
    return;
  }

  Logger::log("[RemoteAuth] Preparing RSA keys (Background)...");
  isInitializing = true;
  initSuccess = false;

  if (workerThread.joinable()) {
    workerThread.join();
  }

  workerThread = std::thread([this]() { runInit(); });
}

void RemoteAuth::runInit() {
  Logger::log("[RemoteAuth] Init started (Background)");

  if (initRSA()) {
    Logger::log("[RemoteAuth] RSA initialization successful");
    initSuccess = true;
  } else {
    Logger::log("[RemoteAuth] RSA initialization failed");
    initSuccess = false;
  }

  isInitializing = false;
}

void RemoteAuth::cancel() {
  Logger::log("[RemoteAuth] cancel() called, state: %d", (int)state);

  if (state == RemoteAuthState::IDLE || state == RemoteAuthState::COMPLETED ||
      state == RemoteAuthState::FAILED) {
    Logger::log("[RemoteAuth] cancel() early return due to state");
    return;
  }

  Logger::log("[RemoteAuth] cancel() calling ws.disconnect()");
  ws.disconnect();
  Logger::log("[RemoteAuth] cancel() ws.disconnect() completed");

  Logger::log("[RemoteAuth] cancel() calling setState()");
  fingerprint = "";
  setState(RemoteAuthState::CANCELLED,
           Core::I18n::getInstance().get("login.status.mobile_cancelled"));
  lastRetryTime = osGetTime();
  Logger::log("[RemoteAuth] cancel() setState() completed");
}

void RemoteAuth::poll() {
  ws.poll();

  if (state == RemoteAuthState::CONNECTING && !isInitializing) {
    if (!initSuccess) {
      setState(RemoteAuthState::FAILED,
               Core::I18n::getInstance().get("login.status.init_rsa_failed"));
      return;
    }

    if (ws.getState() == Network::WebSocketState::DISCONNECTED ||
        ws.getState() == Network::WebSocketState::CLOSED) {

      setState(RemoteAuthState::CONNECTING,
               Core::I18n::getInstance().get("login.status.connecting_auth"));

      ws.setOnMessage([this](std::string &message) { handleMessage(message); });

      ws.setOnError([this](const std::string &error) {
        Logger::log("[RemoteAuth] Error: %s", error.c_str());
        setState(RemoteAuthState::FAILED, Core::I18n::getInstance().get(
                                              "login.status.connection_error") +
                                              error);
      });

      ws.setOnClose([this](int code, const std::string &reason) {
        Logger::log("[RemoteAuth] Connection closed: %d - %s", code,
                    reason.c_str());
        if (state != RemoteAuthState::COMPLETED &&
            state != RemoteAuthState::CANCELLED) {
          setState(
              RemoteAuthState::FAILED,
              Core::I18n::getInstance().get("login.status.connection_closed"));
          lastRetryTime = osGetTime();
        }
      });

      std::string url = "wss://remote-auth-gateway.discord.gg/?v=2";
      Logger::log("[RemoteAuth] Connecting to %s", url.c_str());

      bool connected = ws.connect(url);
      if (!connected) {
        setState(RemoteAuthState::FAILED,
                 Core::I18n::getInstance().get("login.status.failed_connect"));
      }
    }
  }

  // Auto-retry if failed or cancelled
  if (state == RemoteAuthState::FAILED || state == RemoteAuthState::CANCELLED) {
    uint64_t now = osGetTime();
    if (now - lastRetryTime >= retryDelay) {
      Logger::log("[RemoteAuth] Auto-retrying...");
      start();
    }
  }

  if (heartbeatInterval > 0) {
    uint64_t now = osGetTime();
    if (now - lastHeartbeat >= heartbeatInterval) {
      sendHeartbeat();
      lastHeartbeat = now;
    }
  }
}

std::string RemoteAuth::getQRCodeUrl() const {
  if (fingerprint.empty()) {
    return "";
  }
  return "https://discord.com/ra/" + fingerprint;
}

void RemoteAuth::setOnStateChange(
    std::function<void(RemoteAuthState, const std::string &)> callback) {
  onStateChange = callback;
}

void RemoteAuth::setOnUserScanned(
    std::function<void(const RemoteAuthUser &)> callback) {
  onUserScanned = callback;
}

void RemoteAuth::setOnTokenReceived(
    std::function<void(const std::string &)> callback) {
  onTokenReceived = callback;
}

void RemoteAuth::handleMessage(std::string &message) {
  rapidjson::Document doc;
  doc.ParseInsitu<rapidjson::kParseDefaultFlags | rapidjson::kParseInsituFlag>(
      &message[0]);

  if (doc.HasParseError() || !doc.IsObject()) {
    Logger::log("[RemoteAuth] Failed to parse message");
    return;
  }

  std::string op = Utils::Json::getString(doc, "op");

  if (op == "hello") {
    if (doc.HasMember("heartbeat_interval") &&
        doc["heartbeat_interval"].IsUint64()) {
      heartbeatInterval = doc["heartbeat_interval"].GetUint64();
      Logger::log("[RemoteAuth] Heartbeat interval: %llu ms",
                  heartbeatInterval);
    }

    lastHeartbeat = osGetTime();
    sendHeartbeat();

    Logger::log("[RemoteAuth] Sending init message with public key");
    rapidjson::Document initDoc;
    initDoc.SetObject();
    rapidjson::Document::AllocatorType &allocator = initDoc.GetAllocator();
    initDoc.AddMember("op", "init", allocator);
    initDoc.AddMember("encoded_public_key",
                      rapidjson::Value(publicKeyBase64.c_str(), allocator),
                      allocator);

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
    rapidjson::Document::AllocatorType &proofAllocator =
        proofDoc.GetAllocator();
    proofDoc.AddMember("op", "nonce_proof", proofAllocator);
    proofDoc.AddMember("nonce",
                       rapidjson::Value(decryptedNonce.c_str(), proofAllocator),
                       proofAllocator);

    rapidjson::StringBuffer proofBuffer;
    rapidjson::Writer<rapidjson::StringBuffer> proofWriter(proofBuffer);
    proofDoc.Accept(proofWriter);
    ws.send(proofBuffer.GetString());

  } else if (op == "pending_remote_init") {
    fingerprint = Utils::Json::getString(doc, "fingerprint");
    if (!fingerprint.empty()) {
      setState(RemoteAuthState::WAITING_FOR_SCAN,
               Core::I18n::getInstance().get("login.status.login"));
    }

  } else if (op == "pending_ticket") {
    if (doc.HasMember("encrypted_user_payload") &&
        doc["encrypted_user_payload"].IsString()) {
      setState(RemoteAuthState::WAITING_FOR_CONFIRM,
               Core::I18n::getInstance().get("login.status.user_scanned"));

      if (onUserScanned) {
        Discord::RemoteAuthUser user;
        user.username = "User";
        user.discriminator = "";
        user.id = "";
        onUserScanned(user);
      }
    }

  } else if (op == "pending_login") {
    ticket = Utils::Json::getString(doc, "ticket");
    if (!ticket.empty() && onTokenReceived) {
      setState(RemoteAuthState::COMPLETED,
               Core::I18n::getInstance().get("login.status.auth_completed"));
      onTokenReceived(ticket);
    }

  } else if (op == "pending_finish") {
    if (doc.HasMember("encrypted_user_payload") &&
        doc["encrypted_user_payload"].IsString()) {
      setState(RemoteAuthState::WAITING_FOR_CONFIRM, "Confirming...");
    }

    ticket = Utils::Json::getString(doc, "encrypted_token");
    if (ticket.empty()) {
      ticket = Utils::Json::getString(doc, "ticket");
    }

    if (!ticket.empty() && onTokenReceived) {
      setState(RemoteAuthState::COMPLETED,
               Core::I18n::getInstance().get("login.status.auth_completed"));
      onTokenReceived(ticket);
    }

  } else if (op == "cancel") {
    setState(RemoteAuthState::CANCELLED,
             Core::I18n::getInstance().get("login.status.mobile_cancelled"));
  }
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

void RemoteAuth::setState(RemoteAuthState newState, const std::string &info) {
  state = newState;
  if (onStateChange) {
    onStateChange(state, info);
  }
}

bool RemoteAuth::initRSA() {
  Logger::log("[RemoteAuth] Initializing unique RSA-2048");

  rsaContext = new mbedtls_pk_context();
  ctrDrbgContext = new mbedtls_ctr_drbg_context();
  entropyContext = new mbedtls_entropy_context();

  mbedtls_pk_context *pk = (mbedtls_pk_context *)rsaContext;
  mbedtls_ctr_drbg_context *ctr_drbg =
      (mbedtls_ctr_drbg_context *)ctrDrbgContext;
  mbedtls_entropy_context *entropy = (mbedtls_entropy_context *)entropyContext;

  mbedtls_pk_init(pk);
  mbedtls_ctr_drbg_init(ctr_drbg);
  mbedtls_entropy_init(entropy);

  const char *pers = "discord_remote_auth";
  std::string dynamic_pers = pers + std::to_string(osGetTime());

  int ret = mbedtls_ctr_drbg_seed(ctr_drbg, mbedtls_entropy_func, entropy,
                                  (const unsigned char *)dynamic_pers.c_str(),
                                  dynamic_pers.length());
  if (ret != 0) {
    cleanupRSA();
    return false;
  }

  bool generated = false;

  Logger::log("[RemoteAuth] Generating new RSA-2048 key...");
  ret = mbedtls_pk_setup(pk, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
  if (ret == 0) {
    ret = mbedtls_rsa_gen_key(mbedtls_pk_rsa(*pk), mbedtls_ctr_drbg_random,
                              ctr_drbg, 2048, 65537);
    if (ret == 0) {
      Logger::log("[RemoteAuth] RSA key generation successful");
      generated = true;
    } else {
      Logger::log("[RemoteAuth] RSA key generation failed: -0x%x", -ret);
    }
  }

  if (!generated) {
    cleanupRSA();
    return false;
  }

  mbedtls_rsa_context *rsa = mbedtls_pk_rsa(*pk);
  mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA256);

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
  if (ctrDrbgContext) {
    mbedtls_ctr_drbg_free((mbedtls_ctr_drbg_context *)ctrDrbgContext);
    delete (mbedtls_ctr_drbg_context *)ctrDrbgContext;
    ctrDrbgContext = nullptr;
  }
  if (entropyContext) {
    mbedtls_entropy_free((mbedtls_entropy_context *)entropyContext);
    delete (mbedtls_entropy_context *)entropyContext;
    entropyContext = nullptr;
  }
}

std::string RemoteAuth::exportPublicKeyBase64() {
  if (!rsaContext)
    return "";

  mbedtls_pk_context *pk = (mbedtls_pk_context *)rsaContext;
  unsigned char buf[4096];
  int ret = mbedtls_pk_write_pubkey_der(pk, buf, sizeof(buf));
  if (ret < 0)
    return "";

  unsigned char *der_start = buf + sizeof(buf) - ret;
  return Utils::Base64::encode(der_start, ret);
}

std::string RemoteAuth::decryptNonce(const std::string &encryptedNonceBase64) {
  if (!rsaContext)
    return "";

  std::vector<unsigned char> encrypted =
      Utils::Base64::decode(encryptedNonceBase64);
  if (encrypted.empty())
    return "";

  mbedtls_pk_context *pk = (mbedtls_pk_context *)rsaContext;
  mbedtls_ctr_drbg_context *ctr_drbg =
      (mbedtls_ctr_drbg_context *)ctrDrbgContext;

  mbedtls_rsa_context *rsa = mbedtls_pk_rsa(*pk);
  mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA256);

  unsigned char decrypted[256];
  size_t olen = 0;

  int ret = mbedtls_pk_decrypt(pk, encrypted.data(), encrypted.size(),
                               decrypted, &olen, sizeof(decrypted),
                               mbedtls_ctr_drbg_random, ctr_drbg);
  if (ret != 0)
    return "";

  std::string encoded = Utils::Base64::encode(decrypted, olen);
  for (char &c : encoded) {
    if (c == '+')
      c = '-';
    else if (c == '/')
      c = '_';
  }
  size_t padPos = encoded.find('=');
  if (padPos != std::string::npos) {
    encoded = encoded.substr(0, padPos);
  }

  return encoded;
}

std::string RemoteAuth::decryptToken(const std::string &encryptedTokenBase64) {
  if (!rsaContext || encryptedTokenBase64.empty())
    return "";

  std::vector<unsigned char> encrypted =
      Utils::Base64::decode(encryptedTokenBase64);
  if (encrypted.empty())
    return "";

  mbedtls_pk_context *pk = (mbedtls_pk_context *)rsaContext;
  mbedtls_ctr_drbg_context *ctr_drbg =
      (mbedtls_ctr_drbg_context *)ctrDrbgContext;

  mbedtls_rsa_context *rsa = mbedtls_pk_rsa(*pk);
  mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA256);

  unsigned char decrypted[256];
  size_t olen = 0;

  int ret = mbedtls_pk_decrypt(pk, encrypted.data(), encrypted.size(),
                               decrypted, &olen, sizeof(decrypted),
                               mbedtls_ctr_drbg_random, ctr_drbg);
  if (ret != 0)
    return "";

  return std::string((char *)decrypted, olen);
}

} // namespace Discord
