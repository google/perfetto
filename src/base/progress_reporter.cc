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
#include <string_view>

#include "perfetto/base/build_config.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/utils.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#include <windows.h>
#elif !PERFETTO_BUILDFLAG(PERFETTO_OS_WASM)
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace perfetto::base {
namespace {

constexpr int64_t kMinUpdateIntervalMs = 100;

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WASM)
bool StderrIsInteractiveTerminal() {
#if PERFETTO_BUILDFLAG(PERFETTO_CHROMIUM_BUILD)
  // isatty() is blocked by the Chrome seccomp-bpf sandbox (b/191235714).
  return false;
#else
  const char* term = getenv("TERM");
  return IsTty(stderr) && (!term || strcmp(term, "dumb") != 0);
#endif
}
#endif

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
// Fails when stderr is not a console or the console predates ANSI support.
bool EnableVirtualTerminalProcessing() {
  HANDLE handle = GetStdHandle(STD_ERROR_HANDLE);
  DWORD mode = 0;
  if (handle == INVALID_HANDLE_VALUE || !GetConsoleMode(handle, &mode))
    return false;
  return SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
}
#endif

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
  return true;
#else
  return StderrIsInteractiveTerminal();
#endif
}

bool StderrSupportsColor() {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  // Attempt this even when colors are forced, so that escapes are interpreted
  // wherever the console can.
  static const bool vt_enabled = EnableVirtualTerminalProcessing();
#endif
  const char* force_color = getenv("FORCE_COLOR");
  if (force_color && *force_color)
    return true;
  const char* no_color = getenv("NO_COLOR");
  if (no_color && *no_color)
    return false;
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WASM)
  return false;
#elif PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  return vt_enabled && StderrIsInteractiveTerminal();
#else
  return StderrIsInteractiveTerminal();
#endif
}

ProgressReporter& ProgressReporter::GetInstance() {
  static ProgressReporter* instance = new ProgressReporter();
  return *instance;
}

void ProgressReporter::set_enabled(bool enabled) {
  std::lock_guard<std::mutex> lock(mutex_);
  enabled_ = enabled;
}

void ProgressReporter::Update(std::string_view message) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!enabled_ || !StderrSupportsProgress())
    return;
  int64_t now = GetWallTimeMs().count();
  if (last_update_ms_ && now - last_update_ms_ < kMinUpdateIntervalMs)
    return;
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WASM)
  last_update_ms_ = now;
  // stdio reaches the embedder only when a write ends with a newline.
  fprintf(stderr, "%.*s\n", static_cast<int>(message.size()), message.data());
#else
  // Leave one column unused to avoid wrapping, including on narrow terminals.
  size_t width = TerminalWidth();
  size_t new_width = std::min(message.size(), width > 1 ? width - 1 : 0);
  if (new_width < visible_width_)
    ClearLocked();
  // Set after clearing, which resets the throttle for the next phase.
  last_update_ms_ = now;
  if (!new_width)
    return;
  fprintf(stderr, "\r%.*s", static_cast<int>(new_width), message.data());
  fflush(stderr);
  visible_width_ = new_width;
#endif
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
  // The next phase's first update should not be throttled by this one.
  last_update_ms_ = 0;
}

void ProgressReporter::Clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  ClearLocked();
}

}  // namespace perfetto::base
