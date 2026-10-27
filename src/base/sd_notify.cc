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

#include "perfetto/ext/base/sd_notify.h"

#include <stdlib.h>
#include <string.h>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/unix_socket.h"

namespace perfetto {
namespace base {

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)

namespace {
// Do not hold up startup if systemd's notify socket queue is full.
constexpr uint32_t kSendTimeoutMs = 1000;
}  // namespace

bool SdNotify(const char* state) {
  const char* addr = getenv("NOTIFY_SOCKET");
  if (!addr || !*addr)
    return false;

  // As in sd_notify(3), only filesystem ("/...") and abstract ("@...") sockets
  // are supported. UnixSocketRaw::Connect() maps the leading '@' to an abstract
  // address.
  if (addr[0] != '/' && addr[0] != '@') {
    PERFETTO_ELOG("Unsupported NOTIFY_SOCKET value: %s", addr);
    return false;
  }

  auto sock = UnixSocketRaw::CreateMayFail(SockFamily::kUnix, SockType::kDgram);
  if (!sock) {
    PERFETTO_PLOG("Failed to create the sd_notify socket");
    return false;
  }
  sock.SetTxTimeout(kSendTimeoutMs);
  if (!sock.Connect(addr)) {
    PERFETTO_PLOG("Failed to connect to NOTIFY_SOCKET %s", addr);
    return false;
  }
  const size_t len = strlen(state);
  if (sock.Send(state, len) != static_cast<ssize_t>(len)) {
    PERFETTO_PLOG("Failed to send \"%s\" to NOTIFY_SOCKET %s", state, addr);
    return false;
  }
  return true;
}

#else  // Not Linux or Android.

bool SdNotify(const char*) {
  return false;
}

#endif

}  // namespace base
}  // namespace perfetto
