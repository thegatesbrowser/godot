/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// the_gates: forward-port of Firefox's chromium-shim/base/logging.cpp to
// chromium 137 (mid-2025) API. Differences from the Mozilla original:
//   - LOG_FATAL/LOG_ERROR renamed to LOGGING_FATAL/LOGGING_ERROR (~M122)
//   - The (file, line, const char* condition) ctor was dropped from
//     LogMessage; LogMessageFatal is a separate class now.
//   - GetDisableAllVLogLevel removed under USE_RUNTIME_VLOG=1.
// We keep the spirit: a stripped-down logging.cc that prevents chromium
// logging from dragging in the rest of base.

#include "base/logging.h"

#include <windows.h>

#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"

#include <algorithm>

#include "mozilla/Assertions.h"

namespace logging {

namespace {

int g_min_log_level = 0;

LoggingDestination g_logging_destination = LOG_DEFAULT;

// For LOGGING_ERROR and above, always print to stderr.
const int kAlwaysPrintErrorLevel = LOGGING_ERROR;

// A log message handler that gets notified of every log message we process.
LogMessageHandlerFunction log_message_handler = nullptr;

}  // namespace

// Used by EAT_STREAM_PARAMETERS to give the unused branch of the ternary
// an ostream of the right type.
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

  // Return true here unless ~LogMessage won't do anything. Note that
  // ~LogMessage writes to stderr if severity_ >= kAlwaysPrintErrorLevel,
  // even when g_logging_destination is LOG_NONE.
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

// Chromium 137 split LogMessage::LogMessage(file, line, condition) into a
// separate LogMessageFatal final class. Provide stubs.
LogMessageFatal::~LogMessageFatal() {
  MOZ_CRASH("Hit fatal chromium sandbox condition (LogMessageFatal).");
}

SystemErrorCode GetLastSystemErrorCode() {
  return ::GetLastError();
}

Win32ErrorLogMessage::Win32ErrorLogMessage(const char* file, int line,
                                           LogSeverity severity,
                                           SystemErrorCode err)
    : LogMessage(file, line, severity), err_(err) {
  (void)err_;
}

Win32ErrorLogMessage::~Win32ErrorLogMessage() {}

Win32ErrorLogMessageFatal::~Win32ErrorLogMessageFatal() {
  MOZ_CRASH("Hit fatal Win32 error in chromium sandbox.");
}

void RawLog(int level, const char* message) {}

std::string LogMessage::BuildCrashString() const {
  return std::string();
}

}  // namespace logging
