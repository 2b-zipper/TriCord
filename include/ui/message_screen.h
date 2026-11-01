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
		bool hasMedia;
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
		std::vector<UI::MarkdownRenderer::LayoutRef> blockLayouts;
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
	bool isAnimating() const override;
	void renderTop(C3D_RenderTarget *target) override;
	void renderBottom(C3D_RenderTarget *target) override;
	void onEnter() override;
	bool hidesMenu() const override;

  private:
	// One walk of a message's blocks, measuring or drawing, so that the two cannot disagree on the layout.
	struct BlockPass {
		const Discord::Message &msg;
		const MessageRenderCache *cache = nullptr;
		std::vector<UI::MarkdownRenderer::LayoutRef> *layouts = nullptr;
		bool draw = false;
		bool selected = false;
		bool revealed = false;
		int control = 0;
		size_t text = 0;
	};

	struct SelectPicker {
		bool open = false;
		bool multi = false;
		int cursor = 0;
		int first = 0;
		int minValues = 1;
		int maxValues = 1;
		std::vector<Discord::SelectOption> options;
		std::vector<std::string> chosen;
	};

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
	bool componentMode = false;
	int componentIndex = 0;
	float componentCursorTop = 0.0f;
	float componentCursorHeight = 0.0f;
	SelectPicker picker;
	bool modalOpen = false;
	int modalCursor = 0;
	int modalFirst = 0;
	Discord::Modal modal;
	std::string linkPopupUrl;
	bool interactionPending = false;
	uint64_t interactionSentAt = 0;
	std::string interactionNonce;
	std::string interactionMessageId;
	std::string interactionCustomId;
	int interactionControl = -1;
	std::string interactionToast;
	std::vector<std::string> revertChosen;
	std::string revertChosenLabel;
	bool wasAtBottom = false;
	std::string lastMarkedMessageId;
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
	float drawMessage(const Discord::Message &msg, float y, float maxWidth, bool isSelected, bool showHeader,
	                  bool prevGroupedHighlight = false, bool nextGroupedHighlight = false,
	                  const MessageRenderCache *renderCache = nullptr);
	float drawForumMessage(const Discord::Message &msg, float y, bool isSelected);
	float drawSystemMessage(const Discord::Message &msg, float y, float topMargin, float height, bool isSelected);
	float drawReplyPreview(const Discord::Message &msg, float x, float y);
	float drawCommandLine(const Discord::Message &msg, float x, float y);
	float drawForwardHeader(const Discord::Message &msg, float x, float y);
	float drawAuthorHeader(const Discord::Message &msg, float x, float y, bool showHeader,
	                       const MessageRenderCache *renderCache = nullptr);
	float drawMessageContent(const Discord::Message &msg, float x, float y,
	                         const MessageRenderCache *renderCache = nullptr);
	float drawAttachments(const Discord::Message &msg, float x, float y, float maxWidth);
	float drawAttachment(const Discord::Attachment &attach, float x, float y, float maxWidth, bool revealed);
	float drawStickers(const Discord::Message &msg, float x, float y, float maxWidth);
	float drawReactions(const Discord::Message &msg, float x, float y, bool isSelected);
	float drawPoll(const Discord::Message &msg, float x, float y, float maxWidth, bool isSelected,
	               const MessageRenderCache *renderCache = nullptr);
	float layoutBlocks(const std::vector<Discord::MessageBlock> &blocks, float x, float y, float width,
	                   BlockPass &pass);
	void drawControl(const Discord::MessageControl &control, float x, float y, float width, const BlockPass &pass);
	static size_t messageFingerprint(const Discord::Message &msg);
	void buildMessageCache(const Discord::Message &msg, MessageRenderCache &cache);
	float calculatePollHeight(const Discord::Poll &poll, float maxWidth);
	void submitPollVote(int answerIndex);
	std::vector<Discord::MessageControl *> selectedControls();
	bool canUseControl(const Discord::MessageControl &control) const;
	bool enterComponentMode();
	void moveComponentCursor(int step);
	void activateControl();
	void sendInteraction(Discord::MessageControl &control, const std::vector<std::string> &values,
	                     const std::string &label);
	void onInteractionResult(const std::string &nonce, Discord::InteractionResult result);
	void onModalCreate(const Discord::Modal &incoming);
	bool canSelectModalRow(int row) const;
	bool canSubmitModal() const;
	void updateModal(u32 kDown, u32 kHeld, std::unique_lock<std::recursive_mutex> &updateLock);
	void submitModal();
	void renderModal();
	std::vector<Discord::SelectOption> selectTargets(const Discord::MessageControl &control);
	void openSelectPicker(const Discord::MessageControl &control);
	void updateSelectPicker(u32 kDown, u32 kHeld);
	void submitSelectPicker();
	void renderSelectPicker();
	void renderLinkPopup();
	float calculateMessageHeight(const Discord::Message &msg, bool showHeader);
	float calculateEmbedHeight(const Discord::Embed &embed, float maxWidth);
	float renderEmbed(const Discord::Embed &embed, float x, float y, float maxWidth,
	                  const EmbedRenderCache *embedCache = nullptr);
	void openKeyboard();
	void showMessageOptions();

	void scrollToBottom();
	void rebuildLayoutCache();
	void ensureSelectionVisible();
	void catchUpMessages();
	bool isAtBottom() const;
	bool followsNewMessages() const;
	std::string getLatestRealMessageId() const;
	void replaceHistory(const std::vector<Discord::Message> &newestFirst);
	void removeSelectedMessage();
	bool selectedIsEphemeral() const;
	void checkAndMarkChannelRead();
	void syncScrollAfterRebuild(bool wasAtBottom, bool updateSelection = false);

	Discord::Message createOptimisticMessage(const std::string &content, int type = 0,
	                                         const std::string &referencedAuthor = "");
	void handleMessageSendResult(const std::string &pendingId, const Discord::Message &sentMsg, bool success,
	                             int errorCode);
	void onMessageCreate(const Discord::Message &msg);
	void onMessageUpdate(const Discord::Message &msg);
	void onMessageDelete(const std::string &msgId);
	void onReaction(const std::string &messageId, const Discord::Emoji &emoji, const std::string &userId, bool added);

	struct KeyboardResult {
		int button;
		std::string text;
	};
	KeyboardResult runKeyboard(const std::string &hint, const std::string &initialText = "", bool multiline = true,
	                           int maxLength = -1, const std::string &confirmLabel = "");

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
