#include "ui/message_screen.h"
#include "core/config.h"
#include "core/i18n.h"
#include "discord/avatar_cache.h"
#include "discord/discord_client.h"
#include "discord/voice_client.h"
#include "log.h"
#include "ui/emoji_manager.h"
#include "ui/image_manager.h"
#include "ui/voice_controls.h"
#include "ui/markdown_renderer.h"
#include "ui/screen_manager.h"
#include "utils/message_utils.h"
#include "utils/utf8_utils.h"
#include <3ds.h>
#include <algorithm>
#include <citro2d.h>
#include <ctime>

#include <mutex>
#include <set>

namespace UI {

namespace {
constexpr float VOICE_BTN_X = 320.0f - VoiceControls::WIDTH - 10.0f;
constexpr float VOICE_BTN_Y = 240.0f - VoiceControls::BUTTON_SIZE - 10.0f;

float bottomButtonY() {
	return VoiceControls::visible() ? VOICE_BTN_Y - VoiceControls::BUTTON_SIZE - 8.0f : VOICE_BTN_Y;
}
} // namespace

MessageScreen::MessageScreen(const std::string &channelId, const std::string &channelName)
    : channelId(channelId), channelName(channelName), channelType(0), rulesChannelId(""), selectedIndex(0),
      isLoading(true), isFetchingHistory(false), requestHistoryFetch(false), scrollInitialized(false),
      showNewMessageIndicator(false), newMessageCount(0), isForumView(false), hasMoreHistory(true), targetScrollY(0.0f),
      currentScrollY(0.0f), totalContentHeight(0.0f), isMenuOpen(false), menuIndex(0),
      bottomMode(BottomScreenMode::TOPIC) {
	emojiPicker = std::make_unique<EmojiPicker>();
	emojiPicker->setOnEmojiSelected([this](const std::string &emoji) {
		if (selectedIndex < (int)messages.size()) {
			const auto &msg = messages[selectedIndex];
			bool alreadyReacted = false;
			for (const auto &r : msg.reactions) {
				if (r.me && r.emoji.name == emoji) {
					alreadyReacted = true;
					break;
				}
			}
			if (alreadyReacted) {
				Discord::DiscordClient::getInstance().removeReaction(this->channelId, msg.id, emoji);
			} else {
				Discord::DiscordClient::getInstance().addReaction(this->channelId, msg.id, emoji);
			}
		}
	});
	emojiPicker->setOnClosed([this]() { bottomMode = BottomScreenMode::TOPIC; });

	aliveToken = std::make_shared<bool>(true);
	Logger::log("MessageScreen initialized for channel: %s", channelName.c_str());
}

MessageScreen::~MessageScreen() {
	*aliveToken = false;
	Discord::DiscordClient::getInstance().setMessageCallback(nullptr);
	Discord::DiscordClient::getInstance().setMessageUpdateCallback(nullptr);
	Discord::DiscordClient::getInstance().setMessageDeleteCallback(nullptr);
	Discord::DiscordClient::getInstance().setConnectionCallback(nullptr);
	// Guarded so a screen that has already opened another channel keeps its id.
	if (Discord::DiscordClient::getInstance().getSelectedChannelId() == channelId) {
		Discord::DiscordClient::getInstance().setSelectedChannelId("");
	}

	embedHeightCache.clear();
	revealedSpoilers.clear();
	ImageManager::getInstance().clearRemote();
}

void MessageScreen::onEnter() {
	isLoading = true;
	newMessageCount = 0;

	Discord::DiscordClient &client = Discord::DiscordClient::getInstance();
	std::lock_guard<std::recursive_mutex> lock(client.getMutex());
	Discord::Channel channel = client.getChannel(channelId);
	this->channelType = channel.type;
	this->channelTopic = channel.topic;
	this->guildId = client.getGuildIdFromChannel(channelId);
	this->rulesChannelId = client.getGuild(guildId).rules_channel_id;

	if (!this->guildId.empty() && this->guildId != "DM") {
		const auto &guilds = client.getGuilds();
		for (const auto &g : guilds) {
			if (g.id == this->guildId) {
				this->rulesChannelId = g.rules_channel_id;
				break;
			}
		}
	}

	this->channelName = MessageUtils::getChannelDisplayName(channel);

	this->truncatedChannelName = getTruncatedRichText(this->channelName, 310.0f - 56.0f, 0.55f, 0.55f);

	client.setSelectedChannelId(channelId);

	if (!this->guildId.empty()) {
		client.sendLazyRequest(this->guildId, channelId);
	}

	if (channel.type == 1 && !channel.recipients.empty()) {
		const auto &r = channel.recipients[0];
		Discord::AvatarCache::getInstance().prefetchAvatar(r.id, r.avatar, r.discriminator);
	} else if (channel.type == 3 && !channel.icon.empty()) {
		Discord::AvatarCache::getInstance().prefetchChannelIcon(channel.id, channel.icon);
	}

	client.setMessageCallback([this](const Discord::Message &msg) {
		if (msg.channelId != channelId) {
			return;
		}
		std::lock_guard<std::recursive_mutex> lock(messageMutex);
		bool found = false;
		for (auto &m : this->messages) {
			if (m.id == msg.id) {
				m = msg;
				found = true;
				break;
			}
			if (m.id.substr(0, 8) == "pending_" && !msg.nonce.empty() && m.nonce == msg.nonce) {
				m = msg;
				found = true;
				break;
			}
		}
		if (!found) {
			this->messages.push_back(msg);
			bool atBottom = isAtBottom();
			rebuildLayoutCache();
			syncScrollAfterRebuild(atBottom, true);
			if (!atBottom) {
				showNewMessageIndicator = true;
				newMessageCount++;
			}
		} else {
			rebuildLayoutCache();
		}
	});

	client.setMessageUpdateCallback([this](const Discord::Message &msg) {
		if (msg.channelId != channelId) {
			return;
		}
		std::lock_guard<std::recursive_mutex> lock(messageMutex);
		for (auto &m : this->messages) {
			if (m.id == msg.id) {
				bool atBottom = isAtBottom();
				m = msg;
				rebuildLayoutCache();
				syncScrollAfterRebuild(atBottom);
				break;
			}
		}
	});

	client.setMessageDeleteCallback([this](const std::string &msgId) {
		std::lock_guard<std::recursive_mutex> lock(messageMutex);
		for (auto it = this->messages.begin(); it != this->messages.end(); ++it) {
			if (it->id == msgId) {
				this->messages.erase(it);
				rebuildLayoutCache();
				break;
			}
		}
	});

	client.setMessageReactionAddCallback([this](const std::string &channelId, const std::string &messageId,
	                                            const std::string &userId, const Discord::Emoji &emoji) {
		if (channelId != this->channelId) {
			return;
		}
		std::lock_guard<std::recursive_mutex> lock(messageMutex);
		for (auto &msg : this->messages) {
			if (msg.id == messageId) {
				bool atBottom = isAtBottom();
				bool found = false;
				bool isMe = (userId == Discord::DiscordClient::getInstance().getCurrentUser().id);
				for (auto &r : msg.reactions) {
					if (r.emoji.id == emoji.id && r.emoji.name == emoji.name) {
						r.count++;
						if (isMe) {
							r.me = true;
						}
						found = true;
						break;
					}
				}
				if (!found) {
					Discord::Reaction newR;
					newR.emoji = emoji;
					newR.count = 1;
					newR.me = isMe;
					msg.reactions.push_back(newR);
				}
				rebuildLayoutCache();
				syncScrollAfterRebuild(atBottom);
				break;
			}
		}
	});

	client.setMessageReactionRemoveCallback([this](const std::string &channelId, const std::string &messageId,
	                                               const std::string &userId, const Discord::Emoji &emoji) {
		if (channelId != this->channelId) {
			return;
		}
		std::lock_guard<std::recursive_mutex> lock(messageMutex);
		for (auto &msg : this->messages) {
			if (msg.id == messageId) {
				bool atBottom = isAtBottom();
				bool isMe = (userId == Discord::DiscordClient::getInstance().getCurrentUser().id);
				for (auto it = msg.reactions.begin(); it != msg.reactions.end(); ++it) {
					if (it->emoji.id == emoji.id && it->emoji.name == emoji.name) {
						it->count--;
						if (isMe) {
							it->me = false;
						}
						if (it->count <= 0) {
							msg.reactions.erase(it);
						}
						rebuildLayoutCache();
						syncScrollAfterRebuild(atBottom);
						break;
					}
				}
				break;
			}
		}
	});

	client.setConnectionCallback([this]() {
		Logger::log("[UI] Gateway reconnected, catching up messages...");
		catchUpMessages();
	});

	this->truncatedChannelName = getTruncatedRichText(this->channelName, 310.0f - 56.0f, 0.55f, 0.55f);
	this->messages.clear();
	targetScrollY = 0.0f;
	currentScrollY = 0.0f;
	totalContentHeight = 0.0f;
	rebuildLayoutCache();
	isForumView = (channel.type == 15);
	// DMs carry no guild permissions, so the viewable flag never applies to them.
	bool isPrivate = (channel.type == 1 || channel.type == 3);
	isHiddenChannel = !isPrivate && !channel.viewable &&
	                  (guildId.empty() || client.getGuild(guildId).ownerId != client.getCurrentUser().id);

	if (isHiddenChannel) {
		isLoading = false;
	} else if (isForumView) {
		client.fetchForumThreads(channelId, [this, token = aliveToken](const std::vector<Discord::Channel> &threads) {
			if (!*token) {
				return;
			}
			std::vector<Discord::Message> threadMsgs;
			for (const auto &t : threads) {
				Discord::Message m;
				m.id = t.id;
				m.content = t.name;
				m.author.username = TR("message.thread");
				m.type = t.type;
				m.timestamp = "";
				threadMsgs.push_back(m);
			}
			{
				std::lock_guard<std::recursive_mutex> lock(messageMutex);
				this->messages = threadMsgs;
				rebuildLayoutCache();
				if (!this->messages.empty()) {
					selectedIndex = 0;
				}
			}
			isLoading = false;
		});
	} else {
		client.fetchMessagesAsync(
		    channelId, 50, [this, token = aliveToken](const std::vector<Discord::Message> &fetched) {
			    if (!*token) {
				    return;
			    }
			    {
				    std::lock_guard<std::recursive_mutex> lock(messageMutex);
				    this->messages = fetched;
				    std::reverse(this->messages.begin(), this->messages.end());
				    rebuildLayoutCache();
				    scrollToBottom();
			    }
			    isLoading = false;
			    if (!fetched.empty()) {
				    Discord::DiscordClient::getInstance().markChannelRead(this->channelId, fetched.front().id);
			    }
		    });
	}

	if (!this->guildId.empty()) {
		client.sendLazyRequest(this->guildId, channelId);
	}
}

bool MessageScreen::hidesMenu() const { return bottomMode == BottomScreenMode::EMOJI_PICKER; }

void MessageScreen::flushMemberFetches() {
	if (queuedMemberFetches.empty()) {
		return;
	}

	if (guildId.empty() || guildId == "DM") {
		queuedMemberFetches.clear();
		return;
	}

	Discord::DiscordClient::getInstance().requestMembers(guildId, queuedMemberFetches);

	// The reply is a gateway event, not a per-request callback, so anything the
	// chunk leaves out is retried after this cooldown rather than never.
	uint64_t retryAt = osGetTime() + (30 * 1000);
	for (const auto &uid : queuedMemberFetches) {
		failedMemberFetches[uid] = retryAt;
		pendingMemberFetches.erase(uid);
	}
	queuedMemberFetches.clear();
}

void MessageScreen::update() {
	flushMemberFetches();

	Discord::DiscordClient &client = Discord::DiscordClient::getInstance();
	std::lock_guard<std::recursive_mutex> clientLock(client.getMutex());
	std::unique_lock<std::recursive_mutex> updateLock(messageMutex);

	uint32_t currentGen = ImageManager::getInstance().getGeneration();
	if (currentGen != lastImageGeneration) {
		lastImageGeneration = currentGen;
		rebuildLayoutCache();
	}

	u32 kDown = hidKeysDown();
	u32 kHeld = hidKeysHeld();
	u32 kUp = hidKeysUp();

	if ((kDown & KEY_TOUCH) && bottomMode != BottomScreenMode::EMOJI_PICKER) {
		touchPosition touch;
		hidTouchRead(&touch);

		if (VoiceControls::handleTouch(touch, VOICE_BTN_X, VOICE_BTN_Y)) {
			return;
		}

		float btnW = 30.0f;
		float btnH = 30.0f;
		float btnX = 320.0f - btnW - 10.0f;
		float btnY = bottomButtonY();

		const float SCREEN_HEIGHT = 240.0f;
		float maxScroll = std::max(0.0f, totalContentHeight - SCREEN_HEIGHT);
		bool isScrollBtnVisible = (targetScrollY < maxScroll - 10.0f);

		if (isScrollBtnVisible && touch.px >= btnX && touch.px <= btnX + btnW && touch.py >= btnY &&
		    touch.py <= btnY + btnH) {
			if (!isMenuOpen && !isLoading) {
				scrollToBottom();
			}
		}

		float reactBtnX = isScrollBtnVisible ? (btnX - btnW - 8.0f) : btnX;
		if (touch.px >= reactBtnX && touch.px <= reactBtnX + btnW && touch.py >= btnY && touch.py <= btnY + btnH) {
			if (!isMenuOpen && !isLoading) {
				bottomMode = BottomScreenMode::EMOJI_PICKER;
			}
		}

		float callBtnX = reactBtnX - btnW - 8.0f;
		if (isCallableChannel() && !isCallActive() && touch.px >= callBtnX && touch.px <= callBtnX + btnW &&
		    touch.py >= btnY && touch.py <= btnY + btnH) {
			if (!isMenuOpen && !isLoading) {
				startCall();
			}
		}
	}

	if ((kDown & KEY_B) && !isMenuOpen) {
		if (bottomMode == BottomScreenMode::EMOJI_PICKER) {
			bottomMode = BottomScreenMode::TOPIC;
			return;
		}

		Discord::DiscordClient::getInstance().setMessageCallback(nullptr);
		Discord::DiscordClient::getInstance().setMessageDeleteCallback(nullptr);
		Discord::DiscordClient::getInstance().setMessageReactionAddCallback(nullptr);
		Discord::DiscordClient::getInstance().setMessageReactionRemoveCallback(nullptr);

		{
			std::lock_guard<std::recursive_mutex> lock(client.getMutex());
			Discord::Channel channel = client.getChannel(channelId);
			if (!channel.parent_id.empty()) {
				Discord::Channel parent = client.getChannel(channel.parent_id);
				if (parent.type == 15) {
					client.setSelectedChannelId(channel.parent_id);
				}
			}
		}

		ScreenManager::getInstance().returnToPreviousScreen();
		return;
	}

	if (isLoading) {
		return;
	}

	if (isMenuOpen) {
		if (kDown & KEY_DOWN) {
			if (menuIndex < (int)menuOptions.size() - 1) {
				menuIndex++;
			}
		}
		if (kDown & KEY_UP) {
			if (menuIndex > 0) {
				menuIndex--;
			}
		}

		if (kDown & KEY_B) {
			isMenuOpen = false;
		}

		if (kDown & KEY_A) {
			if (menuIndex < 0 || menuIndex >= (int)menuActions.size()) {
				isMenuOpen = false;
				return;
			}

			std::string action = menuActions[menuIndex];
			isMenuOpen = false;

			if (action.find("RemoveReaction_") == 0) {
				if (selectedIndex >= 0 && selectedIndex < (int)messages.size()) {
					std::string emoji = action.substr(15);
					Discord::DiscordClient::getInstance().removeReaction(channelId, messages[selectedIndex].id, emoji);
				}
			} else if (action == "ToggleSpoiler") {
				if (selectedIndex >= 0 && selectedIndex < (int)messages.size()) {
					const std::string &id = messages[selectedIndex].id;
					if (!revealedSpoilers.erase(id)) {
						revealedSpoilers.insert(id);
					}
				}
			} else if (action == "Reply") {
				if (selectedIndex >= 0 && selectedIndex < (int)messages.size()) {
					std::string targetMsgId = messages[selectedIndex].id;
					std::string targetAuthorName = messages[selectedIndex].author.global_name.empty()
					                                   ? messages[selectedIndex].author.username
					                                   : messages[selectedIndex].author.global_name;

					updateLock.unlock();
					auto res = runKeyboard(TR("common.reply_hint"));
					updateLock.lock();

					if (res.button == SWKBD_BUTTON_RIGHT && !res.text.empty()) {
						Discord::Message replyMsg;
						replyMsg.id = "pending_" + std::to_string(osGetTime());
						replyMsg.nonce = replyMsg.id;
						replyMsg.content = res.text;
						replyMsg.channelId = channelId;
						replyMsg.author = client.getCurrentUser();
						replyMsg.timestamp = TR("message.status.sending");
						replyMsg.type = 19;
						replyMsg.referencedAuthorName = targetAuthorName;

						this->messages.push_back(replyMsg);
						rebuildLayoutCache();
						scrollToBottom();

						client.sendReply(
						    channelId, res.text, targetMsgId,
						    [this, replyMsgId = replyMsg.id](const Discord::Message &sentMsg, bool success,
						                                     int errorCode) {
							    std::lock_guard<std::recursive_mutex> lock(messageMutex);
							    for (auto &m : this->messages) {
								    if (m.id == replyMsgId) {
									    if (success) {
										    m = sentMsg;
										    Logger::log("Updated pending reply with confirmed ID: %s",
										                sentMsg.id.c_str());
									    } else {
										    m.timestamp = TR("message.status.failed");
										    Logger::log("Reply failed with code: %d", errorCode);
									    }
									    break;
								    }
							    }
							    rebuildLayoutCache();
							    scrollToBottom();
						    },
						    replyMsg.nonce);
					}
				}
			} else if (action == "Edit") {
				if (selectedIndex >= 0 && selectedIndex < (int)messages.size()) {
					std::string editId = messages[selectedIndex].id;
					std::string oldContent = messages[selectedIndex].content;

					updateLock.unlock();
					auto res = runKeyboard(TR("common.message_hint"), oldContent);
					updateLock.lock();

					if (res.button == SWKBD_BUTTON_RIGHT && !res.text.empty() && res.text != oldContent) {
						Discord::DiscordClient::getInstance().editMessage(channelId, editId, res.text);
						for (auto &msg : messages) {
							if (msg.id == editId) {
								msg.content = res.text;
								break;
							}
						}
					}
				}
			} else if (action == "Delete") {
				if (selectedIndex >= 0 && selectedIndex < (int)messages.size()) {
					std::string mid = this->messages[selectedIndex].id;
					if (Discord::DiscordClient::getInstance().deleteMessage(channelId, mid)) {
						this->messages.erase(this->messages.begin() + selectedIndex);
						if (selectedIndex >= (int)this->messages.size()) {
							selectedIndex = std::max(0, (int)this->messages.size() - 1);
						}
						rebuildLayoutCache();
					}
				}
			} else if (action == "Retry") {
				if (selectedIndex >= 0 && selectedIndex < (int)messages.size()) {
					const auto &msg = messages[selectedIndex];
					for (const auto &attach : msg.attachments) {
						std::string url = attach.proxy_url.empty() ? attach.url : attach.proxy_url;
						ImageManager::getInstance().clearFailed(url);
						ImageManager::getInstance().prefetch(url, attach.width, attach.height);
					}
					for (const auto &sticker : msg.stickers) {
						std::string ext = (sticker.format_type == 4) ? ".gif" : ".png";
						std::string url = "https://cdn.discordapp.com/stickers/" + sticker.id + ext;
						ImageManager::getInstance().clearFailed(url);
						ImageManager::getInstance().prefetch(url);
					}
					for (const auto &embed : msg.embeds) {
						if (!embed.image_url.empty()) {
							ImageManager::getInstance().clearFailed(embed.image_url);
							if (!embed.image_proxy_url.empty()) {
								ImageManager::getInstance().clearFailed(embed.image_proxy_url);
							}
							std::string mainUrl =
							    embed.image_proxy_url.empty() ? embed.image_url : embed.image_proxy_url;
							ImageManager::getInstance().prefetch(mainUrl, embed.image_width, embed.image_height);
						}
						if (!embed.thumbnail_url.empty()) {
							ImageManager::getInstance().clearFailed(embed.thumbnail_url);
							if (!embed.thumbnail_proxy_url.empty()) {
								ImageManager::getInstance().clearFailed(embed.thumbnail_proxy_url);
							}
							std::string thumbUrl =
							    embed.thumbnail_proxy_url.empty() ? embed.thumbnail_url : embed.thumbnail_proxy_url;
							ImageManager::getInstance().prefetch(thumbUrl, embed.thumbnail_width,
							                                     embed.thumbnail_height);
						}
					}
				}
			}
		}
		return;
	}

	bool shouldMoveDown = false;
	bool shouldMoveUp = false;
	bool isManualScrolling = false;

	circlePosition circle;
	hidCircleRead(&circle);
	bool isAnalogMoving = abs(circle.dx) > 35 || abs(circle.dy) > 35;

	if (abs(circle.dy) > 35 && bottomMode != BottomScreenMode::EMOJI_PICKER) {
		float scrollDelta = circle.dy * 0.08f;
		targetScrollY -= scrollDelta;
		const float SCREEN_HEIGHT = 240.0f;
		float maxScroll = std::max(0.0f, totalContentHeight - SCREEN_HEIGHT);
		targetScrollY = std::clamp(targetScrollY, 0.0f, maxScroll);
		isManualScrolling = true;
	} else {
		isManualScrolling = false;
	}

	float scrollSpeed = 0.5f;
	currentScrollY += (targetScrollY - currentScrollY) * scrollSpeed;

	if (bottomMode == BottomScreenMode::EMOJI_PICKER) {
		int oldCat = emojiPicker->getCurrentCategory();
		emojiPicker->update(kDown, kHeld, kUp, circle);
		if (oldCat != emojiPicker->getCurrentCategory()) {
			EmojiManager::getInstance().onCategoryChanged(getVisibleTwemojis());
		}
	} else {
		if (!isAnalogMoving) {
			if (kDown & KEY_DOWN) {
				shouldMoveDown = true;
				keyRepeatTimer = 0;
			} else if (kHeld & KEY_DOWN) {
				keyRepeatTimer++;
				if (keyRepeatTimer >= REPEAT_INITIAL_DELAY) {
					if ((keyRepeatTimer - REPEAT_INITIAL_DELAY) % REPEAT_INTERVAL == 0) {
						shouldMoveDown = true;
					}
				}
			}

			if (kDown & KEY_UP) {
				shouldMoveUp = true;
				keyRepeatTimer = 0;
			} else if (kHeld & KEY_UP) {
				keyRepeatTimer++;
				if (keyRepeatTimer >= REPEAT_INITIAL_DELAY) {
					if ((keyRepeatTimer - REPEAT_INITIAL_DELAY) % REPEAT_INTERVAL == 0) {
						shouldMoveUp = true;
					}
				}
			}

			if (!(kHeld & (KEY_UP | KEY_DOWN))) {
				keyRepeatTimer = 0;
			}
		} else {
			keyRepeatTimer = 0;
		}

		if (!isManualScrolling && (shouldMoveDown || shouldMoveUp)) {
			if (shouldMoveDown) {
				bool visible = false;
				if (selectedIndex >= 0 && selectedIndex < (int)messagePositions.size()) {
					float y = messagePositions[selectedIndex];
					float h = messageHeights[selectedIndex];
					visible = (y + h > currentScrollY && y < currentScrollY + 240.0f);
				}

				if (!visible && !messagePositions.empty()) {
					auto it = std::lower_bound(messagePositions.begin(), messagePositions.end(), currentScrollY);
					int snapIdx = std::distance(messagePositions.begin(), it);
					if (snapIdx > 0 && (messagePositions[snapIdx - 1] + messageHeights[snapIdx - 1] > currentScrollY)) {
						snapIdx--;
					}
					if (snapIdx >= (int)this->messages.size()) {
						snapIdx = (int)this->messages.size() - 1;
					}
					selectedIndex = snapIdx;
				} else if (selectedIndex < (int)this->messages.size() - 1) {
					selectedIndex++;
					ensureSelectionVisible();
				}
			} else if (shouldMoveUp) {
				bool visible = false;
				if (selectedIndex >= 0 && selectedIndex < (int)messagePositions.size()) {
					float y = messagePositions[selectedIndex];
					float h = messageHeights[selectedIndex];
					visible = (y + h > currentScrollY && y < currentScrollY + 240.0f);
				}

				if (!visible && !messagePositions.empty()) {
					auto it =
					    std::lower_bound(messagePositions.begin(), messagePositions.end(), currentScrollY + 240.0f);
					int snapIdx = std::distance(messagePositions.begin(), it);
					if (snapIdx > 0) {
						snapIdx--;
					}
					if (snapIdx >= (int)this->messages.size()) {
						snapIdx = (int)this->messages.size() - 1;
					}
					selectedIndex = snapIdx;
				} else if (selectedIndex > 0) {
					selectedIndex--;
					ensureSelectionVisible();
				}
			}
		}

		if (kDown & KEY_A) {
			if (selectedIndex >= 0 && selectedIndex < (int)this->messages.size()) {
				const auto &msg = this->messages[selectedIndex];
				if (isForumView) {
					Discord::DiscordClient::getInstance().setSelectedChannelId(msg.id);
					ScreenManager::getInstance().setScreen(ScreenType::MESSAGES);
					return;
				} else {
					bottomMode = BottomScreenMode::EMOJI_PICKER;
					return;
				}
			}
		}

		if (kDown & KEY_Y) {
			updateLock.unlock();
			openKeyboard();
			updateLock.lock();
		}

		if ((kDown & KEY_X) && !(kHeld & KEY_SELECT) && !this->messages.empty()) {
			if (selectedIndex >= 0 && selectedIndex < (int)this->messages.size()) {
				showMessageOptions();
			}
		}

		if (kDown & KEY_B) {
			if (wasAtBottom && !isLoading && !isForumView && !messages.empty()) {
				Discord::DiscordClient::getInstance().markChannelRead(channelId, messages.back().id);
			}
			Discord::DiscordClient::getInstance().setMessageCallback(nullptr);
			Discord::DiscordClient::getInstance().setMessageUpdateCallback(nullptr);
			Discord::DiscordClient::getInstance().setMessageDeleteCallback(nullptr);
			auto &client = Discord::DiscordClient::getInstance();
			Discord::Channel ch = client.getChannel(channelId);
			if (!ch.parent_id.empty()) {
				Discord::Channel parent = client.getChannel(ch.parent_id);
				client.setSelectedChannelId(ch.parent_id);
				if (parent.type == 15) {
					ScreenManager::getInstance().setScreen(ScreenType::FORUM_CHANNEL);
				} else {
					ScreenManager::getInstance().setScreen(ScreenType::MESSAGES);
				}
				return;
			}
			ScreenManager::getInstance().setScreen(ScreenType::GUILD_LIST);
			return;
		}
	}

	if (showNewMessageIndicator) {
		const float SCREEN_HEIGHT = 240.0f;
		float maxScroll = std::max(0.0f, totalContentHeight - SCREEN_HEIGHT);
		if (currentScrollY >= maxScroll - 5.0f) {
			showNewMessageIndicator = false;
		}
	}

	if (currentScrollY < 40.0f && !isFetchingHistory && hasMoreHistory && !this->messages.empty()) {
		isFetchingHistory = true;
		fetchOlderMessages();
	}

	if (!isLoading && !isForumView && !messages.empty()) {
		const float SCREEN_HEIGHT = 240.0f;
		float maxScroll = std::max(0.0f, totalContentHeight - SCREEN_HEIGHT);
		bool atBottom = (targetScrollY >= maxScroll - 5.0f);
		if (atBottom && !wasAtBottom) {
			Discord::DiscordClient::getInstance().markChannelRead(channelId, messages.back().id);
		}
		wasAtBottom = atBottom;
	}
}

static bool editedFitsOnLastLine(const UI::MarkdownRenderer::Layout &layout, float maxWidth) {
	if (layout.lastLineType == Utils::Markdown::BlockType::CODE_BLOCK) {
		return false;
	}
	float editedWidth = UI::measureText(TR("message.edited"), 0.35f, 0.35f);
	return layout.lastLineEndX + 4.0f + editedWidth <= maxWidth;
}

float MessageScreen::calculateMessageHeight(const Discord::Message &msg, bool showHeader) {
	float topMargin = showHeader ? 4.0f : 0.0f;
	float totalH = 0.0f;
	float maxWidth = 400.0f;

	if (isForumView) {
		return 45.0f;
	}

	if (msg.type != 0 && msg.type != 19) {
		totalH = 22.0f;
	} else {
		if (msg.type == 19 && !msg.referencedAuthorName.empty()) {
			totalH += 12.0f;
		}

		if (msg.isForwarded) {
			totalH += 15.0f;
		}

		if (showHeader) {
			totalH += 14.0f;
		}

		std::string content = msg.content;
		if (!content.empty()) {
			int emojiCount = 0;
			if (MessageUtils::isEmojiOnly(content, emojiCount) && emojiCount <= 10) {
				float lineHeight = (emojiCount <= 3) ? 34.0f : 26.0f;
				totalH += lineHeight;
			} else {
				const auto &layout = UI::MarkdownRenderer::get(content, 350.0f, 0.4f);
				totalH += layout.height;

				if (!msg.edited_timestamp.empty() && !editedFitsOnLastLine(layout, 350.0f)) {
					totalH += 12.0f;
				}
			}
		}

		if (!msg.embeds.empty()) {
			float embedMaxWidth = 400.0f - 42.0f - 10.0f;
			for (const auto &embed : msg.embeds) {
				totalH += calculateEmbedHeight(embed, embedMaxWidth) + 6.0f;
			}
		}

		if (!msg.attachments.empty()) {
			for (const auto &attach : msg.attachments) {
				bool isImage = attach.content_type.find("image/") != std::string::npos ||
				               attach.filename.find(".png") != std::string::npos ||
				               attach.filename.find(".jpg") != std::string::npos ||
				               attach.filename.find(".jpeg") != std::string::npos;

				if (isImage) {
					float mediaMaxWidth = std::min(maxWidth - 42.0f - 10.0f, 330.0f);
					float maxHeight = 260.0f;
					float drawW = mediaMaxWidth;
					float drawH = 100.0f;

					std::string imageUrl = attach.proxy_url.empty() ? attach.url : attach.proxy_url;
					auto info = ImageManager::getInstance().getImageInfo(imageUrl);

					int imgW = attach.width;
					int imgH = attach.height;
					if (info.tex) {
						imgW = info.originalW;
						imgH = info.originalH;
					}

					if (imgW > 0 && imgH > 0) {
						float aspect = (float)imgW / imgH;
						drawW = std::min((float)imgW, mediaMaxWidth);
						if (imgW > 160) {
							drawW = mediaMaxWidth;
						}

						drawH = drawW / aspect;
						if (drawH > maxHeight) {
							drawH = maxHeight;
							drawW = drawH * aspect;
						}
					} else {
						drawW = std::min(mediaMaxWidth, 160.0f);
						drawH = drawW * 0.75f;
					}
					totalH += drawH + 4.0f;
				} else {
					totalH += 12.0f;
				}
			}
		}

		if (!msg.stickers.empty()) {
			for (const auto &sticker : msg.stickers) {
				if (sticker.format_type == 3) {

					totalH += 12.0f;
				} else {
					totalH += 100.0f + 4.0f;
				}
			}
		}
	}

	if (!msg.reactions.empty()) {
		float textOffsetX = 42.0f;
		float reactionX = textOffsetX;
		float rowHeight = 21.0f;
		float gap = 4.0f;
		float wrapBound = 320.0f;
		float currentReactionsH = rowHeight;

		for (const auto &react : msg.reactions) {
			std::string countStr = std::to_string(react.count);
			float countW = UI::measureText(countStr, 0.4f, 0.4f);
			float emojiW = 18.0f;
			float boxPad = 6.0f;
			float boxW = emojiW + countW + boxPad + 4.0f;

			if (reactionX + boxW > textOffsetX + wrapBound) {
				reactionX = textOffsetX;
				currentReactionsH += rowHeight + gap;
			}
			reactionX += boxW + gap;
		}
		totalH += currentReactionsH + 7.0f;
	}

	if (showHeader && (msg.type == 0 || msg.type == 19)) {
		if (totalH < 28.0f) {
			totalH = 28.0f;
		}
	}

	return topMargin + totalH + 3.0f;
}

float MessageScreen::drawForumMessage(const Discord::Message &msg, float y, bool isSelected) {
	float drawY = y + 2.0f;
	float cardH = 40.0f;

	u32 bgColor = isSelected ? ScreenManager::colorBackgroundLight() : ScreenManager::colorBackgroundDark();
	if (isSelected) {
		C2D_DrawRectSolid(0.0f, drawY, 0.5f, 400.0f, cardH, bgColor);
		C2D_DrawRectSolid(0.0f, drawY, 0.5f, 4.0f, cardH, ScreenManager::colorAccent());
		C2D_DrawRectSolid(10.0f, drawY + cardH - 1.0f, 0.5f, 380.0f, 1.0f, C2D_Color32(60, 60, 70, 255));
	}

	std::string icon = "#";
	if (msg.type == 10) {
		icon = "!";
	} else if (msg.type == 12) {
		icon = "@";
	}

	drawText(15.0f, drawY + 10.0f, 0.5f, 0.6f, 0.6f, ScreenManager::colorTextMuted(), icon);

	std::string name = getTruncatedText(msg.content, 400.0f - 40.0f - 15.0f, 0.5f, 0.5f);
	drawText(40.0f, drawY + 8.0f, 0.5f, 0.5f, 0.5f, ScreenManager::colorText(), name);

	return 45.0f;
}

float MessageScreen::drawSystemMessage(const Discord::Message &msg, float y, float topMargin, float height) {
	float blockHeight = 14.0f;
	float drawY = y + topMargin + ((height - topMargin - blockHeight) / 2.0f);

	u32 iconColor = ScreenManager::colorSuccess();
	std::string icon = "->";
	std::string text = "";
	std::string authorName = msg.author.global_name.empty() ? msg.author.username : msg.author.global_name;

	u32 nameColor = authorNameColor(msg);

	if (msg.type == 7 || msg.type == 1) {
		std::string targetName = "";
		if (msg.type == 1 && !msg.mentions.empty()) {
			targetName = msg.mentions[0].global_name.empty() ? msg.mentions[0].username : msg.mentions[0].global_name;
		}
		text = (msg.type == 7) ? TR("message.system.joined")
		                       : Core::I18n::format(TR("message.system.recipient_add"), targetName);
		iconColor = C2D_Color32(55, 151, 93, 255);
	} else if (msg.type == 2) {
		std::string targetName = "";
		if (!msg.mentions.empty()) {
			targetName = msg.mentions[0].global_name.empty() ? msg.mentions[0].username : msg.mentions[0].global_name;
		}
		text = Core::I18n::format(TR("message.system.recipient_remove"), targetName);
		iconColor = C2D_Color32(237, 66, 69, 255);
	} else if (msg.type >= 8 && msg.type <= 11) {
		text = TR("message.system.boosted");
		iconColor = C2D_Color32(253, 112, 243, 255);
	} else if (msg.type == 6) {
		iconColor = ScreenManager::colorTextMuted();
		text = TR("message.system.pinned");
	} else if (msg.type == 4) {
		iconColor = ScreenManager::colorTextMuted();
		text = Core::I18n::format(TR("message.system.name_changed"), msg.content);
	} else if (msg.type == 5) {
		iconColor = ScreenManager::colorTextMuted();
		text = TR("message.system.icon_changed");
	} else if (msg.type == 3) {
		iconColor = C2D_Color32(55, 151, 93, 255);
		text = TR("message.system.call");
	} else {
		return height;
	}

	if (true) {
		std::string iconPath;
		if (msg.type == 6) {
			iconPath = "romfs:/discord-icons/pin.png";
		} else if (msg.type == 4 || msg.type == 5) {
			iconPath = "romfs:/discord-icons/pencil.png";
		} else if (msg.type == 7 || msg.type == 1) {
			iconPath = "romfs:/discord-icons/arrow-right.png";
		} else if (msg.type == 2) {
			iconPath = "romfs:/discord-icons/arrow-left.png";
		} else if (msg.type >= 8 && msg.type <= 11) {
			iconPath = "romfs:/discord-icons/boostgem.png";
		} else if (msg.type == 3) {
			iconPath = "romfs:/discord-icons/phone.png";
		} else {
			iconPath = "romfs:/discord-icons/chat.png";
		}

		if (authorName.empty()) {
			authorName = "Discord";
		}

		C3D_Tex *tex = ImageManager::getInstance().getLocalImage(iconPath, true);
		if (tex) {
			float iconSize = 14.0f;
			Tex3DS_SubTexture subtex = {(u16)tex->width, (u16)tex->height, 0.0f, 1.0f, 1.0f, 0.0f};
			C2D_Image img = {tex, &subtex};
			C2D_ImageTint tint;
			C2D_PlainImageTint(&tint, iconColor, 1.0f);

			float scaleX = iconSize / tex->width;
			float scaleY = iconSize / tex->height;

			C2D_DrawImageAt(img, 17.0f, drawY, 0.5f, &tint, scaleX, scaleY);
		} else {
			drawText(12.0f, drawY, 0.55f, 0.35f, 0.35f, iconColor, "->");
		}
	}

	const float textOffsetX = 42.0f;
	float currentX = textOffsetX;
	drawRichText(currentX, drawY, 0.5f, 0.42f, 0.42f, nameColor, authorName);
	currentX += UI::measureRichText(authorName, 0.42f, 0.42f);

	if (msg.type == 1 || msg.type == 2) {
		std::string templateStr =
		    TR(msg.type == 1 ? "message.system.recipient_add" : "message.system.recipient_remove");
		std::string targetName = "";
		if (!msg.mentions.empty()) {
			targetName = msg.mentions[0].global_name.empty() ? msg.mentions[0].username : msg.mentions[0].global_name;
		}

		size_t pos = templateStr.find("{0}");
		if (pos != std::string::npos) {
			std::string before = templateStr.substr(0, pos);
			std::string after = templateStr.substr(pos + 3);

			if (!before.empty()) {
				drawRichText(currentX, drawY, 0.5f, 0.42f, 0.42f, ScreenManager::colorTextMuted(), before);
				currentX += UI::measureRichText(before, 0.42f, 0.42f);
			}

			drawRichText(currentX, drawY, 0.5f, 0.42f, 0.42f, nameColor, targetName);
			currentX += UI::measureRichText(targetName, 0.42f, 0.42f);

			if (!after.empty()) {
				drawRichText(currentX, drawY, 0.5f, 0.42f, 0.42f, ScreenManager::colorTextMuted(), after);
			}
		} else {
			drawRichText(currentX, drawY, 0.5f, 0.42f, 0.42f, ScreenManager::colorTextMuted(), text);
		}
	} else {
		drawRichText(currentX, drawY, 0.5f, 0.42f, 0.42f, ScreenManager::colorTextMuted(), text);
	}
	return height;
}

float MessageScreen::drawReplyPreview(const Discord::Message &msg, float x, float y) {
	if (msg.type != 19 || msg.referencedAuthorName.empty()) {
		return y;
	}

	std::string arrowPath = "romfs:/discord-icons/curve.png";
	auto arrowInfo = ImageManager::getInstance().getImageInfo(arrowPath);
	if (!arrowInfo.tex || arrowInfo.failed) {
		ImageManager::getInstance().getLocalImage(arrowPath, true);
		arrowInfo = ImageManager::getInstance().getImageInfo(arrowPath);
	}

	std::string author =
	    !msg.referencedAuthorNickname.empty() ? msg.referencedAuthorNickname : msg.referencedAuthorName;
	std::string colon = ": ";

	float prefixW = 12.0f;
	float authorW = UI::measureRichText(author, 0.35f, 0.35f);
	float colonW = UI::measureRichText(colon, 0.35f, 0.35f);

	float maxWidthRef = 310.0f - x - (prefixW + authorW + colonW);

	std::string cleanedContent = Utils::Markdown::stripFormatting(msg.referencedContent);
	std::replace(cleanedContent.begin(), cleanedContent.end(), '\n', ' ');
	std::replace(cleanedContent.begin(), cleanedContent.end(), '\r', ' ');

	auto lines = MessageUtils::wrapText(cleanedContent, maxWidthRef, 0.35f);
	std::string replyContent = "";
	if (!lines.empty()) {
		replyContent = lines[0];
		if (lines.size() > 1) {
			replyContent += "...";
		}
	}

	float curX = x;

	if (arrowInfo.tex) {
		float iconSize = 8.0f;
		float uMax = (float)arrowInfo.originalW / arrowInfo.tex->width;
		float vMax = (float)arrowInfo.originalH / arrowInfo.tex->height;
		Tex3DS_SubTexture subtex = {(u16)arrowInfo.originalW, (u16)arrowInfo.originalH, 0.0f, 1.0f, uMax, 1.0f - vMax};
		C2D_Image img = {arrowInfo.tex, &subtex};
		C2D_DrawImageAt(img, curX + 1.0f, y + 2.0f, 0.5f, nullptr, iconSize / arrowInfo.originalW,
		                iconSize / arrowInfo.originalH);
	} else {
		drawText(curX, y, 0.5f, 0.35f, 0.35f, ScreenManager::colorTextMuted(), "↳ ");
	}
	curX += prefixW;

	u32 authorColor = ScreenManager::colorTextMuted();
	if (msg.referencedAuthorColor != 0) {
		authorColor = C2D_Color32((msg.referencedAuthorColor >> 16) & 0xFF, (msg.referencedAuthorColor >> 8) & 0xFF,
		                          msg.referencedAuthorColor & 0xFF, 255);
	}

	drawRichText(curX, y, 0.5f, 0.35f, 0.35f, authorColor, author);
	curX += authorW;
	drawRichText(curX, y, 0.5f, 0.35f, 0.35f, ScreenManager::colorTextMuted(), colon);
	curX += colonW;

	drawRichText(curX, y, 0.5f, 0.35f, 0.35f, ScreenManager::colorTextMuted(), replyContent);
	return y + 12.0f;
}

float MessageScreen::drawForwardHeader(const Discord::Message &msg, float x, float y) {
	if (!msg.isForwarded) {
		return y;
	}

	std::string iconPath = "romfs:/discord-icons/arrow-angle-right-up.png";
	C3D_Tex *icon = UI::ImageManager::getInstance().getLocalImage(iconPath, true);
	if (icon) {
		float iconSize = 10.0f;
		Tex3DS_SubTexture subtex = {(u16)icon->width, (u16)icon->height, 0.0f, 1.0f, 1.0f, 0.0f};
		C2D_Image img = {icon, &subtex};

		C2D_ImageTint tint;
		C2D_PlainImageTint(&tint, ScreenManager::colorTextMuted(), 1.0f);

		C2D_DrawImageAt(img, x + 2.0f, y + 2.0f, 0.5f, &tint, iconSize / icon->width, iconSize / icon->height);

		drawText(x + iconSize + 6.0f, y, 0.5f, 0.38f, 0.38f, ScreenManager::colorTextMuted(), TR("message.forwarded"));
	} else {
		drawText(x + 2.0f, y, 0.5f, 0.38f, 0.38f, ScreenManager::colorTextMuted(), "-> " + TR("message.forwarded"));
	}

	return y + 15.0f;
}

u32 MessageScreen::authorNameColor(const Discord::Message &msg) {
	Discord::DiscordClient &client = Discord::DiscordClient::getInstance();

	int roleColor = 0;
	if (!msg.member.role_ids.empty()) {
		roleColor = client.getRoleColor(guildId, msg.member);
	}

	if (roleColor == 0) {
		roleColor = client.getRoleColor(guildId, msg.author.id);
		if (roleColor == 0 && !guildId.empty()) {
			Discord::Member cached = client.getMember(guildId, msg.author.id);
			if (cached.user_id.empty()) {
				uint64_t now = osGetTime();
				auto it = failedMemberFetches.find(msg.author.id);
				bool onCooldown = (it != failedMemberFetches.end() && now < it->second);

				if (!onCooldown && pendingMemberFetches.find(msg.author.id) == pendingMemberFetches.end()) {
					pendingMemberFetches.insert(msg.author.id);
					queuedMemberFetches.push_back(msg.author.id);
				}
			}
		}
	}

	if (roleColor == 0) {
		return ScreenManager::colorText();
	}
	return C2D_Color32((roleColor >> 16) & 0xFF, (roleColor >> 8) & 0xFF, roleColor & 0xFF, 255);
}

float MessageScreen::drawAuthorHeader(const Discord::Message &msg, float x, float y, bool showHeader) {
	if (!showHeader) {
		return y;
	}

	Discord::DiscordClient &client = Discord::DiscordClient::getInstance();

	std::string displayName;
	if (!msg.member.nickname.empty()) {
		displayName = msg.member.nickname;
	} else {
		displayName = client.getMemberDisplayName(guildId, msg.author.id, msg.author);
	}

	u32 nameColor = authorNameColor(msg);

	float avatarX = 10.0f;
	float avatarSize = 28.0f;

	C3D_Tex *avatarTex =
	    Discord::AvatarCache::getInstance().getAvatar(msg.author.id, msg.author.avatar, msg.author.discriminator);
	if (avatarTex) {
		Tex3DS_SubTexture subtex = {(u16)avatarTex->width, (u16)avatarTex->height, 0.0f, 1.0f, 1.0f, 0.0f};
		C2D_Image img = {avatarTex, &subtex};
		C2D_DrawImageAt(img, avatarX, y, 0.5f, nullptr, avatarSize / avatarTex->width, avatarSize / avatarTex->height);
	} else {
		Discord::AvatarCache::getInstance().prefetchAvatar(msg.author.id, msg.author.avatar, msg.author.discriminator);

		C2D_DrawRectSolid(avatarX, y, 0.5f, avatarSize, avatarSize, C2D_Color32(80, 80, 100, 255));
		std::string initial = Utils::Utf8::getFirstChar(displayName.empty() ? "?" : displayName);
		drawText(avatarX + 10, y + 8, 0.6f, 0.45f, 0.45f, C2D_Color32(255, 255, 255, 255), initial);
	}

	drawRichText(x, y - 2.0f, 0.5f, 0.45f, 0.45f, nameColor, displayName);
	float nameWidth = UI::measureRichText(displayName, 0.45f, 0.45f);
	float timeX = x + nameWidth + 8.0f;
	std::string time = MessageUtils::formatTimestamp(msg.timestamp);

	drawText(timeX, y, 0.5f, 0.35f, 0.35f, ScreenManager::colorTextMuted(), time);
	return y + 14.0f;
}

float MessageScreen::drawMessageContent(const Discord::Message &msg, float x, float y) {
	std::string content = msg.content;
	if (content.empty()) {
		return y;
	}

	int emojiCount = 0;
	float newY = y;
	float lastLineEndX = -1.0f;
	float lastLineHeight = 12.0f;
	bool appendEdited = false;

	if (MessageUtils::isEmojiOnly(content, emojiCount) && emojiCount <= 10) {
		float jumboScale = (emojiCount <= 3) ? 1.15f : 0.85f;
		float lineHeight = (emojiCount <= 3) ? 34.0f : 26.0f;
		drawRichText(x, newY, 0.5f, jumboScale, jumboScale, ScreenManager::colorText(), content);
		newY += lineHeight;
	} else if (!content.empty()) {
		const auto &layout = UI::MarkdownRenderer::get(content, 350.0f, 0.4f);
		bool reveal = revealedSpoilers.count(msg.id) > 0;
		UI::MarkdownRenderer::draw(layout, x, newY, 0.5f, ScreenManager::colorText(), (size_t)-1, reveal);
		newY += layout.height;
		lastLineEndX = layout.lastLineEndX;
		lastLineHeight = layout.lastLineHeight;
		appendEdited = editedFitsOnLastLine(layout, 350.0f);
	}

	if (!msg.edited_timestamp.empty()) {
		std::string editedText = TR("message.edited");
		float editedScale = 0.35f;
		float padding = 4.0f;

		if (appendEdited) {
			drawText(x + lastLineEndX + padding, newY - lastLineHeight + 2.0f, 0.5f, editedScale, editedScale,
			         ScreenManager::colorTextMuted(), editedText);
		} else {
			drawText(x, newY, 0.5f, editedScale, editedScale, ScreenManager::colorTextMuted(), editedText);
			newY += 12.0f;
		}
	}
	return newY;
}

float MessageScreen::drawAttachments(const Discord::Message &msg, float x, float y, float maxWidth) {
	if (msg.attachments.empty()) {
		return y;
	}

	float newY = y;
	for (const auto &attach : msg.attachments) {
		if (attach.content_type.find("image/") != std::string::npos ||
		    attach.filename.find(".png") != std::string::npos || attach.filename.find(".jpg") != std::string::npos ||
		    attach.filename.find(".jpeg") != std::string::npos) {

			std::string imageUrl = attach.proxy_url.empty() ? attach.url : attach.proxy_url;
			auto info = ImageManager::getInstance().getImageInfo(imageUrl);

			float mediaMaxWidth = std::min(maxWidth, 330.0f);
			float maxHeight = 260.0f;
			float drawW = mediaMaxWidth;
			float drawH = 100.0f;

			int imgW = attach.width;
			int imgH = attach.height;
			if (info.tex) {
				imgW = info.originalW;
				imgH = info.originalH;
			}

			if (imgW > 0 && imgH > 0) {
				float aspect = (float)imgW / imgH;
				drawW = std::min((float)imgW, mediaMaxWidth);
				if (imgW > 160) {
					drawW = mediaMaxWidth;
				}

				drawH = drawW / aspect;
				if (drawH > maxHeight) {
					drawH = maxHeight;
					drawW = drawH * aspect;
				}
			} else {
				drawW = std::min(mediaMaxWidth, 160.0f);
				drawH = drawW * 0.75f;
			}

			if (info.tex) {

				float uMax = (float)info.originalW / info.tex->width;
				float vMax = (float)info.originalH / info.tex->height;
				Tex3DS_SubTexture subtex = {(u16)info.originalW, (u16)info.originalH, 0.0f, 1.0f, uMax, 1.0f - vMax};

				const C2D_Image img = {info.tex, &subtex};
				C2D_DrawImageAt(img, x, newY, 0.5f, nullptr, drawW / info.originalW, drawH / info.originalH);
				newY += drawH + 4.0f;
			} else if (info.failed) {
				u32 errorBg = (Config::getInstance().getThemeType() == 1) ? C2D_Color32(255, 235, 235, 255)
				                                                          : C2D_Color32(60, 40, 40, 255);
				C2D_DrawRectSolid(x, newY, 0.5f, drawW, drawH, errorBg);
				drawText(x + 5, newY + (drawH / 2) - 6, 0.5f, 0.3f, 0.3f, ScreenManager::colorError(),
				         TR("message.image_failed"));
				newY += drawH + 4.0f;
			} else {
				ImageManager::getInstance().prefetch(imageUrl, attach.width, attach.height,
				                                     Network::RequestPriority::INTERACTIVE);

				u32 placeholderBg = ScreenManager::colorBackgroundDark();
				C2D_DrawRectSolid(x, newY, 0.5f, drawW, drawH, placeholderBg);
				drawText(x + 5, newY + (drawH / 2) - 6, 0.5f, 0.3f, 0.3f, ScreenManager::colorTextMuted(),
				         TR("common.loading"));
				newY += drawH + 4.0f;
			}
		} else {
			std::string fileInfo = Core::I18n::format(TR("message.file"), attach.filename);
			drawText(x, newY, 0.5f, 0.35f, 0.35f, ScreenManager::colorTextMuted(), fileInfo);
			newY += 12.0f;
		}
	}
	return newY;
}

float MessageScreen::drawStickers(const Discord::Message &msg, float x, float y, float maxWidth) {
	if (msg.stickers.empty()) {
		return y;
	}

	float newY = y;
	for (const auto &sticker : msg.stickers) {
		if (sticker.format_type == 3) {
			std::string label = Core::I18n::format(TR("message.sticker"), sticker.name);
			drawText(x, newY, 0.5f, 0.35f, 0.35f, ScreenManager::colorTextMuted(), label);
			newY += 12.0f;
			continue;
		}

		std::string ext = ".png";
		if (sticker.format_type == 4) {
			ext = ".gif";
		}

		std::string stickerUrl = "https://cdn.discordapp.com/stickers/" + sticker.id + ext;
		float stickerSize = 100.0f;

		UI::ImageManager::ImageInfo info = UI::ImageManager::getInstance().getImageInfo(stickerUrl);
		if (info.tex) {
			float uMax = (float)info.originalW / info.tex->width;
			float vMax = (float)info.originalH / info.tex->height;
			Tex3DS_SubTexture subtex = {(u16)info.originalW, (u16)info.originalH, 0.0f, 1.0f, uMax, 1.0f - vMax};
			const C2D_Image img = {info.tex, &subtex};
			C2D_DrawImageAt(img, x, newY, 0.5f, nullptr, stickerSize / info.originalW, stickerSize / info.originalH);
			newY += stickerSize + 4.0f;
		} else if (info.failed) {
			u32 stickerBg = (Config::getInstance().getThemeType() == 1) ? C2D_Color32(255, 235, 235, 255)
			                                                            : C2D_Color32(60, 40, 40, 255);
			C2D_DrawRectSolid(x, newY, 0.5f, stickerSize, stickerSize, stickerBg);
			drawText(x + 5, newY + (stickerSize / 2) - 6, 0.55f, 0.3f, 0.3f, ScreenManager::colorError(),
			         TR("message.sticker_failed"));
			newY += stickerSize + 4.0f;
		} else {
			UI::ImageManager::getInstance().prefetch(stickerUrl, 160, 160, Network::RequestPriority::INTERACTIVE);
			u32 stickerBg = ScreenManager::colorBackgroundDark();
			C2D_DrawRectSolid(x, newY, 0.5f, stickerSize, stickerSize, stickerBg);
			drawText(x + 5, newY + (stickerSize / 2) - 6, 0.55f, 0.3f, 0.3f, ScreenManager::colorTextMuted(),
			         TR("common.loading"));
			newY += stickerSize + 4.0f;
		}
	}
	return newY;
}

float MessageScreen::drawReactions(const Discord::Message &msg, float x, float y, bool isSelected) {
	if (msg.reactions.empty()) {
		return y;
	}

	float reactionX = x;
	float rowHeight = 21.0f;
	float gap = 4.0f;
	float newY = y + 3.0f;

	struct ReactionDrawInfo {
		float x;
		float y;
		float boxW;
		const Discord::Reaction *react;
	};
	std::vector<ReactionDrawInfo> drawInfos;

	for (const auto &react : msg.reactions) {
		std::string countStr = std::to_string(react.count);
		float countW = UI::measureText(countStr, 0.4f, 0.4f);
		float emojiW = 18.0f;
		float boxW = emojiW + countW + 10.0f;

		if (reactionX + boxW > x + 320.0f) {
			reactionX = x;
			newY += rowHeight + gap;
		}

		u32 boxBg = react.me ? ScreenManager::colorReactionMe() : ScreenManager::colorReaction();

		if (isSelected) {
			u8 r, g, b;
			r = (boxBg >> 0) & 0xFF;
			g = (boxBg >> 8) & 0xFF;
			b = (boxBg >> 16) & 0xFF;
			boxBg = C2D_Color32(std::min(r + 20, 255), std::min(g + 20, 255), std::min(b + 20, 255), 255);
		}

		if (react.me) {
			drawRoundedRect(reactionX, newY, 0.45f, boxW, rowHeight, 6.0f, ScreenManager::colorSelection());
			drawRoundedRect(reactionX + 1.0f, newY + 1.0f, 0.46f, boxW - 2.0f, rowHeight - 2.0f, 5.0f, boxBg);
		} else {
			drawRoundedRect(reactionX, newY, 0.45f, boxW, rowHeight, 6.0f, boxBg);
		}

		drawInfos.push_back({reactionX, newY, boxW, &react});
		reactionX += boxW + gap;
	}

	UI::EmojiManager &emojiMgr = UI::EmojiManager::getInstance();

	for (const auto &info : drawInfos) {
		float emojiX = info.x + 4.0f;
		float emojiY = info.y + 2.0f;
		const auto &react = *info.react;

		EmojiManager::EmojiInfo emojiInfo;
		if (!react.emoji.id.empty()) {
			emojiInfo = emojiMgr.getEmojiInfo(react.emoji.id);
		} else {
			std::string hex = Utils::Utf8::utf8ToHex(react.emoji.name);
			emojiInfo = emojiMgr.getTwemojiInfo(hex);
		}

		if (emojiInfo.tex) {
			float uMax = (float)emojiInfo.originalW / emojiInfo.tex->width;
			float vMax = (float)emojiInfo.originalH / emojiInfo.tex->height;
			Tex3DS_SubTexture subtex = {
			    (u16)emojiInfo.originalW, (u16)emojiInfo.originalH, 0.0f, 1.0f, uMax, 1.0f - vMax};
			float scale = std::min(16.0f / emojiInfo.originalW, 16.0f / emojiInfo.originalH);
			float dx = emojiX + (16.0f - emojiInfo.originalW * scale) / 2.0f;
			float dy = emojiY + (16.0f - emojiInfo.originalH * scale) / 2.0f;
			C2D_DrawImageAt({emojiInfo.tex, &subtex}, dx, dy, 0.47f, nullptr, scale, scale);
		} else if (!react.emoji.id.empty()) {
			emojiMgr.prefetchEmoji(react.emoji.id);
			drawText(emojiX, emojiY + 2.0f, 0.47f, 0.4f, 0.4f, ScreenManager::colorTextMuted(), "?");
		} else {
			drawText(emojiX, emojiY + 2.0f, 0.47f, 0.5f, 0.5f, ScreenManager::colorText(), react.emoji.name);
		}

		std::string countStr = std::to_string(react.count);
		drawText(info.x + 18.0f + 6.0f, info.y + 5.0f, 0.47f, 0.4f, 0.4f,
		         react.me ? ScreenManager::colorText() : ScreenManager::colorTextMuted(), countStr);
	}

	return newY + rowHeight + 4.0f;
}

float MessageScreen::drawMessage(const Discord::Message &msg, float y, float maxWidth, bool isSelected,
                                 bool showHeader) {
	float height = calculateMessageHeight(msg, showHeader);
	float topMargin = showHeader ? 4.0f : 0.0f;
	const float textOffsetX = 42.0f;

	if (isForumView) {
		return drawForumMessage(msg, y, isSelected);
	}

	if (isSelected) {
		float highlightY = y + topMargin;
		float highlightH = height - topMargin;
		drawRoundedRect(4.0f, highlightY, 0.1f, 392.0f, highlightH, 6.0f, ScreenManager::colorBackgroundLight());
	}

	if (msg.type != 0 && msg.type != 19) {
		drawSystemMessage(msg, y, topMargin, height);
		drawReactions(msg, textOffsetX, y + topMargin + 18.0f, isSelected);
		return height;
	}

	float contentY = y + topMargin + 1.0f;
	contentY = drawReplyPreview(msg, textOffsetX, contentY);

	float avatarTopY = contentY;
	contentY = drawAuthorHeader(msg, textOffsetX, contentY, showHeader);

	float forwardedBarStartY = contentY;
	contentY = drawForwardHeader(msg, textOffsetX, contentY);

	if (!showHeader && isSelected) {
		std::string time = MessageUtils::formatTimeOnly(msg.timestamp);
		drawText(10.0f, contentY + 2.0f, 0.5f, 0.35f, 0.35f, ScreenManager::colorTextMuted(), time);
	}

	contentY = drawMessageContent(msg, textOffsetX, contentY);

	if (!msg.embeds.empty()) {
		for (const auto &embed : msg.embeds) {
			contentY += renderEmbed(embed, textOffsetX, contentY, 400.0f - textOffsetX - 10.0f);
			contentY += 6.0f;
		}
	}

	float attachmentMaxWidth = 400.0f - textOffsetX - 10.0f;
	contentY = drawAttachments(msg, textOffsetX, contentY, attachmentMaxWidth);
	contentY = drawStickers(msg, textOffsetX, contentY, attachmentMaxWidth);

	if (msg.isForwarded) {
		float barX = 38.0f;
		float barW = 2.0f;
		float barH = contentY - forwardedBarStartY;
		C2D_DrawRectSolid(barX, forwardedBarStartY, 0.45f, barW, barH, ScreenManager::colorTextMuted());
	}

	contentY = drawReactions(msg, textOffsetX, contentY, isSelected);

	if (showHeader) {
		if (contentY < avatarTopY + 28.0f) {
			contentY = avatarTopY + 28.0f;
		}
	}

	return height;
}

void MessageScreen::renderTop(C3D_RenderTarget *target) {
	C2D_TargetClear(target, ScreenManager::colorBackground());
	C2D_SceneBegin(target);

	const float SCREEN_HEIGHT = 240.0f;

	if (isLoading) {
		drawCenteredRichText(110.0f, 0.5f, 0.6f, 0.6f, ScreenManager::colorTextMuted(),
		                     Core::I18n::getInstance().get("common.loading"), 400.0f);
		return;
	}

	std::lock_guard<std::recursive_mutex> lock(messageMutex);

	if (isHiddenChannel) {
		drawCenteredRichText(110.0f, 0.5f, 0.6f, 0.6f, ScreenManager::colorTextMuted(),
		                     Core::I18n::getInstance().get("message.no_view_permission"), 400.0f);
		return;
	}

	if (this->messages.empty() && !isLoading) {
		drawCenteredRichText(110.0f, 0.5f, 0.6f, 0.6f, ScreenManager::colorTextMuted(),
		                     Core::I18n::getInstance().get("message.no_messages"), 400.0f);
		return;
	}

	if (isFetchingHistory) {
		drawCenteredRichText(5.0f, 0.55f, 0.4f, 0.4f, ScreenManager::colorTextMuted(),
		                     Core::I18n::getInstance().get("message.loading_history"), 400.0f);
	}

	float yOffset = std::max(0.0f, SCREEN_HEIGHT - totalContentHeight);

	float yStart = -currentScrollY + yOffset;
	const float MARGIN = 10.0f;
	const float TOP_MARGIN = 30.0f;

	for (size_t i = 0; i < messages.size(); i++) {
		if (i >= messagePositions.size() || i >= messageHeights.size()) {
			break;
		}

		float msgY = yStart + messagePositions[i];
		float msgH = messageHeights[i];

		if (msgY + msgH < -TOP_MARGIN || msgY > SCREEN_HEIGHT + MARGIN) {
			continue;
		}

		bool showDateSeparator = false;
		std::string currDate = "";

		if (i == 0) {
			showDateSeparator = true;
			currDate = MessageUtils::getLocalDateString(this->messages[i].timestamp);
		} else if (this->messages[i].timestamp != "Sending...") {
			currDate = MessageUtils::getLocalDateString(this->messages[i].timestamp);
			std::string prevDate = MessageUtils::getLocalDateString(this->messages[i - 1].timestamp);
			if (currDate != prevDate) {
				showDateSeparator = true;
			}
		}

		float dateY = msgY - 20.0f;

		float renderTopY = showDateSeparator ? dateY : msgY;
		float renderBottomY = msgY + msgH;

		const float MARGIN = 10.0f;

		const float TOP_MARGIN = showDateSeparator ? 30.0f : MARGIN;

		if (renderBottomY < -TOP_MARGIN || renderTopY > SCREEN_HEIGHT + MARGIN) {
			continue;
		}

		if (showDateSeparator) {
			if (dateY > -30.0f && dateY < SCREEN_HEIGHT) {
				float lineY = dateY + 7.0f;
				u32 lineColor = C2D_Color32(80, 80, 85, 255);
				C2D_DrawRectSolid(10.0f, lineY, 0.7f, 130.0f, 1.0f, lineColor);
				C2D_DrawRectSolid(260.0f, lineY, 0.7f, 130.0f, 1.0f, lineColor);

				float dateW = UI::measureText(currDate, 0.4f, 0.4f);
				float dateX = (400.0f - dateW) / 2.0f;
				drawText(dateX, dateY, 0.7f, 0.4f, 0.4f, ScreenManager::colorTextMuted(), currDate);
			}
		}

		bool isSelected = (i == (size_t)selectedIndex);
		bool showHeader = (i == 0) || !MessageUtils::canGroupWithPrevious(messages[i], messages[i - 1]);

		if (showDateSeparator) {
			showHeader = true;
		}

		drawMessage(this->messages[i], msgY, 400.0f, isSelected, showHeader);
	}

	if (showNewMessageIndicator) {
		float indicatorY = 205.0f;
		float indicatorW = 130.0f;
		float indicatorH = 22.0f;
		float indicatorX = (400.0f - indicatorW) / 2.0f;

		drawRoundedRect(indicatorX, indicatorY, 0.65f, indicatorW, indicatorH, 11.0f, ScreenManager::colorSelection());
		std::string text = TR("message.new_indicator");
		if (newMessageCount > 0) {
			text = Core::I18n::format(TR("message.new_indicator_count"), std::to_string(newMessageCount));
		}
		drawCenteredRichText(indicatorY + 5.0f, 0.66f, 0.4f, 0.4f, ScreenManager::colorWhite(), text, 400.0f);
	}

	if (isMenuOpen) {
		renderMenu();
	}
}

void MessageScreen::renderBottom(C3D_RenderTarget *target) {
	C2D_DrawRectSolid(0, 0, 0.0f, 320, 240, ScreenManager::colorBackgroundDark());

	if (bottomMode == BottomScreenMode::EMOJI_PICKER) {
		const Discord::Message *activeMsg = nullptr;
		{
			std::lock_guard<std::recursive_mutex> lock(messageMutex);
			if (selectedIndex >= 0 && selectedIndex < (int)messages.size()) {
				activeMsg = &messages[selectedIndex];
			}
		}
		emojiPicker->render(target, activeMsg);
		return;
	}

	float headerX = 35.0f;

	std::string iconPath;
	if (!rulesChannelId.empty() && channelId == rulesChannelId) {
		iconPath = "romfs:/discord-icons/bookcheck.png";
	} else if (channelType == 5) {
		iconPath = "romfs:/discord-icons/announcement.png";
	} else if (channelType == 10 || channelType == 11 || channelType == 12 || channelType == 1 || channelType == 3 ||
	           channelType == 2 || channelType == 13) {
		iconPath = "romfs:/discord-icons/chat.png";
	} else {
		iconPath = "romfs:/discord-icons/text.png";
	}

	C3D_Tex *icon = nullptr;
	bool isAvatar = false;

	if (channelType == 1 || channelType == 3) {
		Discord::Channel ch = Discord::DiscordClient::getInstance().getChannel(channelId);
		if (channelType == 3 && !ch.icon.empty()) {
			icon = Discord::AvatarCache::getInstance().getChannelIcon(ch.id, ch.icon);
		} else if (channelType == 1 && !ch.recipients.empty()) {
			const auto &r = ch.recipients[0];
			icon = Discord::AvatarCache::getInstance().getAvatar(r.id, r.avatar, r.discriminator);
		}
		if (icon) {
			isAvatar = true;
		}
	}

	if (!icon) {
		icon = UI::ImageManager::getInstance().getLocalImage(iconPath);
	}

	if (icon) {
		float iconSize = 16.0f;
		Tex3DS_SubTexture subtex = {(u16)icon->width, (u16)icon->height, 0.0f, 1.0f, 1.0f, 0.0f};
		C2D_Image img = {icon, &subtex};

		C2D_ImageTint *tintPtr = nullptr;
		C2D_ImageTint tint;
		if (!isAvatar) {
			C2D_PlainImageTint(&tint, ScreenManager::colorText(), 1.0f);
			tintPtr = &tint;
		}

		C2D_DrawImageAt(img, 35.0f, 10.0f, 0.51f, tintPtr, iconSize / icon->width, iconSize / icon->height);
		headerX = 35.0f + iconSize + 5.0f;
	} else {
		drawText(35.0f, 10.0f, 0.5f, 0.5f, 0.5f, ScreenManager::colorTextMuted(), "#");
		headerX = 50.0f;
	}

	drawRichText(headerX, 10.0f, 0.5f, 0.55f, 0.55f, ScreenManager::colorText(), truncatedChannelName);

	C2D_DrawRectSolid(10, 32, 0.5f, 320 - 20, 1, ScreenManager::colorSeparator());

	std::vector<Discord::VoiceParticipant> participants = callParticipants();
	if (isCallActive() || !participants.empty()) {
		renderCallParticipants(40.0f, participants);
	} else {
		std::string displayTopic =
		    channelTopic.empty() ? Core::I18n::getInstance().get("common.no_topic") : channelTopic;

		float topicY = 40.0f;

		drawText(10.0f, topicY, 0.5f, 0.45f, 0.45f, ScreenManager::colorSelection(),
		         Core::I18n::getInstance().get("message.topic"));
		topicY += 15.0f;

		const auto &topicLayout = UI::MarkdownRenderer::get(displayTopic, 300.0f, 0.4f, 13.0f / 0.4f);
		UI::MarkdownRenderer::draw(topicLayout, 10.0f, topicY, 0.5f, ScreenManager::colorText(), 10);
		topicY += UI::MarkdownRenderer::heightOf(topicLayout, 10);
	}

	bool canSend = Discord::DiscordClient::getInstance().canSendMessage(channelId);

	std::string hints = "\uE079\uE07A: " + TR("common.navigate") + "  ";
	if (isMenuOpen) {
		hints += "\uE000: " + TR("common.select") + "  \uE001: " + TR("common.close");
	} else if (isForumView) {
		hints += "\uE000: " + TR("common.open") + "  \uE001: " + TR("common.back");
	} else {
		if (canSend) {
			hints += "\uE003: " + TR("common.type") + "  ";
		}
		hints += "\uE002: " + TR("common.menu") + "  \uE001: " + TR("common.back");
	}

	drawText(10.0f, BOTTOM_SCREEN_HEIGHT - 25.0f, 0.5f, 0.4f, 0.4f, ScreenManager::colorTextMuted(), hints);

	auto typingUsers = Discord::DiscordClient::getInstance().getTypingUsers(channelId);
	if (!typingUsers.empty()) {
		std::string typingText = "";
		if (typingUsers.size() == 1) {
			typingText = typingUsers[0].displayName + " " + TR("common.is_typing");
		} else if (typingUsers.size() <= 3) {
			for (size_t i = 0; i < typingUsers.size(); i++) {
				typingText += typingUsers[i].displayName;
				if (i < typingUsers.size() - 1) {
					typingText += ", ";
				}
			}
			typingText += " " + TR("common.are_typing");
		} else {
			typingText = TR("common.several_users_typing");
		}

		drawText(10.0f, BOTTOM_SCREEN_HEIGHT - 50.0f, 0.5f, 0.4f, 0.4f, ScreenManager::colorSelection(), typingText);
	}

	const float SCREEN_HEIGHT = 240.0f;
	float maxScroll = std::max(0.0f, totalContentHeight - SCREEN_HEIGHT);

	if (targetScrollY < maxScroll - 10.0f) {
		float btnW = 30.0f;
		float btnH = 30.0f;
		float btnX = 320.0f - btnW - 10.0f;
		float btnY = bottomButtonY();

		drawRoundedRect(btnX, btnY, 0.54f, btnW, btnH, 8.0f, ScreenManager::colorBackgroundLight());

		float centerX = btnX + (btnW / 2.0f);
		float centerY = btnY + (btnH / 2.0f);

		C2D_DrawTriangle(centerX - 6, centerY - 3, ScreenManager::colorText(), centerX + 6, centerY - 3,
		                 ScreenManager::colorText(), centerX, centerY + 4, ScreenManager::colorText(), 0.55f);
		C2D_DrawRectSolid(centerX - 6, centerY + 5, 0.55f, 12, 1.5f, ScreenManager::colorText());
	}

	renderReactionIcon();
	VoiceControls::draw(VOICE_BTN_X, VOICE_BTN_Y);
}

void MessageScreen::fetchOlderMessages() {
	if (this->messages.empty()) {
		isFetchingHistory = false;
		return;
	}

	std::string beforeId = this->messages.front().id;
	Discord::DiscordClient &client = Discord::DiscordClient::getInstance();

	client.fetchMessagesBeforeAsync(
	    channelId, beforeId, 25, [this, token = aliveToken](const std::vector<Discord::Message> &olderMessages) {
		    if (!*token) {
			    return;
		    }
		    if (!olderMessages.empty()) {
			    std::vector<Discord::Message> reversed = olderMessages;
			    std::reverse(reversed.begin(), reversed.end());

			    float oldTotalHeight = totalContentHeight;
			    size_t addedCount = 0;

			    {
				    std::lock_guard<std::recursive_mutex> lock(messageMutex);
				    this->messages.insert(this->messages.begin(), reversed.begin(), reversed.end());
				    selectedIndex += reversed.size();
				    addedCount = reversed.size();
			    }

			    rebuildLayoutCache();

			    float heightDiff = totalContentHeight - oldTotalHeight;
			    currentScrollY += heightDiff;
			    targetScrollY += heightDiff;

			    Logger::log("Loaded %d older messages async, adjusted scroll by %.2f", addedCount, heightDiff);
		    } else {

			    hasMoreHistory = false;
			    Logger::log("End of history reached for channel %s", channelId.c_str());
		    }

		    isFetchingHistory = false;
	    });
}

void MessageScreen::openKeyboard() {
	auto &client = Discord::DiscordClient::getInstance();
	if (!client.canSendMessage(channelId)) {
		ScreenManager::getInstance().showToast(TR("message.no_permission"));
		return;
	}

	client.triggerTypingIndicator(channelId);

	auto res = runKeyboard(TR("common.message_hint"));

	if (res.button == SWKBD_BUTTON_RIGHT && !res.text.empty()) {
		std::lock_guard<std::recursive_mutex> lock(messageMutex);

		Discord::Message optimisticMsg;
		optimisticMsg.id = "pending_" + std::to_string(osGetTime());
		optimisticMsg.nonce = optimisticMsg.id;
		optimisticMsg.content = res.text;
		optimisticMsg.channelId = channelId;
		optimisticMsg.author = client.getCurrentUser();
		optimisticMsg.timestamp = TR("message.status.sending");
		optimisticMsg.type = 0;

		this->messages.push_back(optimisticMsg);
		rebuildLayoutCache();
		scrollToBottom();

		client.sendMessage(
		    channelId, res.text,
		    [this, pendingId = optimisticMsg.id](const Discord::Message &sentMsg, bool success, int errorCode) {
			    std::lock_guard<std::recursive_mutex> lock(messageMutex);
			    for (auto &msg : this->messages) {
				    if (msg.id == pendingId) {
					    if (success) {
						    msg = sentMsg;
						    Logger::log("Updated pending message with confirmed ID: %s", sentMsg.id.c_str());
					    } else {
						    msg.timestamp = TR("message.status.failed");
						    Logger::log("Message send failed with code: %d", errorCode);
					    }
					    break;
				    }
			    }
			    rebuildLayoutCache();
			    scrollToBottom();
		    },
		    optimisticMsg.nonce);
	}
}

MessageScreen::KeyboardResult MessageScreen::runKeyboard(const std::string &hint, const std::string &initialText) {
	SwkbdState swkbd;
	char mybuf[2000];
	swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, -1);
	swkbdSetFeatures(&swkbd, SWKBD_PREDICTIVE_INPUT | SWKBD_DARKEN_TOP_SCREEN | SWKBD_ALLOW_HOME | SWKBD_ALLOW_RESET |
	                             SWKBD_ALLOW_POWER | SWKBD_MULTILINE);

	if (!initialText.empty()) {
		swkbdSetInitialText(&swkbd, initialText.c_str());
	}
	swkbdSetHintText(&swkbd, hint.c_str());
	swkbdSetButton(&swkbd, SWKBD_BUTTON_LEFT, TR("common.cancel").c_str(), false);
	swkbdSetButton(&swkbd, SWKBD_BUTTON_RIGHT, TR("common.send").c_str(), true);

	SwkbdButton button = swkbdInputText(&swkbd, mybuf, sizeof(mybuf));
	std::string content = (button == SWKBD_BUTTON_RIGHT) ? mybuf : "";

	if (!content.empty()) {
		size_t first = content.find_first_not_of(" \n\r\t");
		if (first != std::string::npos) {
			content = content.substr(first, content.find_last_not_of(" \n\r\t") - first + 1);
		} else {
			content = "";
		}
	}

	return {(int)button, content};
}

void MessageScreen::showMessageOptions() {
	if (isForumView) {
		return;
	}

	if (this->messages.empty() || selectedIndex < 0 || selectedIndex >= (int)this->messages.size()) {
		return;
	}

	const Discord::Message &msg = this->messages[selectedIndex];
	if (msg.type != 0 && msg.type != 19 && msg.type != 7) {
		return;
	}

	Discord::DiscordClient &client = Discord::DiscordClient::getInstance();
	bool isMine = (msg.author.id == client.getCurrentUser().id);

	menuOptions.clear();
	menuActions.clear();

	auto addOption = [&](const std::string &actionId, const std::string &i18nKey) {
		menuActions.push_back(actionId);
		menuOptions.push_back(Core::I18n::getInstance().get(i18nKey));
	};

	bool canSend = client.canSendMessage(channelId);

	if (!msg.content.empty() && UI::MarkdownRenderer::get(msg.content, 350.0f, 0.4f).hasSpoiler) {
		addOption("ToggleSpoiler",
		          revealedSpoilers.count(msg.id) ? "message.menu.hide_spoiler" : "message.menu.reveal_spoiler");
	}

	if (canSend) {
		addOption("Reply", "message.menu.reply");
	}

	if (isMine && canSend) {
		addOption("Edit", "message.menu.edit");
	}

	if (isMine || client.canManageMessages(channelId)) {
		addOption("Delete", "message.menu.delete");
	}

	bool hasFailed = false;
	for (const auto &attach : msg.attachments) {
		std::string url = attach.proxy_url.empty() ? attach.url : attach.proxy_url;
		if (ImageManager::getInstance().getImageInfo(url).failed ||
		    ImageManager::getInstance().getImageInfo(attach.url).failed) {
			hasFailed = true;
			break;
		}
	}
	if (!hasFailed) {
		for (const auto &sticker : msg.stickers) {
			std::string ext = (sticker.format_type == 4) ? ".gif" : ".png";
			std::string url = "https://cdn.discordapp.com/stickers/" + sticker.id + ext;
			if (ImageManager::getInstance().getImageInfo(url).failed) {
				hasFailed = true;
				break;
			}
		}
	}
	if (!hasFailed) {
		for (const auto &embed : msg.embeds) {
			if (!embed.image_url.empty() && (ImageManager::getInstance().getImageInfo(embed.image_url).failed ||
			                                 ImageManager::getInstance().getImageInfo(embed.image_proxy_url).failed)) {
				hasFailed = true;
				break;
			}
			if (!embed.thumbnail_url.empty() &&
			    (ImageManager::getInstance().getImageInfo(embed.thumbnail_url).failed ||
			     ImageManager::getInstance().getImageInfo(embed.thumbnail_proxy_url).failed)) {
				hasFailed = true;
				break;
			}
		}
	}

	if (hasFailed) {
		addOption("Retry", "message.menu.retry");
	}

	addOption("Cancel", "message.menu.cancel");

	isMenuOpen = true;
	menuIndex = 0;
}

