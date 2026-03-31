/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/trace_processor/importers/proto/linux_probes_parser.h"

#include <cstdint>
#include <optional>

#include "perfetto/ext/base/string_view.h"
#include "protos/perfetto/trace/linux/journald_event.pbzero.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/linux_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {

LinuxProbesParser::LinuxProbesParser(TraceProcessorContext* context)
    : context_(context) {}

void LinuxProbesParser::ParseJournaldPacket(int64_t ts,
                                            protozero::ConstBytes blob) {
  protos::pbzero::JournaldEventPacket::Decoder pkt(blob);
  for (auto it = pkt.events(); it; ++it) {
    ParseJournaldEvent(ts, *it);
  }
}

void LinuxProbesParser::ParseJournaldEvent(int64_t ts,
                                           protozero::ConstBytes blob) {
  protos::pbzero::JournaldEventPacket::JournaldEvent::Decoder evt(blob);

  auto pid = evt.has_pid() ? static_cast<uint32_t>(evt.pid()) : 0u;
  auto tid = evt.has_tid() ? static_cast<uint32_t>(evt.tid()) : pid;

  std::optional<uint32_t> utid;
  if (pid != 0) {
    utid = context_->process_tracker->UpdateThread(tid, pid);
  }

  auto prio = static_cast<uint32_t>(evt.has_prio() ? evt.prio() : 0u);

  StringId tag_id = evt.has_tag() ? context_->storage->InternString(evt.tag())
                                  : kNullStringId;
  StringId msg_id = context_->storage->InternString(
      evt.has_message() ? evt.message() : base::StringView());

  std::optional<uint32_t> uid;
  if (evt.has_uid())
    uid = static_cast<uint32_t>(evt.uid());

  StringId comm_id = evt.has_comm()
                         ? context_->storage->InternString(evt.comm())
                         : kNullStringId;
  StringId unit_id = evt.has_systemd_unit()
                         ? context_->storage->InternString(evt.systemd_unit())
                         : kNullStringId;
  StringId host_id = evt.has_hostname()
                         ? context_->storage->InternString(evt.hostname())
                         : kNullStringId;
  StringId transport_id = evt.has_transport()
                              ? context_->storage->InternString(evt.transport())
                              : kNullStringId;

  tables::JournaldLogTable::Row row;
  row.ts = ts;
  row.utid = utid;
  row.prio = prio;
  row.tag = evt.has_tag() ? std::make_optional(tag_id) : std::nullopt;
  row.msg = msg_id;
  row.uid = uid;
  row.comm = evt.has_comm() ? std::make_optional(comm_id) : std::nullopt;
  row.systemd_unit =
      evt.has_systemd_unit() ? std::make_optional(unit_id) : std::nullopt;
  row.hostname =
      evt.has_hostname() ? std::make_optional(host_id) : std::nullopt;
  row.transport =
      evt.has_transport() ? std::make_optional(transport_id) : std::nullopt;

  context_->storage->mutable_journald_log_table()->Insert(row);
}

}  // namespace perfetto::trace_processor
