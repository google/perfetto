/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "perfetto/ext/base/progress_reporter.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "perfetto/base/build_config.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/no_destructor.h"
#include "perfetto/ext/base/utils.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#include <windows.h>
#elif !PERFETTO_BUILDFLAG(PERFETTO_OS_WASM)
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace perfetto::base {
namespace {
std::mutex& Mutex() {
  static NoDestructor<std::mutex> mutex;
  return mutex.ref();
}
ProgressReporter* g_visible_reporter = nullptr;

size_t TerminalWidth() {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  CONSOLE_SCREEN_BUFFER_INFO info{};
  if (GetConsoleScreenBufferInfo(GetStdHandle(STD_ERROR_HANDLE), &info))
    return static_cast<size_t>(info.srWindow.Right - info.srWindow.Left + 1);
#elif !PERFETTO_BUILDFLAG(PERFETTO_OS_WASM)
  struct winsize size{};
  if (ioctl(STDERR_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col)
    return size.ws_col;
#endif
  return 80;
}
}  // namespace

bool StderrSupportsProgress() {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WASM)
  return false;
#else
  const char* term = getenv("TERM");
  return IsTty(stderr) && (!term || strcmp(term, "dumb") != 0);
#endif
}

bool StderrSupportsColor() {
  const char* force_color = getenv("FORCE_COLOR");
  if (force_color && *force_color)
    return true;
  const char* no_color = getenv("NO_COLOR");
  if (no_color && *no_color)
    return false;
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  // Do not emit ANSI colors without first enabling virtual terminal output.
  return false;
#elif PERFETTO_BUILDFLAG(PERFETTO_CHROMIUM_BUILD)
  return false;
#else
  return StderrSupportsProgress();
#endif
}

ProgressReporter::ProgressReporter(bool enabled) : enabled_(enabled) {}

ProgressReporter::~ProgressReporter() {
  Clear();
}

void ProgressReporter::Update(const std::string& message) {
  if (!enabled_ || !StderrSupportsProgress())
    return;
  std::lock_guard<std::mutex> lock(Mutex());
  int64_t now = GetWallTimeMs().count();
  if (last_update_ms_ && now - last_update_ms_ < 100)
    return;
  last_update_ms_ = now;
  if (g_visible_reporter)
    g_visible_reporter->ClearLocked();
  // Leave one column unused to avoid wrapping, including on narrow terminals.
  size_t width = TerminalWidth();
  visible_width_ = std::min(message.size(), width > 1 ? width - 1 : 0);
  if (!visible_width_)
    return;
  fprintf(stderr, "\r%.*s", static_cast<int>(visible_width_), message.c_str());
  fflush(stderr);
  g_visible_reporter = this;
}

void ProgressReporter::ClearLocked() {
  if (!visible_width_)
    return;
  // Bound clearing by the current width in case the terminal was resized.
  size_t width = TerminalWidth();
  size_t clear = std::min(visible_width_, width > 1 ? width - 1 : 0);
  fprintf(stderr, "\r%*s\r", static_cast<int>(clear), "");
  fflush(stderr);
  visible_width_ = 0;
  g_visible_reporter = nullptr;
}

void ProgressReporter::Clear() {
  std::lock_guard<std::mutex> lock(Mutex());
  ClearLocked();
}

void ProgressReporter::ClearBeforeLog() {
  std::lock_guard<std::mutex> lock(Mutex());
  if (g_visible_reporter)
    g_visible_reporter->ClearLocked();
}
}  // namespace perfetto::base