bool MessageScreen::isAtBottom() const {
	float maxScroll = std::max(0.0f, totalContentHeight - 240.0f);
	return targetScrollY >= maxScroll - 5.0f;
}

void MessageScreen::syncScrollAfterRebuild(bool wasAtBottom, bool updateSelection) {
	if (!wasAtBottom) {
		return;
	}
	if (bottomMode != BottomScreenMode::EMOJI_PICKER) {
		if (updateSelection && !messages.empty()) {
			selectedIndex = (int)messages.size() - 1;
		}
		scrollToBottom();
	} else {
		float maxScroll = std::max(0.0f, totalContentHeight - 240.0f);
		targetScrollY = maxScroll;
		currentScrollY = maxScroll;
	}
}

void MessageScreen::scrollToBottom() {
	if (this->messages.empty()) {
		return;
	}

	selectedIndex = this->messages.size() - 1;

	const float SCREEN_HEIGHT = 240.0f;
	float maxScroll = std::max(0.0f, totalContentHeight - SCREEN_HEIGHT);
	targetScrollY = maxScroll;
	currentScrollY = maxScroll;

	showNewMessageIndicator = false;
}

void MessageScreen::rebuildLayoutCache() {
	messagePositions.clear();
	messageHeights.clear();

	if (messages.empty()) {
		totalContentHeight = 0.0f;
		return;
	}

	float y = 10.0f;
	std::string lastDate = "";

	for (size_t i = 0; i < this->messages.size(); i++) {
		bool showHeader = (i == 0) || !MessageUtils::canGroupWithPrevious(this->messages[i], this->messages[i - 1]);

		if (this->messages[i].id.substr(0, 8) != "pending_") {
			std::string currDate = MessageUtils::getLocalDateString(this->messages[i].timestamp);
			if (this->messages[i].timestamp != TR("message.status.sending") &&
			    this->messages[i].timestamp != TR("message.status.failed")) {
				if (currDate != lastDate) {
					y += 28.0f;
					lastDate = currDate;
					showHeader = true;
				}
			}
		}

		for (const auto &react : this->messages[i].reactions) {
			if (!react.emoji.id.empty()) {
				EmojiManager::getInstance().prefetchEmoji(react.emoji.id);
			}
		}
		EmojiManager::getInstance().prefetchEmojisFromText(this->messages[i].content);

		messagePositions.push_back(y);
		float h = calculateMessageHeight(this->messages[i], showHeader);
		messageHeights.push_back(h);

		y += h;
	}

	totalContentHeight = y + 2.0f;

	const float SCREEN_HEIGHT = 240.0f;
	float maxScroll = std::max(0.0f, totalContentHeight - SCREEN_HEIGHT);

	if (targetScrollY > maxScroll) {
		targetScrollY = maxScroll;
	}
	if (currentScrollY > maxScroll) {
		currentScrollY = maxScroll;
	}
}

