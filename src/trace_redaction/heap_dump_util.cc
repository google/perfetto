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

#include "src/trace_redaction/heap_dump_util.h"

#include <string_view>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"

#include "protos/perfetto/trace/profiling/deobfuscation.pbzero.h"

namespace perfetto::trace_redaction {
namespace heap_dump {
namespace {

using TracePacket = protos::pbzero::TracePacket;

}  // namespace

std::optional<protozero::ConstChars> DeobfuscationMappingPackage(
    protozero::ProtoDecoder* packet) {
  // Exactly one mapping, and nothing but non-data fields next to it, so a
  // single package check covers the whole packet.
  std::optional<protozero::Field> mapping;
  packet->Reset();
  for (auto field = packet->ReadField(); field.valid();
       field = packet->ReadField()) {
    if (field.id() == TracePacket::kDeobfuscationMappingFieldNumber) {
      if (PERFETTO_UNLIKELY(
              mapping.has_value() ||
              field.type() !=
                  protozero::proto_utils::ProtoWireType::kLengthDelimited)) {
        return std::nullopt;
      }
      mapping = field;
    } else if (PERFETTO_UNLIKELY(!IsTracePacketNonDataField(field.id()))) {
      return std::nullopt;
    }
  }
  if (PERFETTO_UNLIKELY(!mapping.has_value())) {
    return std::nullopt;
  }

  // package_name is field 1 and is emitted first; the classes that follow can
  // be several MB. Require exactly one package_name so there is no ambiguity
  // about which one a consumer uses.
  protozero::ProtoDecoder decoder(mapping->as_bytes());
  std::optional<protozero::ConstChars> name;
  for (auto field = decoder.ReadField(); field.valid();
       field = decoder.ReadField()) {
    if (field.id() !=
        protos::pbzero::DeobfuscationMapping::kPackageNameFieldNumber) {
      continue;
    }
    if (PERFETTO_UNLIKELY(
            name.has_value() ||
            field.type() !=
                protozero::proto_utils::ProtoWireType::kLengthDelimited)) {
      return std::nullopt;
    }
    name = field.as_string();
  }
  return name;
}

}  // namespace heap_dump

bool AllowTargetDeobfuscationMapping::Includes(
    const Context& context,
    const std::string& packet) const {
  // Validated once before Transform() by PackageValidation.
  PERFETTO_DCHECK(!context.package_name.empty());
  protozero::ProtoDecoder decoder(packet);
  auto name = heap_dump::DeobfuscationMappingPackage(&decoder);
  if (!name.has_value() ||
      std::string_view(name->data, name->size) != context.package_name) {
    return false;
  }
  context.stats.deobfuscation_mappings_kept++;
  return true;
}

HeapDumpRedactionSummary SummarizeHeapDumpRedaction(const Context& context) {
  HeapDumpRedactionSummary summary;

  for (const auto& [key, sequence] : context.heap_dump_sequences) {
    bool is_target_dump = context.IsTargetUid(key.writer_uid);
    if (is_target_dump && sequence.is_complete) {
      summary.heap_graph_packets_kept += sequence.heap_graph_packets;
      summary.complete_dumps_kept++;
      continue;
    }
    summary.heap_graph_packets_dropped += sequence.heap_graph_packets;
    if (is_target_dump) {
      summary.incomplete_dumps_dropped++;
    }
  }

  summary.untrusted_app_packets_dropped =
      context.stats.untrusted_app_packets_dropped;
  summary.missing_uid_packets_dropped =
      context.stats.missing_uid_packets_dropped;
  summary.deobfuscation_mappings_kept =
      context.stats.deobfuscation_mappings_kept;
  return summary;
}

}  // namespace perfetto::trace_redaction
