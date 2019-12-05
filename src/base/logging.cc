/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "perfetto/base/logging.h"

#include <stdarg.h>
#include <stdio.h>

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#include <unistd.h>  // For isatty()
#endif

#include "perfetto/base/time.h"

namespace perfetto {
namespace base {

namespace {
const char kReset[] = "\x1b[0m";
const char kDefault[] = "\x1b[39m";
const char kDim[] = "\x1b[2m";
const char kRed[] = "\x1b[31m";
const char kBoldGreen[] = "\x1b[1m\x1b[32m";
const char kLightGray[] = "\x1b[90m";

}  // namespace

void LogMessage(LogLev level,
                const char* fname,
                int line,
                const char* fmt,
                ...) {
  va_list args;
  va_start(args, fmt);
  char log_msg[1024];
  vsnprintf(log_msg, sizeof(log_msg), fmt, args);
  va_end(args);

  const char* color = kDefault;
  switch (level) {
    case kLogDebug:
      color = kDim;
      break;
    case kLogInfo:
      color = kDefault;
      break;
    case kLogImportant:
      color = kBoldGreen;
      break;
    case kLogError:
      color = kRed;
      break;
  }

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN) && \
    !PERFETTO_BUILDFLAG(PERFETTO_OS_WASM)
  static const bool use_colors = isatty(STDERR_FILENO);
#else
  static const bool use_colors = false;
#endif

  // Formats file.cc:line as a space-padded fixed width string. If the file name
  // |fname| is too long, truncate it on the left-hand side.
  char line_str[10];
  size_t line_len =
      static_cast<size_t>(snprintf(line_str, sizeof(line_str), "%d", line));

  // 24 will be the width of the file.cc:line column in the log event.
  char file_and_line[24];
  size_t fname_len = strlen(fname);
  size_t fname_max = sizeof(file_and_line) - line_len - 2;  // 2 = ':' + '\0'.
  size_t fname_offset = fname_len <= fname_max ? 0 : fname_len - fname_max;
  int len = snprintf(file_and_line, sizeof(file_and_line), "%s:%s",
                     fname + fname_offset, line_str);
  memset(&file_and_line[len], ' ', sizeof(file_and_line) - size_t(len));
  file_and_line[sizeof(file_and_line) - 1] = '\0';

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
  // Logcat has already timestamping, don't re-emit it.
  __android_log_print(ANDROID_LOG_DEBUG + level, "perfetto", "%s %s",
                      file_and_line, log_msg);
#endif

  // When printing on stderr, print also the timestamp. We don't really care
  // about the actual time. We just need some reference clock that can be used
  // to correlated events across differrent processses (e.g. traced and
  // traced_probes). The wall time % 1000 is good enough.
  char timestamp[32];
  int t_ms = static_cast<int>(GetWallTimeMs().count());
  int t_sec = t_ms / 1000;
  t_ms -= t_sec * 1000;
  t_sec = t_sec % 1000;
  snprintf(timestamp, sizeof(timestamp), "[%03d.%03d] ", t_sec, t_ms);

  if (use_colors) {
    fprintf(stderr, "%s%s%s%s %s%s%s\n", kLightGray, timestamp, file_and_line,
            kReset, color, log_msg, kReset);
  } else {
    fprintf(stderr, "%s%s %s\n", timestamp, file_and_line, log_msg);
  }
}

}  // namespace base
}  // namespace perfetto
