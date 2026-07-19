#include "discord/dave_session.h"
#include "log.h"

#include <dave/dave_interfaces.h>
#include <dave/logger.h>
#include <mls/crypto.h>

#include "decryptor.h"
#include "encryptor.h"
#include "mls/session.h"

#include <map>
#include <mutex>

namespace Discord {

namespace {
constexpr uint16_t DAVE_PROTOCOL_VERSION = 1;

void daveLogSink(discord::dave::LoggingSeverity severity, const char *file, int line, const std::string &message) {
	(void)file;
	(void)line;
	const char *level = (severity >= discord::dave::LS_ERROR) ? "ERR" : "inf";
	Logger::log("[libdave/%s] %.400s", level, message.c_str());
}
} // namespace

struct DaveSession::Impl {
	std::unique_ptr<discord::dave::mls::Session> session;
	std::unique_ptr<discord::dave::Encryptor> encryptor;
	std::map<std::string, std::unique_ptr<discord::dave::Decryptor>> decryptors;
	std::shared_ptr<::mlspp::SignaturePrivateKey> transientKey;

	uint16_t version = 0;
	bool grouped = false;
	std::string selfUserId;
	std::string error;
	std::mutex mutex;
};

DaveSession::DaveSession() : impl(std::make_unique<Impl>()) { discord::dave::SetLogSink(daveLogSink); }

DaveSession::~DaveSession() = default;

bool DaveSession::init(uint16_t protocolVersion, uint64_t groupId, const std::string &selfUserId) {
	std::lock_guard<std::mutex> lock(impl->mutex);

	if (protocolVersion != DAVE_PROTOCOL_VERSION) {
		impl->error = "unsupported DAVE protocol version " + std::to_string(protocolVersion);
		Logger::log("[DAVE] %s", impl->error.c_str());
		return false;
	}

	impl->session = std::make_unique<discord::dave::mls::Session>(
	    nullptr, "", [this](std::string const &reason, std::string const &stack) {
		    impl->error = reason;
		    Logger::log("[DAVE] MLS failure: %s (%s)", reason.c_str(), stack.c_str());
	    });

	impl->session->Init(protocolVersion, groupId, selfUserId, impl->transientKey);
	impl->encryptor = std::make_unique<discord::dave::Encryptor>();
	impl->decryptors.clear();
	impl->selfUserId = selfUserId;
	impl->version = protocolVersion;
	impl->grouped = false;

	Logger::log("[DAVE] Session init v%u group=%llu user=%s", protocolVersion, (unsigned long long)groupId,
	            selfUserId.c_str());
	return true;
}

void DaveSession::reset() {
	std::lock_guard<std::mutex> lock(impl->mutex);
	if (impl->session) {
		impl->session->Reset();
	}
	impl->decryptors.clear();
	impl->grouped = false;
}

uint16_t DaveSession::protocolVersion() const { return impl->version; }

bool DaveSession::hasGroup() const { return impl->grouped; }

void DaveSession::setExternalSender(const std::vector<uint8_t> &package) {
	std::lock_guard<std::mutex> lock(impl->mutex);
	if (!impl->session) {
		return;
	}
	impl->session->SetExternalSender(package);
	Logger::log("[DAVE] External sender set (%zu bytes)", package.size());
}

std::vector<uint8_t> DaveSession::keyPackage() {
	std::lock_guard<std::mutex> lock(impl->mutex);
	if (!impl->session) {
		return {};
	}
	auto kp = impl->session->GetMarshalledKeyPackage();
	Logger::log("[DAVE] Key package produced (%zu bytes)", kp.size());
	return kp;
}

bool DaveSession::processProposals(const std::vector<uint8_t> &proposals,
                                   const std::set<std::string> &recognizedUserIds,
                                   std::vector<uint8_t> &commitWelcomeOut) {
	std::lock_guard<std::mutex> lock(impl->mutex);
	if (!impl->session) {
		return false;
	}

	auto result = impl->session->ProcessProposals(proposals, recognizedUserIds);
	if (!result) {
		impl->error = "proposals rejected";
		return false;
	}

	commitWelcomeOut = *result;
	return true;
}

bool DaveSession::processCommit(const std::vector<uint8_t> &commit) {
	std::lock_guard<std::mutex> lock(impl->mutex);
	if (!impl->session) {
		return false;
	}

	auto roster = impl->session->ProcessCommit(commit);
	if (!std::holds_alternative<discord::dave::RosterMap>(roster)) {
		impl->error = "commit rejected";
		Logger::log("[DAVE] Commit rejected");
		return false;
	}

	impl->grouped = true;
	return true;
}

bool DaveSession::processWelcome(const std::vector<uint8_t> &welcome, const std::set<std::string> &recognizedUserIds) {
	std::lock_guard<std::mutex> lock(impl->mutex);
	if (!impl->session) {
		return false;
	}

	auto roster = impl->session->ProcessWelcome(welcome, recognizedUserIds);
	if (!roster) {
		impl->error = "welcome rejected";
		Logger::log("[DAVE] Welcome rejected");
		return false;
	}

	impl->grouped = true;
	Logger::log("[DAVE] Joined group, %zu members", roster->size());
	return true;
}

void DaveSession::refreshKeyRatchets(const std::set<std::string> &userIds) {
	std::lock_guard<std::mutex> lock(impl->mutex);
	if (!impl->session || !impl->encryptor) {
		return;
	}

	auto selfRatchet = impl->session->GetKeyRatchet(impl->selfUserId);
	if (selfRatchet) {
		impl->encryptor->SetKeyRatchet(std::move(selfRatchet));
		Logger::log("[DAVE] Encryptor key ratchet set");
	} else {
		Logger::log("[DAVE] No key ratchet available for self (%s)", impl->selfUserId.c_str());
	}

	for (const auto &userId : userIds) {
		if (userId == impl->selfUserId) {
			continue;
		}

		auto ratchet = impl->session->GetKeyRatchet(userId);
		if (!ratchet) {
			continue;
		}

		auto &decryptor = impl->decryptors[userId];
		if (!decryptor) {
			decryptor = std::make_unique<discord::dave::Decryptor>();
		}
		decryptor->TransitionToKeyRatchet(std::move(ratchet));
	}
}

void DaveSession::setPassthrough(bool enabled) {
	std::lock_guard<std::mutex> lock(impl->mutex);
	if (impl->encryptor) {
		impl->encryptor->SetPassthroughMode(enabled);
	}
	for (auto &pair : impl->decryptors) {
		pair.second->TransitionToPassthroughMode(enabled);
	}
}

size_t DaveSession::maxCiphertextSize(size_t frameSize) const {
	if (!impl->encryptor) {
		return frameSize;
	}
	return impl->encryptor->GetMaxCiphertextByteSize(discord::dave::MediaType::Audio, frameSize);
}

bool DaveSession::encryptOpus(uint32_t ssrc, const uint8_t *frame, size_t frameLen, uint8_t *out, size_t outCapacity,
                              size_t *written) {
	std::lock_guard<std::mutex> lock(impl->mutex);
	if (!impl->encryptor) {
		return false;
	}

	// Encrypt writes the tag past the frame without bounds checking, so the
	// caller's buffer has to be at least the size libdave asks for.
	const auto needed = impl->encryptor->GetMaxCiphertextByteSize(discord::dave::MediaType::Audio, frameLen);
	if (outCapacity < needed) {
		Logger::log("[DAVE] Encrypt buffer too small: have %zu, need %zu", outCapacity, needed);
		return false;
	}

	impl->encryptor->AssignSsrcToCodec(ssrc, discord::dave::Codec::Opus);

	const auto rc =
	    impl->encryptor->Encrypt(discord::dave::MediaType::Audio, ssrc, discord::dave::MakeArrayView(frame, frameLen),
	                             discord::dave::MakeArrayView(out, outCapacity), written);
	return rc == discord::dave::Encryptor::ResultCode::Success;
}

bool DaveSession::decryptOpus(const std::string &userId, const uint8_t *frame, size_t frameLen, uint8_t *out,
                              size_t outCapacity, size_t *written) {
	std::lock_guard<std::mutex> lock(impl->mutex);

	auto it = impl->decryptors.find(userId);
	if (it == impl->decryptors.end() || !it->second) {
		return false;
	}

	const auto rc = it->second->Decrypt(discord::dave::MediaType::Audio, discord::dave::MakeArrayView(frame, frameLen),
	                                    discord::dave::MakeArrayView(out, outCapacity), written);
	return rc == discord::dave::Decryptor::ResultCode::Success;
}

std::string DaveSession::lastError() const { return impl->error; }

} // namespace Discord