void MessageScreen::ensureSelectionVisible() {
	if (selectedIndex < 0 || selectedIndex >= (int)this->messages.size()) {
		return;
	}

	if (messagePositions.empty()) {
		return;
	}

	const float SCREEN_HEIGHT = 240.0f;
	const float TOP_MARGIN = 20.0f;
	const float BOTTOM_MARGIN = 20.0f;

	float msgY = messagePositions[selectedIndex];
	float msgH = messageHeights[selectedIndex];

	float visibleTop = targetScrollY;
	float visibleBottom = targetScrollY + SCREEN_HEIGHT;

	if (msgY < visibleTop + TOP_MARGIN) {
		targetScrollY = msgY - TOP_MARGIN;
	} else if (msgY + msgH > visibleBottom - BOTTOM_MARGIN) {
		targetScrollY = (msgY + msgH) - SCREEN_HEIGHT + BOTTOM_MARGIN;
	}

	float maxScroll = std::max(0.0f, totalContentHeight - SCREEN_HEIGHT);
	targetScrollY = std::clamp(targetScrollY, 0.0f, maxScroll);
}

void MessageScreen::renderMenu() {
	if (!isMenuOpen) {
		return;
	}

	drawOverlay(0.98f);

	float menuW = 200.0f;
	float menuH = menuOptions.size() * 25.0f + 9.0f;
	float menuX = (400.0f - menuW) / 2.0f;
	float menuY = (240.0f - menuH) / 2.0f;

	drawPopupBackground(menuX, menuY, menuW, menuH, 0.99f);

	for (size_t i = 0; i < menuOptions.size(); i++) {
		float itemY = menuY + 6.0f + i * 25.0f;
		u32 color = ScreenManager::colorText();

		bool isSelected = (i == (size_t)menuIndex);
		drawPopupMenuItem(menuX + 5.0f, itemY, menuW - 10.0f, 22.0f, 0.995f, isSelected,
		                  ScreenManager::colorSelection());

		if (isSelected) {
			color = ScreenManager::colorWhite();
		}

		drawCenteredRichText(itemY + 4.0f, 0.996f, 0.5f, 0.5f, color, menuOptions[i], 400.0f);
	}
}

