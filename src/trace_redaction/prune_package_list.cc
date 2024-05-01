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

#include <string>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "src/trace_redaction/proto_util.h"

#include "protos/perfetto/trace/android/packages_list.pbzero.h"

namespace perfetto::trace_redaction {
namespace {

bool ShouldKeepPackageInfo(protozero::Field package_info, uint64_t uid) {
  PERFETTO_DCHECK(package_info.id() ==
                  protos::pbzero::PackagesList::kPackagesFieldNumber);

  protozero::ProtoDecoder decoder(package_info.as_bytes());
  auto uid_field = decoder.FindField(
      protos::pbzero::PackagesList::PackageInfo::kUidFieldNumber);

  return uid_field.valid() &&
         NormalizeUid(uid_field.as_uint64()) == NormalizeUid(uid);
}

}  // namespace

base::Status PrunePackageList::Transform(const Context& context,
                                         std::string* packet) const {
  if (!context.package_uid.has_value()) {
    return base::ErrStatus("PrunePackageList: missing package uid.");
  }

  protozero::ProtoDecoder packet_decoder(*packet);

  protos::pbzero::TracePacket::Decoder trace_packet_decoder(*packet);

  auto package_list = packet_decoder.FindField(
      protos::pbzero::TracePacket::kPackagesListFieldNumber);

  if (!package_list.valid()) {
    return base::OkStatus();
  }

  auto uid = context.package_uid.value();

  protozero::HeapBuffered<protos::pbzero::TracePacket> packet_message;

  for (auto packet_field = packet_decoder.ReadField(); packet_field.valid();
       packet_field = packet_decoder.ReadField()) {
    if (packet_field.id() !=
        protos::pbzero::TracePacket::kPackagesListFieldNumber) {
      proto_util::AppendField(packet_field, packet_message.get());
      continue;
    }

    auto* package_list_message = packet_message->set_packages_list();

    protozero::ProtoDecoder package_list_decoder(packet_field.as_bytes());

    for (auto package_field = package_list_decoder.ReadField();
         package_field.valid();
         package_field = package_list_decoder.ReadField()) {
      // If not packages, keep.
      // If packages and uid matches, keep.
      if (package_field.id() !=
              protos::pbzero::PackagesList::kPackagesFieldNumber ||
          ShouldKeepPackageInfo(package_field, uid)) {
        proto_util::AppendField(package_field, package_list_message);
      }
    }
  }

  packet->assign(packet_message.SerializeAsString());

  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction
