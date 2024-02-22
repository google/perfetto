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

#include "src/trace_redaction/prune_package_list.h"

#include "protos/perfetto/trace/android/packages_list.gen.h"

namespace perfetto::trace_redaction {

PrunePackageList::PrunePackageList() = default;
PrunePackageList::~PrunePackageList() = default;

base::Status PrunePackageList::Transform(const Context& context,
                                         std::string* packet) const {
  if (!context.package_uid.has_value()) {
    return base::ErrStatus("PrunePackageList: missing package uid.");
  }

  if (protos::pbzero::TracePacket::Decoder trace_packet_decoder(*packet);
      !trace_packet_decoder.has_packages_list()) {
    return base::OkStatus();
  }

  auto normalized_uid = NormalizeUid(context.package_uid.value());

  protos::gen::TracePacket mutable_packet;
  mutable_packet.ParseFromString(*packet);

  auto* packages = mutable_packet.mutable_packages_list()->mutable_packages();

  // Remove all entries that don't match the uid. After this, one or more
  // packages will be left in the list (multiple packages can share a uid).
  packages->erase(
      std::remove_if(
          packages->begin(), packages->end(),
          [normalized_uid](const protos::gen::PackagesList::PackageInfo& info) {
            return NormalizeUid(info.uid()) != normalized_uid;
          }),
      packages->end());

  packet->assign(mutable_packet.SerializeAsString());

  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction
