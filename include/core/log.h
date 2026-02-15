#ifndef LOG_H
#define LOG_H

#include <string>
#include <vector>

namespace Logger {
void init();
void log(const char *fmt, ...);
std::vector<std::string> getRecentLogs();
void setFileLoggingEnabled(bool enabled);
bool isFileLoggingEnabled();
} // namespace Logger

#endif // LOG_H
