#ifndef MESSAGE_SCREEN_H
#define MESSAGE_SCREEN_H

#include "discord/types.h"
#include "ui/screen_manager.h"
#include "ui/emoji_picker.h"
#include "ui/markdown_renderer.h"
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace UI {

class MessageScreen : public Screen {
  public:
	struct EmbedLayout {
		bool hasImage;
		bool hasThumbnail;
		bool isLargeThumbnail;
		bool isMedia;
		bool isSimpleMedia;
		bool showThumbnailOnRight;
		float pixelWidth;
	};

	struct EmbedRenderCache {
		float height = 0.0f;
		EmbedLayout layout;
		UI::MarkdownRenderer::LayoutRef providerLayout;
		UI::MarkdownRenderer::LayoutRef authorLayout;
		UI::MarkdownRenderer::LayoutRef titleLayout;
		UI::MarkdownRenderer::LayoutRef descriptionLayout;
		std::vector<std::pair<UI::MarkdownRenderer::LayoutRef, UI::MarkdownRenderer::LayoutRef>> fieldLayouts;
		UI::MarkdownRenderer::LayoutRef footerLayout;
	};

	struct MessageRenderCache {
		float position = 0.0f;
		float height = 0.0f;
		bool showHeader = false;
		bool showDateSeparator = false;
		std::string dateString;
		bool isEmojiOnly = false;
		int emojiCount = 0;
		UI::MarkdownRenderer::LayoutRef contentLayout;
		std::vector<EmbedRenderCache> embeds;
		bool canGroupWithPrev = false;
		// Resolved in renderTop: isUserMentioned locks the client mutex, which messageMutex holders must not.
		int8_t mentionState = -1;
		std::string headerTimestamp;
		float pollHeight = 0.0f;
		std::vector<std::string> pollQuestionLines;

		std::string msgId;
		std::string prevId;
		std::string dateKey;
		size_t fingerprint = 0;
		bool dependsOnImages = false;
		uint32_t imageGeneration = 0;
	};

	MessageScreen(const std::string &channelId, const std::string &channelName);
	virtual ~MessageScreen();

	void update() override;
	void renderTop(C3D_RenderTarget *target) override;
	void renderBottom(C3D_RenderTarget *target) override;
	void onEnter() override;
	bool hidesMenu() const override;

  private:
	std::string channelId;
	std::string channelName;
	std::string truncatedChannelName;
	int channelType;
	std::string rulesChannelId;
	std::string channelTopic;
	std::string guildId;
	std::vector<Discord::Message> messages;
	int selectedIndex;
	std::recursive_mutex messageMutex;
	bool isLoading;
	bool isFetchingHistory;
	bool requestHistoryFetch;
	bool scrollInitialized;
	bool showNewMessageIndicator;
	int newMessageCount;
	bool isForumView;
	bool isHiddenChannel = false;
	bool hasMoreHistory;
	uint32_t lastImageGeneration;
	int64_t cacheDayStamp = -1;

	std::string dmRecipientId;
	std::string dmRecipientAvatar;
	std::string dmRecipientDiscriminator;
	std::string groupIconHash;
	std::string cachedHints;
	int cachedHintsKey = -1;
	bool canSendCached = false;
	int canSendRecheck = 0;

	int keyRepeatTimer;
	static const int REPEAT_INITIAL_DELAY = 25;
	static const int REPEAT_INTERVAL = 8;

	std::vector<float> messagePositions;
	std::vector<float> messageHeights;
	std::unordered_map<size_t, float> embedHeightCache;
	std::vector<MessageRenderCache> renderCaches;
	std::set<std::string> revealedSpoilers;
	float targetScrollY;
	float currentScrollY;
	float totalContentHeight;

	float bottomScrollY = 0.0f;
	touchPosition lastTouch = {0, 0};
	bool isDraggingBottom = false;

	bool isMenuOpen;
	int menuIndex;
	bool pollMode = false;
	int pollAnswerIndex = 0;
	bool wasAtBottom = false;
	std::vector<std::string> menuOptions;
	std::vector<std::string> menuActions;
	std::set<std::string> pendingMemberFetches;
	std::vector<std::string> queuedMemberFetches;
	void flushMemberFetches();
	u32 authorNameColor(const Discord::Message &msg);
	std::map<std::string, uint64_t> failedMemberFetches;
	std::shared_ptr<bool> aliveToken;
	enum class BottomScreenMode { TOPIC, EMOJI_PICKER };
	BottomScreenMode bottomMode;

	std::unique_ptr<EmojiPicker> emojiPicker;

	void renderMenu();
	std::unordered_set<std::string> getVisibleTwemojis();

	void fetchMessages();
	void fetchOlderMessages();
	float drawMessage(const Discord::Message &msg, float y, float maxWidth, bool isSelected, bool showHeader, bool prevGroupedMention = false, bool nextGroupedMention = false, const MessageRenderCache *renderCache = nullptr);
	float drawForumMessage(const Discord::Message &msg, float y, bool isSelected);
	float drawSystemMessage(const Discord::Message &msg, float y, float topMargin, float height, bool isSelected);
	float drawReplyPreview(const Discord::Message &msg, float x, float y);
	float drawForwardHeader(const Discord::Message &msg, float x, float y);
	float drawAuthorHeader(const Discord::Message &msg, float x, float y, bool showHeader,
	                       const MessageRenderCache *renderCache = nullptr);
	float drawMessageContent(const Discord::Message &msg, float x, float y, const MessageRenderCache *renderCache = nullptr);
	float drawAttachments(const Discord::Message &msg, float x, float y, float maxWidth);
	float drawStickers(const Discord::Message &msg, float x, float y, float maxWidth);
	float drawReactions(const Discord::Message &msg, float x, float y, bool isSelected);
	float drawPoll(const Discord::Message &msg, float x, float y, float maxWidth, bool isSelected,
	               const MessageRenderCache *renderCache = nullptr);
	static size_t messageFingerprint(const Discord::Message &msg);
	void buildMessageCache(const Discord::Message &msg, MessageRenderCache &cache);
	float calculatePollHeight(const Discord::Poll &poll, float maxWidth);
	void submitPollVote(int answerIndex);
	float calculateMessageHeight(const Discord::Message &msg, bool showHeader);
	float calculateEmbedHeight(const Discord::Embed &embed, float maxWidth);
	float renderEmbed(const Discord::Embed &embed, float x, float y, float maxWidth, const EmbedRenderCache *embedCache = nullptr);
	void openKeyboard();
	void showMessageOptions();

	void scrollToBottom();
	void rebuildLayoutCache();
	void ensureSelectionVisible();
	void catchUpMessages();
	bool isAtBottom() const;
	std::string getLatestRealMessageId() const;
	void checkAndMarkChannelRead();
	void syncScrollAfterRebuild(bool wasAtBottom, bool updateSelection = false);

	Discord::Message createOptimisticMessage(const std::string &content, int type = 0, const std::string &referencedAuthor = "");
	void handleMessageSendResult(const std::string &pendingId, const Discord::Message &sentMsg, bool success, int errorCode);
	void onMessageCreate(const Discord::Message &msg);
	void onMessageUpdate(const Discord::Message &msg);
	void onMessageDelete(const std::string &msgId);

	struct KeyboardResult {
		int button;
		std::string text;
	};
	KeyboardResult runKeyboard(const std::string &hint, const std::string &initialText = "");

	void renderReactionIcon();
	void renderCallParticipants(float y, const std::vector<Discord::VoiceParticipant> &participants);
	void renderDmProfile(float y);
	std::vector<Discord::VoiceParticipant> callParticipants() const;
	bool isCallableChannel() const;
	bool isCallActive() const;
	void startCall();
};

} // namespace UI

#endif // MESSAGE_SCREEN_H
