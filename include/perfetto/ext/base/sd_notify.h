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

#ifndef INCLUDE_PERFETTO_EXT_BASE_SD_NOTIFY_H_
#define INCLUDE_PERFETTO_EXT_BASE_SD_NOTIFY_H_

namespace perfetto {
namespace base {

// Minimal implementation of sd_notify(3) that does not depend on libsystemd.
//
// Sends |state| (e.g. "READY=1") as a single datagram to the socket named by
// the NOTIFY_SOCKET environment variable, which systemd sets for services
// declared with Type=notify (or with NotifyAccess=).
//
// Returns true if the datagram was sent. Returns false, without logging, if
// NOTIFY_SOCKET is unset or empty (i.e. we were not started by systemd).
// Returns false and logs an error if NOTIFY_SOCKET is set but the message could
// not be delivered. Always returns false on platforms other than Linux and
// Android.
bool SdNotify(const char* state);

}  // namespace base
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_EXT_BASE_SD_NOTIFY_H_
