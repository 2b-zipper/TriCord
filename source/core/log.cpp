#include "log.h"
#include <3ds.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

#include <deque>
#include <mutex>
#include <string>
#include <vector>

static const char *LOG_DIR = "sdmc:/3ds/TriCord";
static const char *LOG_FILE = "sdmc:/3ds/TriCord/tricord.log";
static std::deque<std::string> logBuffer;
static const size_t MAX_LOG_LINES = 22;
static std::mutex logMutex;
static bool fileLoggingEnabled = false;

namespace Logger {
void init() { mkdir(LOG_DIR, 0700); }

void log(const char *fmt, ...) {
  char buffer[1024];
  va_list args;

  va_start(args, fmt);
  vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);

  std::lock_guard<std::mutex> lock(logMutex);
  std::string logLine(buffer);

  logBuffer.push_back(logLine);
  if (logBuffer.size() > MAX_LOG_LINES) {
    logBuffer.pop_front();
  }

  printf("%s\n", buffer);
  svcOutputDebugString(buffer, strlen(buffer));

  if (fileLoggingEnabled) {
    FILE *f = fopen(LOG_FILE, "a");
    if (f) {
      fprintf(f, "%s\n", buffer);
      fclose(f);
    }
  }
}

std::vector<std::string> getRecentLogs() {
  std::lock_guard<std::mutex> lock(logMutex);
  std::vector<std::string> logs;
  for (const auto &line : logBuffer) {
    logs.push_back(line);
  }
  return logs;
}

void setFileLoggingEnabled(bool enabled) {
  std::lock_guard<std::mutex> lock(logMutex);
  fileLoggingEnabled = enabled;

  if (enabled) {
    FILE *f = fopen(LOG_FILE, "w");
    if (f) {
      fprintf(f, "=== TriCord Log Started ===\n");
      fclose(f);
    }
  }
}

bool isFileLoggingEnabled() {
  std::lock_guard<std::mutex> lock(logMutex);
  return fileLoggingEnabled;
}
} // namespace Logger