float MessageScreen::calculateEmbedHeight(const Discord::Embed &embed, float maxWidth) {

	bool hasImage = !embed.image_url.empty();
	bool hasThumbnail = !embed.thumbnail_url.empty();
	std::string mediaUrl = hasImage
	                           ? (embed.image_proxy_url.empty() ? embed.image_url : embed.image_proxy_url)
	                           : (embed.thumbnail_proxy_url.empty() ? embed.thumbnail_url : embed.thumbnail_proxy_url);
	auto mediaInfo = ImageManager::getInstance().getImageInfo(mediaUrl);

	bool isLargeThumbnail = (hasThumbnail && embed.thumbnail_width >= 160 &&
	                         (float)embed.thumbnail_width > (float)embed.thumbnail_height * 1.2f);
	bool isMedia = (embed.type == "image" || embed.type == "gifv" || embed.type == "video" || embed.type == "article" ||
	                isLargeThumbnail);

	bool isSimpleMedia = isMedia && embed.title.empty() && embed.description.empty() && embed.fields.empty() &&
	                     embed.author_name.empty() && (hasImage || hasThumbnail);

	bool showThumbnailOnRight = !isSimpleMedia && hasThumbnail && !isMedia;
	float pixelWidth = maxWidth - (showThumbnailOnRight ? 76.0f : 16.0f);
	float h = isSimpleMedia ? 0.0f : 10.0f;

	if (!embed.provider_name.empty()) {
		h += 11.0f;
	}
	if (!embed.author_name.empty()) {
		auto lines = MessageUtils::wrapText(embed.author_name, pixelWidth, 0.38f, false);
		h += lines.size() * 11.0f;
	}
	using UI::MarkdownRenderer::EMBED_STYLES;
	if (!embed.title.empty()) {
		h += UI::MarkdownRenderer::get(embed.title, pixelWidth, 0.42f, 14.0f / 0.42f, EMBED_STYLES, false).height;
	}
	if (!embed.description.empty()) {
		h += UI::MarkdownRenderer::get(embed.description, pixelWidth, 0.36f, 11.0f / 0.36f, EMBED_STYLES, false).height;
	}

	for (const auto &field : embed.fields) {
		h += UI::MarkdownRenderer::get(field.name, pixelWidth, 0.35f, 11.0f / 0.35f, EMBED_STYLES, false).height;
		h += UI::MarkdownRenderer::get(field.value, pixelWidth, 0.34f, 11.0f / 0.34f, EMBED_STYLES, false).height;
		h += 2.0f;
	}

	if (!embed.footer_text.empty()) {
		auto lines = MessageUtils::wrapText(embed.footer_text, pixelWidth, 0.30f, false);
		h += lines.size() * 10.0f;
	}

	if (showThumbnailOnRight) {
		h = std::max(h, 72.0f);
	}

	if (hasImage || (isMedia && hasThumbnail)) {
		int imgW = hasImage ? embed.image_width : embed.thumbnail_width;
		int imgH = hasImage ? embed.image_height : embed.thumbnail_height;
		if (mediaInfo.tex) {
			imgW = mediaInfo.originalW;
			imgH = mediaInfo.originalH;
		}

		float availableMaxWidth = maxWidth - (isSimpleMedia ? 0.0f : 16.0f);
		availableMaxWidth = std::min(availableMaxWidth, 330.0f);
		float drawW = availableMaxWidth;

		float imgHeight = 100.0f;
		if (imgW > 0 && imgH > 0) {
			if (imgW < 160) {
				drawW = (float)imgW;
			}

			float aspect = (float)imgW / imgH;
			imgHeight = drawW / aspect;
			if (imgHeight > 220.0f) {
				imgHeight = 220.0f;
				drawW = imgHeight * aspect;
			}
		} else {
			imgHeight = drawW * 0.75f;
		}
		h += imgHeight + 4.0f;
	}

	return h;
}

