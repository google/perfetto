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

#include "src/trace_redaction/prune_heap_dumps.h"

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "src/trace_redaction/heap_dump_util.h"

#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::trace_redaction {
namespace {

using TracePacket = protos::pbzero::TracePacket;

}  // namespace

base::Status PruneHeapDumps::Transform(const Context& context,
                                       std::string* packet) const {
  // Validated once before Transform() by PackageValidation.
  PERFETTO_DCHECK(context.package_uid.has_value());

  TracePacket::Decoder packet_decoder(*packet);

  // Uid-less packets are handled by PruneMissingUidPackets.
  auto dump_key = HeapDumpKey::Of(packet_decoder);
  if (PERFETTO_UNLIKELY(!dump_key.has_value())) {
    return base::OkStatus();
  }

  const bool has_deobfuscation_mapping =
      packet_decoder.has_deobfuscation_mapping();
  if (PERFETTO_UNLIKELY(has_deobfuscation_mapping &&
                        !context.IsTargetUid(dump_key->writer_uid))) {
    packet->clear();
    return base::OkStatus();
  }

  const bool has_heap_dump_fields =
      packet_decoder.Get(kHeapGraphFieldNumber).valid() ||
      packet_decoder.has_art_process_metadata();
  if (PERFETTO_UNLIKELY(!has_heap_dump_fields)) {
    if (PERFETTO_UNLIKELY(has_deobfuscation_mapping)) {
      context.stats.deobfuscation_mappings_kept++;
    }
    return base::OkStatus();
  }

  const bool should_remove_packet =
      !context.IsTargetUid(dump_key->writer_uid) ||
      !context.IsHeapDumpComplete(*dump_key);
  if (should_remove_packet) {
    packet->clear();
  } else if (PERFETTO_UNLIKELY(has_deobfuscation_mapping)) {
    context.stats.deobfuscation_mappings_kept++;
  }
  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction
