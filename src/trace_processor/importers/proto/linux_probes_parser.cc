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
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/log_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/types/variadic.h"

namespace perfetto::trace_processor {

LinuxProbesParser::LinuxProbesParser(TraceProcessorContext* context)
    : context_(context),
      uid_key_id_(context->storage->InternString("uid")),
      comm_key_id_(context->storage->InternString("comm")),
      systemd_unit_key_id_(context->storage->InternString("systemd_unit")),
      hostname_key_id_(context->storage->InternString("hostname")),
      transport_key_id_(context->storage->InternString("transport")),
      journald_source_id_(context->storage->InternString("journald")) {}

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

  tables::LogTable::Row row;
  row.ts = ts;
  row.utid = utid;
  row.prio = prio;
  row.log_source = journald_source_id_;
  row.tag = evt.has_tag() ? std::make_optional(tag_id) : std::nullopt;
  row.msg = msg_id;
  auto id = context_->storage->mutable_log_table()->Insert(row).id;

  if (uid || evt.has_comm() || evt.has_systemd_unit() || evt.has_hostname() ||
      evt.has_transport()) {
    ArgsTracker args_tracker(context_);
    auto inserter = args_tracker.AddArgsTo(id);
    if (uid) {
      inserter.AddArg(uid_key_id_, uid_key_id_,
                      Variadic::Integer(static_cast<int64_t>(*uid)));
    }
    if (evt.has_comm()) {
      inserter.AddArg(comm_key_id_, comm_key_id_, Variadic::String(comm_id));
    }
    if (evt.has_systemd_unit()) {
      inserter.AddArg(systemd_unit_key_id_, systemd_unit_key_id_,
                      Variadic::String(unit_id));
    }
    if (evt.has_hostname()) {
      inserter.AddArg(hostname_key_id_, hostname_key_id_,
                      Variadic::String(host_id));
    }
    if (evt.has_transport()) {
      inserter.AddArg(transport_key_id_, transport_key_id_,
                      Variadic::String(transport_id));
    }
  }
}

}  // namespace perfetto::trace_processor
