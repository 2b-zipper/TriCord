#ifndef DAVE_SESSION_H
#define DAVE_SESSION_H

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace Discord {

// Built with exceptions and RTTI, which the rest of TriCord is not: no libdave
// or mlspp type may appear here and no exception may cross this boundary.
class DaveSession {
  public:
	DaveSession();
	~DaveSession();

	DaveSession(const DaveSession &) = delete;
	DaveSession &operator=(const DaveSession &) = delete;

	bool init(uint16_t protocolVersion, uint64_t groupId, const std::string &selfUserId);
	void reset();

	uint16_t protocolVersion() const;
	bool hasGroup() const;

	void setExternalSender(const std::vector<uint8_t> &package);
	std::vector<uint8_t> keyPackage();
	bool processProposals(const std::vector<uint8_t> &proposals, const std::set<std::string> &recognizedUserIds,
	                      std::vector<uint8_t> &commitWelcomeOut);
	bool processCommit(const std::vector<uint8_t> &commit);
	bool processWelcome(const std::vector<uint8_t> &welcome, const std::set<std::string> &recognizedUserIds);

	void refreshKeyRatchets(const std::set<std::string> &userIds);
	void setPassthrough(bool enabled);

	size_t maxCiphertextSize(size_t frameSize) const;
	bool encryptOpus(uint32_t ssrc, const uint8_t *frame, size_t frameLen, uint8_t *out, size_t outCapacity,
	                 size_t *written);
	bool decryptOpus(const std::string &userId, const uint8_t *frame, size_t frameLen, uint8_t *out, size_t outCapacity,
	                 size_t *written);

	std::string lastError() const;

  private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

} // namespace Discord

#endif // DAVE_SESSION_H
