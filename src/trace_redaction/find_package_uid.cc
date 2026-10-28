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

#include "src/trace_redaction/find_package_uid.h"
#include "perfetto/ext/base/string_view.h"
#include "src/trace_redaction/trace_redaction_framework.h"

#include "protos/perfetto/trace/android/packages_list.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::trace_redaction {

base::Status FindPackageUid::Begin(Context* context) const {
  if (context->package_name.empty()) {
    return base::ErrStatus("FindPackageUid: missing package name.");
  }

  if (context->package_uid.has_value()) {
    context->normalize_uid = false;
    return base::OkStatus();
  }

  context->normalize_uid = true;
  return base::OkStatus();
}

base::Status FindPackageUid::Collect(
    const protos::pbzero::TracePacket::Decoder& packet,
    Context* context) const {
  // If a package has been found in a previous iteration or passed via CLI,
  // stop.
  if (context->package_uid.has_value()) {
    return base::OkStatus();
  }

  if (!packet.has_packages_list()) {
    return base::OkStatus();
  }

  protos::pbzero::PackagesList::Decoder packages_list_decoder(
      packet.packages_list());

  for (auto package = packages_list_decoder.packages(); package; ++package) {
    protos::pbzero::PackagesList::PackageInfo::Decoder info(*package);

    if (!info.has_name() || !info.has_uid()) {
      continue;
    }

    base::StringView expected_name(context->package_name.data(),
                                   context->package_name.size());
    base::StringView actual_name(info.name().data, info.name().size);
    if (actual_name != expected_name) {
      continue;
    }

    // This code path is only reached when no explicit --uid was passed to
    // trace_redactor (when --uid is provided, context->package_uid holds the
    // full per-user UID and Collect() returns early above).
    //
    // On Android, PackageManager writes packages.list with a single shared
    // entry per installed package using its normalized UID (uid % 100,000);
    // full per-user UIDs (e.g. 1010234 for User 10) never appear in
    // packages.list. Therefore, when resolved here from PackagesList,
    // context->package_uid holds the normalized UID (and context->normalize_uid
    // is set to true so ProcessTree UIDs are normalized to match).
    //
    // Verify that the PackagesList entry is already normalized as expected.
    if (NormalizeUid(info.uid()) != info.uid()) {
      return base::ErrStatus(
          "FindPackageUid: package list uid is not normalized.");
    }

    context->package_uid = info.uid();
    return base::OkStatus();
  }

  // Nothing was found. There should only be one package list, but we keep
  // looking just incase. The error case will be handled in End().
  return base::OkStatus();
}

base::Status FindPackageUid::End(Context* context) const {
  if (!context->package_uid.has_value()) {
    return base::ErrStatus("FindPackageUid: did not find package uid.");
  }

  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction
