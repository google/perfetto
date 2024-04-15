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
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_view.h"
#include "src/trace_redaction/trace_redaction_framework.h"

#include "protos/perfetto/trace/android/packages_list.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::trace_redaction {
namespace {

using PackagesList_PackageInfo = protos::pbzero::PackagesList_PackageInfo;
using PackagesList = protos::pbzero::PackagesList;

}  // namespace

FindPackageUid::FindPackageUid() = default;

FindPackageUid::~FindPackageUid() = default;

base::StatusOr<CollectPrimitive::ContinueCollection> FindPackageUid::Collect(
    const protos::pbzero::TracePacket_Decoder& packet,
    Context* context) const {
  if (context->package_name.empty()) {
    return base::ErrStatus("FindPackageUid: missing package name.");
  }

  // Skip package and move onto the next packet.
  if (!packet.has_packages_list()) {
    return ContinueCollection::kNextPacket;
  }

  const PackagesList::Decoder pkg_list_decoder(packet.packages_list());

  for (auto pkg_it = pkg_list_decoder.packages(); pkg_it; ++pkg_it) {
    PackagesList_PackageInfo::Decoder pkg_info(*pkg_it);

    if (pkg_info.has_name() && pkg_info.has_uid()) {
      // Package names should be lowercase, but this check is meant to be more
      // forgiving.
      if (base::StringView(context->package_name)
              .CaseInsensitiveEq(pkg_info.name())) {
        context->package_uid = NormalizeUid(pkg_info.uid());
        return ContinueCollection::kRetire;
      }
    }
  }

  return ContinueCollection::kNextPacket;
}

}  // namespace perfetto::trace_redaction
