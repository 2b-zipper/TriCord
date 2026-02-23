#ifndef DISCORD_TYPES_H
#define DISCORD_TYPES_H

#include <cstdint>
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
  UserStatus status = UserStatus::UNKNOWN;
};

struct Overwrite {
  std::string id;
  int type;
  uint64_t allow;
  uint64_t deny;
};

struct Channel {
  std::string id;
  std::string name;
  std::string parent_id;
  int type;
  int flags;
  int position;
  bool viewable;
  std::string topic;
  int message_count;
  std::string last_message_id;
  std::string owner_id;
  std::string owner_name;
  int owner_color;
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
  int color;
  int position;
  uint64_t permissions;
};

struct Member {
  std::string user_id;
  std::string nickname;
  std::vector<std::string> role_ids;
};

struct GuildFolder {
  std::string id;
  std::string name;
  int color;
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
};

struct EmbedField {
  std::string name;
  std::string value;
  bool isInline;
};

struct Embed {
  std::string title;
  std::string description;
  std::string url;
  int color;
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
  std::string provider_name;
  std::string type;
  std::string timestamp;
  std::vector<EmbedField> fields;
};

struct Attachment {
  std::string id;
  std::string filename;
  std::string url;
  std::string proxy_url;
  int size;
  int width;
  int height;
  std::string content_type;
};

struct Sticker {
  std::string id;
  std::string name;
  int format_type;
};

struct Emoji {
  std::string id;
  std::string name;
  bool animated;
};

struct Reaction {
  Emoji emoji;
  int count;
  bool me;
};

struct Message {
  std::string id;
  std::string content;
  std::string timestamp;
  std::string channelId;
  User author;
  Member member;
  std::vector<Embed> embeds;
  std::vector<Attachment> attachments;
  std::vector<Sticker> stickers;
  std::vector<Reaction> reactions;

  std::string referencedMessageId;
  std::string referencedAuthorName;
  std::string referencedAuthorNickname;
  int referencedAuthorColor = 0;
  std::string referencedContent;
  int type;
  std::string edited_timestamp;

  bool isForwarded = false;
  std::string originalAuthorName;
  std::string originalAuthorAvatar;
};

} // namespace Discord

#endif // DISCORD_TYPES_H