float MessageScreen::renderEmbed(const Discord::Embed &embed, float x, float y, float maxWidth) {
	bool hasImage = !embed.image_url.empty();
	bool hasThumbnail = !embed.thumbnail_url.empty();

	bool isLargeThumbnail = (hasThumbnail && embed.thumbnail_width >= 160 &&
	                         (float)embed.thumbnail_width > (float)embed.thumbnail_height * 1.2f);
	bool isMedia = (embed.type == "image" || embed.type == "gifv" || embed.type == "video" || embed.type == "article" ||
	                isLargeThumbnail);

	bool isSimpleMedia = isMedia && embed.title.empty() && embed.description.empty() && embed.fields.empty() &&
	                     embed.author_name.empty() && (hasImage || hasThumbnail);

	u32 embedColor = embed.color != 0
	                     ? C2D_Color32((embed.color >> 16) & 0xFF, (embed.color >> 8) & 0xFF, embed.color & 0xFF, 255)
	                     : C2D_Color32(32, 102, 148, 255);
	float embedH = calculateEmbedHeight(embed, maxWidth);

	bool showThumbnailOnRight = !isSimpleMedia && hasThumbnail && !isMedia;
	float pixelWidth = maxWidth - (showThumbnailOnRight ? 76.0f : 16.0f);

	if (!isSimpleMedia) {
		C2D_DrawRectSolid(x, y, 0.4f, maxWidth, embedH, ScreenManager::colorEmbed());
		C2D_DrawRectSolid(x, y, 0.45f, 4.0f, embedH, embedColor);
	}

	float currentY = y + (isSimpleMedia ? 0.0f : 5.0f);
	float textX = x + (isSimpleMedia ? 0.0f : 8.0f);

	if (!embed.provider_name.empty()) {
		drawText(textX, currentY, 0.5f, 0.32f, 0.32f, ScreenManager::colorTextMuted(), embed.provider_name);
		currentY += 11.0f;
	}
	if (!embed.author_name.empty()) {
		auto lines = MessageUtils::wrapText(embed.author_name, pixelWidth, 0.38f, false);
		for (const auto &line : lines) {
			drawRichText(textX, currentY, 0.5f, 0.38f, 0.38f, ScreenManager::colorText(), line);
			currentY += 11.0f;
		}
	}
	using UI::MarkdownRenderer::EMBED_STYLES;
	if (!embed.title.empty()) {
		const auto &l = UI::MarkdownRenderer::get(embed.title, pixelWidth, 0.42f, 14.0f / 0.42f, EMBED_STYLES, false);
		UI::MarkdownRenderer::draw(l, textX, currentY, 0.5f, ScreenManager::colorText());
		currentY += l.height;
	}
	if (!embed.description.empty()) {
		const auto &l =
		    UI::MarkdownRenderer::get(embed.description, pixelWidth, 0.36f, 11.0f / 0.36f, EMBED_STYLES, false);
		UI::MarkdownRenderer::draw(l, textX, currentY, 0.5f, ScreenManager::colorText());
		currentY += l.height;
	}
	for (const auto &field : embed.fields) {
		const auto &n = UI::MarkdownRenderer::get(field.name, pixelWidth, 0.35f, 11.0f / 0.35f, EMBED_STYLES, false);
		UI::MarkdownRenderer::draw(n, textX, currentY, 0.5f, ScreenManager::colorText());
		currentY += n.height;

		const auto &v = UI::MarkdownRenderer::get(field.value, pixelWidth, 0.34f, 11.0f / 0.34f, EMBED_STYLES, false);
		UI::MarkdownRenderer::draw(v, textX, currentY, 0.5f, ScreenManager::colorTextMuted());
		currentY += v.height;
		currentY += 2.0f;
	}
	if (!embed.footer_text.empty()) {
		auto lines = MessageUtils::wrapText(embed.footer_text, pixelWidth, 0.30f, false);
		for (const auto &line : lines) {
			drawRichText(textX, currentY, 0.5f, 0.30f, 0.30f, ScreenManager::colorTextMuted(), line);
			currentY += 10.0f;
		}
	}

	if (showThumbnailOnRight) {
		float minH = 72.0f;
		if (currentY - y < minH) {
			currentY = y + minH;
		}
	} else if (!isSimpleMedia) {
		currentY += 5.0f;
	}

	if (showThumbnailOnRight) {
		std::string thumbUrl = !embed.thumbnail_proxy_url.empty() ? embed.thumbnail_proxy_url : embed.thumbnail_url;
		float thumbMaxSize = 64.0f;
		float thumbX = x + maxWidth - thumbMaxSize - 4.0f;
		float thumbY = y + 5.0f;
		auto thumbInfo = ImageManager::getInstance().getImageInfo(thumbUrl);
		if (thumbInfo.tex) {
			float scaleX = thumbMaxSize / thumbInfo.originalW;
			float scaleY = thumbMaxSize / thumbInfo.originalH;
			float scale = std::min(scaleX, scaleY);
			float uMax = (float)thumbInfo.originalW / thumbInfo.tex->width;
			float vMax = (float)thumbInfo.originalH / thumbInfo.tex->height;
			Tex3DS_SubTexture subtex = {
			    (u16)thumbInfo.originalW, (u16)thumbInfo.originalH, 0.0f, 1.0f, uMax, 1.0f - vMax};
			C2D_Image img = {thumbInfo.tex, &subtex};
			C2D_DrawImageAt(img, thumbX, thumbY, 0.49f, nullptr, scale, scale);
		} else if (thumbInfo.failed) {
			C2D_DrawRectSolid(thumbX, thumbY, 0.49f, thumbMaxSize, thumbMaxSize, C2D_Color32(60, 40, 40, 255));
		} else {
			ImageManager::getInstance().prefetch(thumbUrl, embed.thumbnail_width, embed.thumbnail_height,
			                                     Network::RequestPriority::INTERACTIVE);
			C2D_DrawRectSolid(thumbX, thumbY, 0.49f, thumbMaxSize, thumbMaxSize, ScreenManager::colorEmbedMedia());
			drawText(thumbX + 4.0f, thumbY + (thumbMaxSize / 2) - 5.0f, 0.5f, 0.28f, 0.28f,
			         ScreenManager::colorTextMuted(), TR("common.loading"));
		}
	}

	if (hasImage || (isMedia && hasThumbnail)) {
		std::string mediaUrl =
		    hasImage ? (!embed.image_proxy_url.empty() ? embed.image_proxy_url : embed.image_url)
		             : (!embed.thumbnail_proxy_url.empty() ? embed.thumbnail_proxy_url : embed.thumbnail_url);
		int imgW = hasImage ? embed.image_width : embed.thumbnail_width;
		int imgH = hasImage ? embed.image_height : embed.thumbnail_height;
		auto info = ImageManager::getInstance().getImageInfo(mediaUrl);
		if (info.tex) {
			imgW = info.originalW;
			imgH = info.originalH;
		}

		float availableMaxWidth = maxWidth - (isSimpleMedia ? 0.0f : 16.0f);
		availableMaxWidth = std::min(availableMaxWidth, 330.0f);
		float drawW = availableMaxWidth;

		float drawH = 100.0f;
		if (imgW > 0 && imgH > 0) {
			if (imgW < 160) {
				drawW = (float)imgW;
			}

			float aspect = (float)imgW / imgH;
			drawH = drawW / aspect;
			if (drawH > 220.0f) {
				drawH = 220.0f;
				drawW = drawH * aspect;
			}
		} else {
			drawH = drawW * 0.75f;
		}

		if (info.tex) {
			float uMax = (float)info.originalW / info.tex->width;
			float vMax = (float)info.originalH / info.tex->height;
			Tex3DS_SubTexture subtex = {(u16)info.originalW, (u16)info.originalH, 0.0f, 1.0f, uMax, 1.0f - vMax};
			C2D_Image img = {info.tex, &subtex};
			C2D_DrawImageAt(img, textX, currentY, 0.49f, nullptr, drawW / info.originalW, drawH / info.originalH);
		} else if (info.failed) {
			u32 errorBg = C2D_Color32(60, 40, 40, 255);
			C2D_DrawRectSolid(textX, currentY, 0.49f, drawW, drawH, errorBg);
			drawText(textX + 5, currentY + (drawH / 2) - 6, 0.5f, 0.35f, 0.35f, ScreenManager::colorError(),
			         TR("message.image_failed"));
		} else {
			ImageManager::getInstance().prefetch(mediaUrl, imgW, imgH, Network::RequestPriority::INTERACTIVE);
			C2D_DrawRectSolid(textX, currentY, 0.49f, drawW, drawH,
			                  isSimpleMedia ? ScreenManager::colorBackgroundDark() : ScreenManager::colorEmbedMedia());
			drawText(textX + 5, currentY + (drawH / 2) - 6, 0.5f, 0.35f, 0.35f, ScreenManager::colorTextMuted(),
			         TR("common.loading"));
		}
		currentY += drawH + 4.0f;
	}

	if (showThumbnailOnRight) {
		float minH = 72.0f;
		if (currentY - y < minH) {
			currentY = y + minH;
		}
	} else if (!isSimpleMedia) {
		currentY += 5.0f;
	}

	return currentY - y;
}
void MessageScreen::renderReactionIcon() {
	if (bottomMode == BottomScreenMode::EMOJI_PICKER) {
		return;
	}

	float btnW = 30.0f;
	float btnH = 30.0f;
	float btnX = 320.0f - btnW - 10.0f;
	float btnY = bottomButtonY();

	const float SCREEN_HEIGHT = 240.0f;
	float maxScroll = std::max(0.0f, totalContentHeight - SCREEN_HEIGHT);
	bool isScrollBtnVisible = (targetScrollY < maxScroll - 10.0f);

	float reactBtnX = isScrollBtnVisible ? (btnX - btnW - 8.0f) : btnX;

	drawRoundedRect(reactBtnX, btnY, 0.54f, btnW, btnH, 8.0f, ScreenManager::colorBackgroundLight());

	C3D_Tex *tex = UI::ImageManager::getInstance().getLocalImage("romfs:/discord-icons/reaction.png");
	if (tex) {
		float iconSize = 20.0f;
		Tex3DS_SubTexture subtex = {(u16)tex->width, (u16)tex->height, 0.0f, 1.0f, 1.0f, 0.0f};
		C2D_Image img = {tex, &subtex};
		C2D_ImageTint tint;
		C2D_PlainImageTint(&tint, ScreenManager::colorText(), 1.0f);
		C2D_DrawImageAt(img, reactBtnX + (btnW - iconSize) / 2.0f, btnY + (btnH - iconSize) / 2.0f, 0.55f, &tint,
		                iconSize / tex->width, iconSize / tex->height);
	}

	if (!isCallableChannel() || isCallActive()) {
		return;
	}

	float callBtnX = reactBtnX - btnW - 8.0f;

	drawRoundedRect(callBtnX, btnY, 0.54f, btnW, btnH, 8.0f, ScreenManager::colorBackgroundLight());

	C3D_Tex *callTex = UI::ImageManager::getInstance().getLocalImage("romfs:/discord-icons/phone-call.png");
	if (callTex) {
		float iconSize = 20.0f;
		Tex3DS_SubTexture subtex = {(u16)callTex->width, (u16)callTex->height, 0.0f, 1.0f, 1.0f, 0.0f};
		C2D_Image img = {callTex, &subtex};
		C2D_ImageTint tint;
		C2D_PlainImageTint(&tint, ScreenManager::colorText(), 1.0f);
		C2D_DrawImageAt(img, callBtnX + (btnW - iconSize) / 2.0f, btnY + (btnH - iconSize) / 2.0f, 0.55f, &tint,
		                iconSize / callTex->width, iconSize / callTex->height);
	}
}

