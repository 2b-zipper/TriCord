#include "utils/message_utils.h"
#include "core/config.h"
#include "core/i18n.h"
#include "ui/screen_manager.h"
#include "utils/string_utils.h"
#include "utils/utf8_utils.h"
#include <3ds.h>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <sys/time.h>

namespace UI {
namespace MessageUtils {

static int64_t cloudTimeOffset = 0;

extern "C" int _gettimeofday_r(struct _reent *ptr, struct timeval *tp, void *tzp) {
	if (tp != NULL) {
		u64 timecode = osGetTime() - 2208988800000ULL;
		tp->tv_sec = (timecode / 1000) + cloudTimeOffset;
		tp->tv_usec = (timecode % 1000) * 1000;
	}
	return 0;
}

void syncClock(const std::string &dateStr) {
	char month_str[4];
	struct tm tm = {0};
	int day, year, hour, min, sec;

	if (sscanf(dateStr.c_str(), "%*s %d %3s %d %d:%d:%d GMT", &day, month_str, &year, &hour, &min, &sec) == 6) {
		tm.tm_mday = day;
		tm.tm_year = year - 1900;
		tm.tm_hour = hour;
		tm.tm_min = min;
		tm.tm_sec = sec;

		const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
		tm.tm_mon = -1;
		for (int i = 0; i < 12; i++) {
			if (strcmp(month_str, months[i]) == 0) {
				tm.tm_mon = i;
				break;
			}
		}

		if (tm.tm_mon != -1) {
			time_t serverTime = mktime(&tm);
			struct tm dummy = {0};
			dummy.tm_year = 70;
			dummy.tm_mday = 1;
			time_t local_offset = mktime(&dummy);

			serverTime -= local_offset;
			u64 raw_timecode = osGetTime() - 2208988800000ULL;
			cloudTimeOffset = serverTime - (raw_timecode / 1000);
		}
	}
}

time_t getUtcNow() { return time(NULL); }

s64 get3DSLocalTimeOffset() { return (s64)Config::getInstance().getTimezoneOffset() * 3600LL; }

bool getLocalTm(const std::string &timestamp, struct tm &out) {
	time_t utc = parseISO8601(timestamp);
	if (utc == 0) {
		return false;
	}
	time_t local = utc + (time_t)get3DSLocalTimeOffset();
	gmtime_r(&local, &out);
	return true;
}

time_t parseISO8601(const std::string &timestamp) {
	int year, month, day, hour, min, sec;
	if (sscanf(timestamp.c_str(), "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &min, &sec) != 6) {
		return 0;
	}

	static const int days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	time_t epoch = 0;
	for (int y = 1970; y < year; ++y) {
		epoch += (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
	}
	for (int m = 0; m < month - 1; ++m) {
		epoch += days_in_month[m];
		if (m == 1 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) {
			epoch += 1;
		}
	}
	epoch += day - 1;
	epoch = epoch * 86400 + hour * 3600 + min * 60 + sec;

	return epoch;
}

time_t snowflakeToTimestamp(const std::string &snowflake) {
	if (snowflake.empty()) {
		return 0;
	}

	char *end;
	unsigned long long id = strtoull(snowflake.c_str(), &end, 10);
	if (*end != '\0') {
		return 0;
	}

	return (time_t)(((id >> 22) + 1420070400000ULL) / 1000);
}

bool isNewerSnowflake(const std::string &a, const std::string &b) {
	if (b.empty()) {
		return !a.empty();
	}
	if (a.empty()) {
		return false;
	}
	return (a.length() > b.length()) || (a.length() == b.length() && a > b);
}

static bool isUtf8Continuation(unsigned char c) { return (c & 0xC0) == 0x80; }

static bool translatePendingStatus(const std::string &timestamp, std::string &out) {
	if (timestamp == "Sending...") {
		out = TR("message.status.sending");
		return true;
	}
	if (timestamp == "Failed") {
		out = TR("message.status.failed");
		return true;
	}
	return false;
}

std::string formatTimestamp(const std::string &timestamp) {
	std::string pending;
	if (translatePendingStatus(timestamp, pending)) {
		return pending;
	}
	time_t msg_utc = parseISO8601(timestamp);
	if (msg_utc == 0) {
		return timestamp;
	}
	s64 offset = get3DSLocalTimeOffset();
	time_t msg_local = msg_utc + (time_t)offset;
	struct tm msg_tm;
	gmtime_r(&msg_local, &msg_tm);

	time_t now_local = getUtcNow() + (time_t)offset;
	struct tm now_tm;
	gmtime_r(&now_local, &now_tm);

	struct tm today_start_tm = now_tm;
	today_start_tm.tm_hour = today_start_tm.tm_min = today_start_tm.tm_sec = 0;
	time_t today_start_local = mktime(&today_start_tm);

	char buffer[64];
	if (msg_tm.tm_year == now_tm.tm_year && msg_tm.tm_mon == now_tm.tm_mon && msg_tm.tm_mday == now_tm.tm_mday) {
		snprintf(buffer, sizeof(buffer), "%02d:%02d", msg_tm.tm_hour, msg_tm.tm_min);
	} else if (msg_local >= (today_start_local - 86400) && msg_local < today_start_local) {
		std::string timeStr = (msg_tm.tm_hour < 10 ? "0" : "") + std::to_string(msg_tm.tm_hour) + ":" +
		                      (msg_tm.tm_min < 10 ? "0" : "") + std::to_string(msg_tm.tm_min);
		std::string yesterday_at = TR("time.yesterday_at");
		size_t pos = yesterday_at.find("{0}");
		if (pos != std::string::npos) {
			yesterday_at.replace(pos, 3, timeStr);
		}
		return yesterday_at;
	} else {
		snprintf(buffer, sizeof(buffer), "%04d/%02d/%02d %02d:%02d", msg_tm.tm_year + 1900, msg_tm.tm_mon + 1,
		         msg_tm.tm_mday, msg_tm.tm_hour, msg_tm.tm_min);
	}
	return std::string(buffer);
}

std::string formatTimeOnly(const std::string &timestamp) {
	std::string pending;
	if (translatePendingStatus(timestamp, pending)) {
		return "";
	}
	struct tm msg_tm;
	if (!getLocalTm(timestamp, msg_tm)) {
		return timestamp.substr(11, 5);
	}
	char buffer[16];
	snprintf(buffer, sizeof(buffer), "%02d:%02d", msg_tm.tm_hour, msg_tm.tm_min);
	return std::string(buffer);
}

std::vector<std::string> wrapText(const std::string &text, float maxWidth, float scale, bool unicodeOnly) {
	std::vector<std::string> lines;

	if (text.empty()) {
		lines.push_back("");
		return lines;
	}

	size_t start = 0;
	while (start < text.length()) {
		size_t newlinePos = text.find('\n', start);
		size_t end = (newlinePos == std::string::npos) ? text.length() : newlinePos;

		size_t segmentLen = end - start;
		if (segmentLen == 0) {
			lines.push_back("");
		} else {
			size_t innerPos = 0;
			while (innerPos < segmentLen) {
				size_t remaining = segmentLen - innerPos;

				size_t guessLen = std::min(remaining, (size_t)30);

				while (guessLen < remaining && isUtf8Continuation(text[start + innerPos + guessLen])) {
					guessLen++;
				}

				std::string chunk = text.substr(start + innerPos, guessLen);
				float currentW = UI::measureText(chunk, scale, scale);

				if (currentW > maxWidth) {
					while (guessLen > 1 && currentW > maxWidth) {
						guessLen--;
						while (guessLen > 0 && isUtf8Continuation(text[start + innerPos + guessLen])) {
							guessLen--;
						}
						if (guessLen == 0) {
							break;
						}

						chunk = text.substr(start + innerPos, guessLen);
						currentW = UI::measureText(chunk, scale, scale);
					}
				} else {
					while (guessLen < remaining) {
						int charsAdded = 0;
						size_t bytesAdded = 0;
						while (guessLen + bytesAdded < remaining && charsAdded < 3) {
							bytesAdded++;
							while (guessLen + bytesAdded < remaining &&
							       isUtf8Continuation(text[start + innerPos + guessLen + bytesAdded])) {
								bytesAdded++;
							}
							charsAdded++;
						}
						size_t nextStep = bytesAdded;

						std::string nextChunk = text.substr(start + innerPos + guessLen, nextStep);
						float nextW = UI::measureText(nextChunk, scale, scale);

						if (currentW + nextW > maxWidth) {
							size_t fineStep = 0;
							size_t tempPos = 0;
							while (tempPos < nextStep) {
								size_t charLen = 1;
								while (tempPos + charLen < nextStep &&
								       isUtf8Continuation(text[start + innerPos + guessLen + tempPos + charLen])) {
									charLen++;
								}

								std::string ch = text.substr(start + innerPos + guessLen + tempPos, charLen);
								float chW = UI::measureText(ch, scale, scale);
								if (currentW + chW > maxWidth) {
									break;
								}

								currentW += chW;
								fineStep += charLen;
								tempPos += charLen;
							}
							guessLen += fineStep;
							break;
						}

						currentW += nextW;
						guessLen += nextStep;
					}
				}

				if (guessLen < remaining) {
					size_t spacePos = text.rfind(' ', start + innerPos + guessLen);
					if (spacePos != std::string::npos && spacePos > start + innerPos) {
						if (start + innerPos + guessLen - spacePos < 20) {
							guessLen = spacePos - (start + innerPos);
						}
					}
				}

				if (guessLen == 0) {
					size_t charLen = 1;
					while (innerPos + charLen < remaining && isUtf8Continuation(text[start + innerPos + charLen])) {
						charLen++;
					}
					guessLen = charLen;
				}

				lines.push_back(text.substr(start + innerPos, guessLen));
				innerPos += guessLen;

				if (start + innerPos < end && text[start + innerPos] == ' ') {
					innerPos++;
				}
			}
		}

		if (newlinePos == std::string::npos) {
			break;
		}
		start = newlinePos + 1;
		if (start == text.length()) {
			lines.push_back("");
			break;
		}
	}

	if (lines.empty()) {
		lines.push_back("");
	}
	return lines;
}

bool isEmojiOnly(const std::string &text, int &count) {
	if (text.empty()) {
		return false;
	}

	count = 0;
	size_t cursor = 0;

	while (cursor < text.length()) {
		unsigned char c = static_cast<unsigned char>(text[cursor]);
		if (c <= 0x20) {
			cursor++;
			continue;
		}

		if (text[cursor] == '<') {
			size_t start = cursor;
			if (start + 6 < text.length()) {
				bool isAnimated = (text[start + 1] == 'a');
				if (text[start + 1] == ':' || isAnimated) {
					size_t secondColon = text.find(':', start + (isAnimated ? 3 : 2));
					if (secondColon != std::string::npos) {
						size_t closeBracket = text.find('>', secondColon);
						if (closeBracket != std::string::npos) {
							count++;
							cursor = closeBracket + 1;
							continue;
						}
					}
				}
			}
		}

		size_t tempCursor = cursor;
		uint32_t codepoint = Utils::Utf8::decodeNext(text, tempCursor);

		if (Utils::Utf8::isEmoji(codepoint)) {
			count++;
			cursor = tempCursor;
			continue;
		}

		return false;
	}

	return count > 0;
}

bool canGroupWithPrevious(const Discord::Message &current, const Discord::Message &previous) {
	bool isCurrentSystem = (current.type != 0 && current.type != 19);
	bool isPreviousSystem = (previous.type != 0 && previous.type != 19);

	if (isCurrentSystem != isPreviousSystem) {
		return false;
	}

	if (current.author.id != previous.author.id) {
		return false;
	}

	if (!current.referencedMessageId.empty()) {
		return false;
	}

	time_t t1 = parseISO8601(current.timestamp);
	time_t t2 = parseISO8601(previous.timestamp);
	if (t1 == 0 || t2 == 0) {
		return false;
	}

	double diff = difftime(t1, t2);
	if (std::abs(diff) > 300) {
		return false;
	}

	s64 offset_s = (s64)Config::getInstance().getTimezoneOffset() * 3600LL;
	time_t local_t1 = t1 + offset_s;
	time_t local_t2 = t2 + offset_s;

	struct tm *lt1 = gmtime(&local_t1);
	if (!lt1) {
		return false;
	}
	int day1 = lt1->tm_yday;
	int year1 = lt1->tm_year;

	struct tm *lt2 = gmtime(&local_t2);
	if (!lt2) {
		return false;
	}
	int day2 = lt2->tm_yday;
	int year2 = lt2->tm_year;

	if (day1 != day2 || year1 != year2) {
		return false;
	}

	return true;
}

std::string getRelativeTime(time_t targetEpoch) {
	time_t now = time(NULL);
	double diff = difftime(now, targetEpoch);

	if (diff < 3600) {
		int mins = (int)(diff / 60);
		if (mins < 1) {
			mins = 1;
		}
		return std::to_string(mins) + TR("time.minutes_ago");
	}
	if (diff < 86400) {
		return std::to_string((int)(diff / 3600)) + TR("time.hours_ago");
	}
	if (diff <= 30 * 86400) {
		return std::to_string((int)(diff / 86400)) + TR("time.days_ago");
	}
	if (diff < 365 * 86400) {
		return TR("time.more_than_30d");
	}

	time_t local = targetEpoch + (time_t)Config::getInstance().getTimezoneOffset() * 3600;
	struct tm *lt = gmtime(&local);
	if (!lt) {
		return TR("time.more_than_30d");
	}

	char buffer[64];
	std::string fmt = TR("time.format_long");
	strftime(buffer, sizeof(buffer), fmt.c_str(), lt);

	std::string result(buffer);
	return Utils::String::trim(result);
}

std::string getLocalDateString(const std::string &timestamp) {
	struct tm lt;
	if (!getLocalTm(timestamp, lt)) {
		return timestamp.substr(0, 10);
	}
	char buffer[32];
	snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
	return std::string(buffer);
}

std::string getISOTimestamp(time_t epoch) {
	struct tm *gt = gmtime(&epoch);
	if (!gt) {
		return "1970-01-01T00:00:00";
	}
	char buffer[48];
	snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d", gt->tm_year + 1900, gt->tm_mon + 1, gt->tm_mday,
	         gt->tm_hour, gt->tm_min, gt->tm_sec);
	return std::string(buffer);
}

std::string getChannelDisplayName(const Discord::Channel &channel) {
	if (!channel.name.empty() && channel.name != "Channel") {
		return channel.name;
	}

	if ((channel.type == 1 || channel.type == 3) && !channel.recipients.empty()) {
		if (channel.type == 1) {
			return channel.recipients[0].global_name.empty() ? channel.recipients[0].username
			                                                 : channel.recipients[0].global_name;
		} else {
			std::string result = "";
			for (size_t i = 0; i < channel.recipients.size(); ++i) {
				result += channel.recipients[i].global_name.empty() ? channel.recipients[i].username
				                                                    : channel.recipients[i].global_name;
				if (i < channel.recipients.size() - 1) {
					result += ", ";
				}
			}
			return result;
		}
	}

	return channel.name.empty() ? "Channel" : channel.name;
}

} // namespace MessageUtils
} // namespace UI
