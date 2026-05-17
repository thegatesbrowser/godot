/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// POSIX counterpart of the_gates' chromium-shim/base/logging.cpp.
// Compiled only on Linux/macOS via modules/the_gates/sandbox/linux/SCsub.

#include "base/logging.h"

#include <errno.h>
#include <string.h>

#include <algorithm>

#include "base/strings/stringprintf.h"
#include "mozilla/Assertions.h"

namespace logging {

namespace {

int g_min_log_level = 0;
LoggingDestination g_logging_destination = LOG_DEFAULT;
const int kAlwaysPrintErrorLevel = LOGGING_ERROR;
LogMessageHandlerFunction log_message_handler = nullptr;

}  // namespace

std::ostream* g_swallow_stream;

void SetMinLogLevel(int level) {
  g_min_log_level = std::min(LOGGING_FATAL, level);
}

int GetMinLogLevel() {
  return g_min_log_level;
}

bool ShouldCreateLogMessage(int severity) {
  if (severity < g_min_log_level)
    return false;
  return g_logging_destination != LOG_NONE || log_message_handler ||
         severity >= kAlwaysPrintErrorLevel;
}

int GetVlogLevelHelper(const char* file, size_t N) {
  return 0;
}

LogMessage::LogMessage(const char* file, int line, LogSeverity severity)
    : severity_(severity), file_(file), line_(line) {}

LogMessage::~LogMessage() {
  if (severity_ == LOGGING_FATAL) {
    MOZ_CRASH("Hit fatal chromium sandbox condition.");
  }
}

LogMessageFatal::~LogMessageFatal() {
  MOZ_CRASH("Hit fatal chromium sandbox condition (LogMessageFatal).");
}

SystemErrorCode GetLastSystemErrorCode() {
  return errno;
}

std::string SystemErrorCodeToString(SystemErrorCode error_code) {
  char buf[128] = { 0 };
  // strerror_r is XSI on most distros and GNU on glibc; cover both.
#if defined(__GLIBC__) && (!defined(_POSIX_C_SOURCE) || _POSIX_C_SOURCE >= 200112L) && \
        !defined(_GNU_SOURCE)
  if (strerror_r(error_code, buf, sizeof(buf)) != 0) {
    return base::StringPrintf("errno=%d", error_code);
  }
  return std::string(buf);
#else
  return std::string(strerror_r(error_code, buf, sizeof(buf)));
#endif
}

ErrnoLogMessage::ErrnoLogMessage(const char* file, int line,
                                 LogSeverity severity, SystemErrorCode err)
    : LogMessage(file, line, severity), err_(err) {
  (void)err_;
}

ErrnoLogMessage::~ErrnoLogMessage() {}

ErrnoLogMessageFatal::~ErrnoLogMessageFatal() {
  MOZ_CRASH("Hit fatal errno error in chromium sandbox.");
}

void RawLog(int level, const char* message) {}

std::string LogMessage::BuildCrashString() const {
  return std::string();
}

}  // namespace logging