std::vector<Discord::VoiceParticipant> MessageScreen::callParticipants() const {
	if (!isCallableChannel()) {
		return {};
	}
	return Discord::DiscordClient::getInstance().getVoiceParticipants(channelId);
}

void MessageScreen::renderCallParticipants(float y, const std::vector<Discord::VoiceParticipant> &participants) {
	drawText(10.0f, y, 0.5f, 0.45f, 0.45f, ScreenManager::colorSelection(), TR("call.in_call"));
	y += 17.0f;

	const float rowH = 26.0f;
	const float avatarSize = 20.0f;
	const int maxRows = 5;

	int drawn = 0;
	for (const auto &p : participants) {
		if (drawn >= maxRows) {
			break;
		}

		float rowY = y + drawn * rowH;
		float avatarY = rowY + (rowH - avatarSize) / 2.0f;

		if (Discord::VoiceClient::getInstance().isSpeaking(p.userId)) {
			drawCircle(14.0f + avatarSize / 2.0f, avatarY + avatarSize / 2.0f, 0.49f, avatarSize / 2.0f + 1.5f,
			           C2D_Color32(35, 165, 90, 255));
		}

		C3D_Tex *avatar = Discord::AvatarCache::getInstance().getAvatar(p.userId, p.avatar, "0");
		if (avatar) {
			Tex3DS_SubTexture sub = {(u16)avatar->width, (u16)avatar->height, 0.0f, 1.0f, 1.0f, 0.0f};
			C2D_Image img = {avatar, &sub};
			C2D_DrawImageAt(img, 14.0f, avatarY, 0.5f, nullptr, avatarSize / avatar->width,
			                avatarSize / avatar->height);
		}

		int stateIcons = (p.mute || p.selfMute ? 1 : 0) + (p.deaf || p.selfDeaf ? 1 : 0);
		float nameX = 14.0f + avatarSize + 6.0f;
		float nameLimit = 310.0f - nameX - stateIcons * 15.0f;
		drawRichText(nameX, rowY + 5.0f, 0.5f, 0.45f, 0.45f, ScreenManager::colorText(),
		             getTruncatedRichText(p.name, nameLimit, 0.45f, 0.45f));

		const char *micIcon = p.mute       ? "romfs:/discord-icons/mic-denied.png"
		                      : p.selfMute ? "romfs:/discord-icons/mic-muted.png"
		                                   : nullptr;
		const char *deafIcon = p.deaf       ? "romfs:/discord-icons/headphones-denied.png"
		                       : p.selfDeaf ? "romfs:/discord-icons/headphones-muted.png"
		                                    : nullptr;

		const float stateSize = 12.0f;
		float stateX = 310.0f - stateSize;
		const struct {
			const char *path;
			bool byServer;
		} icons[] = {{deafIcon, p.deaf}, {micIcon, p.mute}};

		for (const auto &entry : icons) {
			if (!entry.path) {
				continue;
			}
			C3D_Tex *tex = UI::ImageManager::getInstance().getLocalImage(entry.path);
			if (tex) {
				Tex3DS_SubTexture sub = {(u16)tex->width, (u16)tex->height, 0.0f, 1.0f, 1.0f, 0.0f};
				C2D_Image img = {tex, &sub};
				C2D_ImageTint tint;
				C2D_PlainImageTint(
				    &tint, entry.byServer ? ScreenManager::colorError() : ScreenManager::colorTextMuted(), 1.0f);
				C2D_DrawImageAt(img, stateX, rowY + (rowH - stateSize) / 2.0f, 0.5f, &tint, stateSize / tex->width,
				                stateSize / tex->height);
			}
			stateX -= stateSize + 3.0f;
		}

		drawn++;
	}

	if ((int)participants.size() > maxRows) {
		drawText(14.0f, y + maxRows * rowH, 0.5f, 0.4f, 0.4f, ScreenManager::colorTextMuted(),
		         "+" + std::to_string((int)participants.size() - maxRows));
	}
}

bool MessageScreen::isCallableChannel() const {
	if (channelType == 3) {
		return true;
	}
	if (channelType != 1) {
		return false;
	}

	Discord::Channel ch = Discord::DiscordClient::getInstance().getChannel(channelId);
	return ch.recipients.empty() || !ch.recipients[0].bot;
}

bool MessageScreen::isCallActive() const {
	Discord::VoiceClient &voice = Discord::VoiceClient::getInstance();
	return voice.getState() != Discord::VoiceState::DISCONNECTED && voice.getChannelId() == channelId;
}

void MessageScreen::startCall() {
	bool ongoing = !callParticipants().empty();
	Discord::VoiceClient::getInstance().connect("DM", channelId, !ongoing);
}

std::unordered_set<std::string> MessageScreen::getVisibleTwemojis() {
	std::unordered_set<std::string> visible;
	std::lock_guard<std::recursive_mutex> lock(messageMutex);

	const float SCREEN_HEIGHT = 240.0f;
	float yOffset = std::max(0.0f, SCREEN_HEIGHT - totalContentHeight);
	float yStart = -currentScrollY + yOffset;
	const float MARGIN = 10.0f;
	const float TOP_MARGIN = 30.0f;

	for (size_t i = 0; i < messages.size(); i++) {
		if (i >= messagePositions.size() || i >= messageHeights.size()) {
			break;
		}

		float msgY = yStart + messagePositions[i];
		float msgH = messageHeights[i];

		if (msgY + msgH < -TOP_MARGIN || msgY > SCREEN_HEIGHT + MARGIN) {
			continue;
		}

		const auto &msg = messages[i];

		for (size_t cursor = 0; cursor < msg.content.length();) {
			size_t nextC = cursor;
			uint32_t cp = Utils::Utf8::decodeNext(msg.content, nextC);
			if (Utils::Utf8::isEmoji(cp)) {
				visible.insert(Utils::Utf8::utf8ToHex(Utils::Utf8::getEmojiSequence(msg.content, cursor)));
			} else {
				cursor = nextC;
			}
		}

		for (const auto &r : msg.reactions) {
			if (r.emoji.id.empty()) {
				visible.insert(Utils::Utf8::utf8ToHex(r.emoji.name));
			}
		}
	}
	return visible;
}

void MessageScreen::catchUpMessages() {
	if (channelId.empty()) {
		return;
	}

	Discord::DiscordClient::getInstance().fetchMessagesAsync(
	    channelId, 50, [this, token = aliveToken](const std::vector<Discord::Message> &fetched) {
		    if (!*token || fetched.empty()) {
			    return;
		    }

		    std::lock_guard<std::recursive_mutex> lock(messageMutex);

		    std::string latestRealId;
		    for (auto it = this->messages.rbegin(); it != this->messages.rend(); ++it) {
			    if (it->id.substr(0, 8) != "pending_") {
				    latestRealId = it->id;
				    break;
			    }
		    }

		    if (latestRealId.empty()) {

			    this->messages = fetched;
			    std::reverse(this->messages.begin(), this->messages.end());
			    rebuildLayoutCache();
			    return;
		    }

		    int foundIndex = -1;
		    for (size_t i = 0; i < fetched.size(); i++) {
			    if (fetched[i].id == latestRealId) {
				    foundIndex = i;
				    break;
			    }
		    }

		    bool addedAny = false;
		    if (foundIndex != -1) {

			    for (int i = foundIndex - 1; i >= 0; i--) {
				    messages.push_back(fetched[i]);
				    addedAny = true;
			    }
		    } else {
			    for (int i = (int)fetched.size() - 1; i >= 0; i--) {
				    if (fetched[i].id > latestRealId) {
					    this->messages.push_back(fetched[i]);
					    addedAny = true;
				    }
			    }
		    }

		    if (addedAny) {
			    Logger::log("[UI] Merged %d new messages from catch-up", addedAny);
			    bool atBottom = isAtBottom();
			    rebuildLayoutCache();
			    syncScrollAfterRebuild(atBottom, true);
			    if (!atBottom) {
				    showNewMessageIndicator = true;
				    newMessageCount += addedAny;
			    }
		    }
	    });
}

} // namespace UI
