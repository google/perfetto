/*
 * Copyright (C) 2024 The Android Open Source Project
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

#ifndef SRC_TRACE_REDACTION_FIND_PACKAGE_UID_H_
#define SRC_TRACE_REDACTION_FIND_PACKAGE_UID_H_

#include "src/trace_redaction/trace_redaction_framework.h"

#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::trace_redaction {

// Resolves and sets `Context.package_uid` for the package matching
// `Context.package_name`.
//
// When an explicit target UID is provided via CLI (`--uid`), FindPackageUid
// preserves the full per-user UID, disables `Context.normalize_uid` (sets it
// to false) to isolate processes across Android users, and skips
// `PackagesList` lookup in `Collect()`.
//
// When no explicit target UID is provided, this primitive extracts the
// package's normalized UID from the trace's `PackagesList` into
// `Context.package_uid`, verifies that it is normalized (uid < 100,000), and
// sets `Context.normalize_uid = true`.
//
// If `Context.package_name` is empty or if no matching package UID can be
// resolved by the end of the trace, this primitive returns an error status.
class FindPackageUid final : public CollectPrimitive {
 public:
  base::Status Begin(Context*) const override;

  base::Status Collect(const protos::pbzero::TracePacket::Decoder& packet,
                       Context* context) const override;

  base::Status End(Context*) const override;
};

}  // namespace perfetto::trace_redaction

#endif  // SRC_TRACE_REDACTION_FIND_PACKAGE_UID_H_
