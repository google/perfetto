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
    return base::ErrStatus("FindPackageUid: package uid already found.");
  }

  return base::OkStatus();
}

base::Status FindPackageUid::Collect(
    const protos::pbzero::TracePacket::Decoder& packet,
    Context* context) const {
  // If a package has been found in a pervious iteration, stop.
  if (context->package_uid.has_value()) {
    return base::OkStatus();
  }

  // Skip package and move onto the next packet.
  if (!packet.has_packages_list()) {
    return base::OkStatus();
  }

  protos::pbzero::PackagesList::Decoder packages_list_decoder(
      packet.packages_list());

  for (auto package = packages_list_decoder.packages(); package; ++package) {
    protozero::ProtoDecoder package_decoder(*package);

    auto name = package_decoder.FindField(
        protos::pbzero::PackagesList::PackageInfo::kNameFieldNumber);
    auto uid = package_decoder.FindField(
        protos::pbzero::PackagesList::PackageInfo::kUidFieldNumber);

    if (name.valid() && uid.valid()) {
      // Package names should be lowercase, but this check is meant to be more
      // forgiving.
      if (base::StringView(context->package_name)
              .CaseInsensitiveEq(name.as_string())) {
        context->package_uid = NormalizeUid(uid.as_uint64());
        return base::OkStatus();
      }
    }
  }

  return base::OkStatus();
}

base::Status FindPackageUid::End(Context* context) const {
  if (!context->package_uid.has_value()) {
    return base::ErrStatus("FindPackageUid: did not find package uid.");
  }

  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction
