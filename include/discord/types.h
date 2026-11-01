#ifndef DISCORD_TYPES_H
#define DISCORD_TYPES_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace Discord {

enum class UserStatus { ONLINE, IDLE, DND, INVISIBLE, OFFLINE, UNKNOWN };

struct User {
	std::string id;
	std::string username;
	std::string discriminator;
	std::string global_name;
	std::string avatar;
	bool bot = false;
	UserStatus status = UserStatus::UNKNOWN;
};

struct Overwrite {
	std::string id;
	int type = 0;
	uint64_t allow = 0;
	uint64_t deny = 0;
};

struct UserProfile {
	std::string userId;
	std::string bio;
	std::string pronouns;
	bool loaded = false;
};

struct Channel {
	std::string id;
	std::string name;
	std::string parent_id;
	int type = 0;
	int flags = 0;
	int position = 0;
	bool viewable = false;
	std::string topic;
	int message_count = 0;
	std::string last_message_id;
	std::string owner_id;
	std::string owner_name;
	int owner_color = 0;
	std::string last_message_content;
	std::string op_content;
	bool is_archived = false;

	std::vector<struct Overwrite> permission_overwrites;
	std::vector<User> recipients;
	std::string icon;
};

namespace Permissions {
const uint64_t ADMINISTRATOR = 1ULL << 3;
const uint64_t MANAGE_MESSAGES = 1ULL << 13;
const uint64_t VIEW_CHANNEL = 1ULL << 10;
const uint64_t SEND_MESSAGES = 1ULL << 11;
} // namespace Permissions

struct Role {
	std::string id;
	std::string name;
	int color = 0;
	int position = 0;
	uint64_t permissions = 0;
};

struct Member {
	std::string user_id;
	std::string nickname;
	std::string username;
	std::string globalName;
	std::string avatar;
	std::vector<std::string> role_ids;
};

struct VoiceParticipant {
	std::string userId;
	std::string guildId;
	std::string name;
	std::string avatar;
	bool selfMute = false;
	bool selfDeaf = false;
	bool mute = false;
	bool deaf = false;
};

struct GuildFolder {
	std::string id;
	std::string name;
	int color = 0;
	std::vector<std::string> guildIds;
};

struct Guild {
	std::string id;
	std::string name;
	std::string icon;
	std::string ownerId;
	std::string rules_channel_id;
	std::string description;
	int approximateMemberCount = 0;
	int approximatePresenceCount = 0;
	std::vector<Channel> channels;
	std::vector<std::string> myRoles;
	std::vector<Role> roles;
	std::vector<Member> members;
	bool channelsLoaded = true;
	bool loadedOnDemand = false;
};

struct ReadState {
	std::string channelId;
	std::string lastReadMessageId;
	int mentionCount = 0;
	std::string ackToken;
};

struct ChannelNotificationOverride {
	std::string channelId;
	bool muted = false;
	std::string muteEndTime;
	int messageNotifications = 3; // 3=inherit, 0=all, 1=mentions, 2=none
	int flags = 0;
	// flags: 1<<9=ONLY_MENTIONS, 1<<10=ALL_MESSAGES (precedes notification settings)
};

struct GuildNotificationSettings {
	std::string guildId;
	bool muted = false;
	std::string muteEndTime;
	int messageNotifications = 3;
	int flags = 0;
	bool suppressEveryone = false;
	bool suppressRoles = false;
	// flags: 1<<11=ALL_MESSAGES, 1<<12=ONLY_MENTIONS
	std::map<std::string, ChannelNotificationOverride> channelOverrides;
};

struct EmbedField {
	std::string name;
	std::string value;
	bool isInline = false;
};

struct Embed {
	std::string title;
	std::string description;
	std::string url;
	int color = 0;
	std::string author_name;
	std::string author_icon_url;
	std::string footer_text;
	std::string footer_icon_url;
	std::string image_url;
	std::string image_proxy_url;
	int image_width = 0;
	int image_height = 0;
	std::string thumbnail_url;
	std::string thumbnail_proxy_url;
	int thumbnail_width = 0;
	int thumbnail_height = 0;
	std::string gif_url;
	std::string provider_name;
	std::string type;
	std::string timestamp;
	std::vector<EmbedField> fields;

	bool isBareGif() const {
		return type == "gifv" && !(gif_url.empty() && thumbnail_url.empty() && image_url.empty());
	}
};

struct Attachment {
	std::string id;
	std::string filename;
	std::string url;
	std::string proxy_url;
	int size = 0;
	int width = 0;
	int height = 0;
	std::string content_type;
	bool spoiler = false;
};

struct Sticker {
	std::string id;
	std::string name;
	int format_type = 0;
};

struct Emoji {
	std::string id;
	std::string name;
	mutable std::string hex;
	bool animated = false;

	bool matches(const Emoji &other) const { return id == other.id && name == other.name; }
};

struct Reaction {
	Emoji emoji;
	int count = 0;
	bool me = false;
};

struct PollAnswer {
	int id = 0;
	std::string text;
	Emoji emoji;
	int count = 0;
	bool meVoted = false;
};

struct Poll {
	std::string question;
	std::vector<PollAnswer> answers;
	std::string expiry;
	bool allowMultiselect = false;
	bool finalized = false;
};

struct PollResult {
	std::string question;
	std::string winnerText;
	Emoji winnerEmoji;
	int totalVotes = 0;
	int winnerVotes = 0;
	bool hasWinner = false;
};

enum class InteractionResult { SUCCESS, FAILURE, MODAL };

namespace ComponentType {
constexpr int ACTION_ROW = 1;
constexpr int BUTTON = 2;
constexpr int STRING_SELECT = 3;
constexpr int TEXT_INPUT = 4;
constexpr int USER_SELECT = 5;
constexpr int ROLE_SELECT = 6;
constexpr int MENTIONABLE_SELECT = 7;
constexpr int CHANNEL_SELECT = 8;
constexpr int SECTION = 9;
constexpr int TEXT_DISPLAY = 10;
constexpr int THUMBNAIL = 11;
constexpr int MEDIA_GALLERY = 12;
constexpr int FILE_DISPLAY = 13;
constexpr int SEPARATOR = 14;
constexpr int CONTAINER = 17;
constexpr int LABEL = 18;
constexpr int FILE_UPLOAD = 19;
constexpr int RADIO_GROUP = 21;
constexpr int CHECKBOX_GROUP = 22;
constexpr int CHECKBOX = 23;
} // namespace ComponentType

namespace ButtonStyle {
constexpr int PRIMARY = 1;
constexpr int SUCCESS = 3;
constexpr int DANGER = 4;
constexpr int LINK = 5;
constexpr int PREMIUM = 6;
} // namespace ButtonStyle

struct SelectOption {
	std::string label;
	std::string value;
	std::string description;
	Emoji emoji;
};

struct MessageControl {
	int type = 0;
	int style = 0;
	std::string label;
	std::string customId;
	std::string url;
	Emoji emoji;
	bool disabled = false;
	bool hidden = false;

	int minValues = 1;
	int maxValues = 1;
	std::vector<SelectOption> options;
	std::vector<int> channelTypes;
	std::vector<std::string> chosen;
	std::string chosenLabel;

	bool isButton() const { return type == ComponentType::BUTTON; }
	// Premium buttons have neither, so there is nothing to do with them here.
	bool canActivate() const { return !disabled && (!customId.empty() || !url.empty()); }
};

struct ModalField {
	int wrapperType = 0;
	std::string label;
	std::string description;
	std::string placeholder;
	std::string value;
	int minLength = 0;
	int maxLength = 4000;
	bool required = true;
	bool checked = false;
	MessageControl control;
};

struct Modal {
	std::string id;
	std::string applicationId;
	std::string channelId;
	std::string customId;
	std::string title;
	std::string nonce;
	std::vector<ModalField> fields;
};

struct MessageBlock {
	enum class Kind { TEXT, MEDIA, SEPARATOR, CONTROLS, CONTAINER };

	Kind kind = Kind::TEXT;
	bool spoiler = false;
	std::string text;
	// Indices into Message::attachments, which keeps retries and spoiler checks in one place.
	std::vector<int> attachments;
	// A section is a TEXT block with its accessory: this thumbnail, or a single button in controls.
	int thumbnail = -1;
	std::vector<MessageControl> controls;
	bool divider = true;
	bool largeSpacing = false;
	int accentColor = -1;
	std::vector<MessageBlock> children;
};

struct Message {
	std::string id;
	std::string content;
	std::string displayContent;
	std::string timestamp;
	std::string channelId;
	User author;
	Member member;
	std::vector<Embed> embeds;
	std::vector<Attachment> attachments;
	std::vector<Sticker> stickers;
	std::vector<Reaction> reactions;
	std::vector<User> mentions;
	bool mentionEveryone = false;
	std::vector<std::string> mentionRoles;

	std::string referencedMessageId;
	std::string referencedAuthorName;
	std::string referencedAuthorNickname;
	int referencedAuthorColor = 0;
	std::string referencedContent;
	int type = 0;
	std::string edited_timestamp;

	int flags = 0;
	std::string applicationId;
	std::string interactionName;
	std::string interactionUser;
	std::vector<MessageBlock> blocks;

	// The types Discord renders as their content rather than as a system line.
	bool showsContent() const { return type == 0 || type == 19 || type == 20 || type == 23 || type == 26; }
	bool isComponentsV2() const { return flags & (1 << 15); }
	bool isEphemeral() const { return flags & (1 << 6); }
	bool isThinking() const { return flags & (1 << 7); }
	bool showsReplyPreview() const { return type == 19 && !referencedAuthorName.empty(); }
	bool showsCommandLine() const { return !interactionName.empty() && !showsReplyPreview(); }

	bool isForwarded = false;
	std::string originalAuthorName;
	std::string originalAuthorAvatar;
	std::string nonce;

	bool hasPoll = false;
	Poll poll;

	bool hasPollResult = false;
	PollResult pollResult;
};

} // namespace Discord

#endif // DISCORD_TYPES_H
