/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/trace_processor/sqlite_raw_table.h"

#include <inttypes.h>

#include "perfetto/base/compiler.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/ftrace_utils.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"
#include "src/trace_processor/types/gfp_flags.h"
#include "src/trace_processor/types/variadic.h"

#include "protos/perfetto/trace/ftrace/binder.pbzero.h"
#include "protos/perfetto/trace/ftrace/clk.pbzero.h"
#include "protos/perfetto/trace/ftrace/filemap.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/power.pbzero.h"
#include "protos/perfetto/trace/ftrace/sched.pbzero.h"
#include "protos/perfetto/trace/ftrace/workqueue.pbzero.h"

namespace perfetto {
namespace trace_processor {

namespace {
std::tuple<uint32_t, uint32_t> ParseKernelReleaseVersion(
    base::StringView system_release) {
  size_t first_dot_pos = system_release.find(".");
  size_t second_dot_pos = system_release.find(".", first_dot_pos + 1);
  auto major_version = base::StringToUInt32(
      system_release.substr(0, first_dot_pos).ToStdString());
  auto minor_version = base::StringToUInt32(
      system_release
          .substr(first_dot_pos + 1, second_dot_pos - (first_dot_pos + 1))
          .ToStdString());
  return std::make_tuple(major_version.value(), minor_version.value());
}

int FreeString(char* ptr) {
  free(ptr);
  return 0;
}

struct FtraceTime {
  FtraceTime(int64_t ns)
      : secs(ns / 1000000000LL), micros((ns - secs * 1000000000LL) / 1000) {}

  const int64_t secs;
  const int64_t micros;
};

class SystraceSerializer {
 public:
  using ScopedString = base::ScopedResource<char*, FreeString, nullptr>;

  SystraceSerializer(const TraceStorage* storage, uint32_t raw_row)
      : storage_(storage), raw_row_(raw_row) {}

  ScopedString SerializeToString();

 private:
  void SerializePrefix(base::StringWriter* writer);
  void SerializeArgs(NullTermStringView event_name,
                     ArgSetId arg_set_id,
                     base::StringWriter* writer);
  bool ParseGfpFlags(Variadic value, base::StringWriter* writer);

  const TraceStorage* storage_ = nullptr;
  uint32_t raw_row_ = 0;
};

SystraceSerializer::ScopedString SystraceSerializer::SerializeToString() {
  const auto& raw = storage_->raw_table();

  char line[4096];
  base::StringWriter writer(line, sizeof(line));

  SerializePrefix(&writer);

  const auto& event_name = storage_->GetString(raw.name()[raw_row_]);
  writer.AppendChar(' ');
  if (event_name == "print") {
    writer.AppendString("tracing_mark_write");
  } else {
    writer.AppendString(event_name.c_str(), event_name.size());
  }
  writer.AppendChar(':');

  SerializeArgs(event_name, raw.arg_set_id()[raw_row_], &writer);

  return ScopedString(writer.CreateStringCopy());
}

void SystraceSerializer::SerializePrefix(base::StringWriter* writer) {
  const auto& raw = storage_->raw_table();

  int64_t ts = raw.ts()[raw_row_];
  uint32_t cpu = raw.cpu()[raw_row_];

  UniqueTid utid = raw.utid()[raw_row_];
  uint32_t tid = storage_->thread_table().tid()[utid];

  uint32_t tgid = 0;
  auto opt_upid = storage_->thread_table().upid()[utid];
  if (opt_upid.has_value()) {
    tgid = storage_->process_table().pid()[*opt_upid];
  }
  auto name = storage_->GetString(storage_->thread_table().name()[utid]);

  FtraceTime ftrace_time(ts);
  if (tid == 0) {
    name = "<idle>";
  } else if (name == "") {
    name = "<unknown>";
  } else if (name == "CrRendererMain") {
    // TODO(taylori): Remove this when crbug.com/978093 is fixed or
    // when a better solution is found.
    name = "CrRendererMainThread";
  }

  int64_t padding = 16 - static_cast<int64_t>(name.size());
  if (padding > 0) {
    writer->AppendChar(' ', static_cast<size_t>(padding));
  }
  for (size_t i = 0; i < name.size(); ++i) {
    char c = name.data()[i];
    writer->AppendChar(c == '-' ? '_' : c);
  }
  writer->AppendChar('-');

  size_t pre_pid_pos = writer->pos();
  writer->AppendInt(tid);
  size_t pid_chars = writer->pos() - pre_pid_pos;
  if (PERFETTO_LIKELY(pid_chars < 5)) {
    writer->AppendChar(' ', 5 - pid_chars);
  }

  writer->AppendLiteral(" (");
  if (tgid == 0) {
    writer->AppendLiteral("-----");
  } else {
    writer->AppendPaddedInt<' ', 5>(tgid);
  }
  writer->AppendLiteral(") [");
  writer->AppendPaddedInt<'0', 3>(cpu);
  writer->AppendLiteral("] .... ");

  writer->AppendInt(ftrace_time.secs);
  writer->AppendChar('.');
  writer->AppendPaddedInt<'0', 6>(ftrace_time.micros);
  writer->AppendChar(':');
}

void SystraceSerializer::SerializeArgs(NullTermStringView event_name,
                                       ArgSetId arg_set_id,
                                       base::StringWriter* writer) {
  const auto& set_ids = storage_->arg_table().arg_set_id();

  // TODO(lalitm): this code is quite hacky for performance reasons. We assume
  // that the row map is a contiguous range (which is always the case
  // because arg_set_ids are contiguous by definition). We also assume that
  // the proto field order is also the order of insertion (which happens to
  // be true but proabably shouldn't be relied on).
  RowMap rm = storage_->arg_table().FilterToRowMap({set_ids.eq(arg_set_id)});
  if (rm.empty())
    return;

  uint32_t start_row = rm.Get(0);
  using ValueWriter = std::function<void(const Variadic&)>;
  auto write_value = [this, writer](const Variadic& value) {
    switch (value.type) {
      case Variadic::kInt:
        writer->AppendInt(value.int_value);
        break;
      case Variadic::kUint:
        writer->AppendUnsignedInt(value.uint_value);
        break;
      case Variadic::kString: {
        const auto& str = storage_->GetString(value.string_value);
        writer->AppendString(str.c_str(), str.size());
        break;
      }
      case Variadic::kReal:
        writer->AppendDouble(value.real_value);
        break;
      case Variadic::kPointer:
        writer->AppendUnsignedInt(value.pointer_value);
        break;
      case Variadic::kBool:
        writer->AppendBool(value.bool_value);
        break;
      case Variadic::kJson: {
        const auto& str = storage_->GetString(value.json_value);
        writer->AppendString(str.c_str(), str.size());
        break;
      }
    }
  };
  auto write_value_at_index = [this, start_row](uint32_t arg_idx,
                                                ValueWriter value_fn) {
    value_fn(storage_->GetArgValue(start_row + arg_idx));
  };
  auto write_arg = [this, writer, start_row](uint32_t arg_idx,
                                             ValueWriter value_fn) {
    uint32_t arg_row = start_row + arg_idx;
    const auto& args = storage_->arg_table();
    const auto& key = storage_->GetString(args.key()[arg_row]);
    auto value = storage_->GetArgValue(arg_row);

    writer->AppendChar(' ');
    writer->AppendString(key.c_str(), key.size());
    writer->AppendChar('=');

    if (key == "gfp_flags" && ParseGfpFlags(value, writer))
      return;
    value_fn(value);
  };

  if (event_name == "sched_switch") {
    using SS = protos::pbzero::SchedSwitchFtraceEvent;
    write_arg(SS::kPrevCommFieldNumber - 1, write_value);
    write_arg(SS::kPrevPidFieldNumber - 1, write_value);
    write_arg(SS::kPrevPrioFieldNumber - 1, write_value);
    write_arg(SS::kPrevStateFieldNumber - 1, [writer](const Variadic& value) {
      PERFETTO_DCHECK(value.type == Variadic::Type::kInt);
      auto state = static_cast<uint16_t>(value.int_value);
      writer->AppendString(ftrace_utils::TaskState(state).ToString('|').data());
    });
    writer->AppendLiteral(" ==>");
    write_arg(SS::kNextCommFieldNumber - 1, write_value);
    write_arg(SS::kNextPidFieldNumber - 1, write_value);
    write_arg(SS::kNextPrioFieldNumber - 1, write_value);
    return;
  } else if (event_name == "sched_wakeup") {
    using SW = protos::pbzero::SchedWakeupFtraceEvent;
    write_arg(SW::kCommFieldNumber - 1, write_value);
    write_arg(SW::kPidFieldNumber - 1, write_value);
    write_arg(SW::kPrioFieldNumber - 1, write_value);
    write_arg(SW::kTargetCpuFieldNumber - 1, [writer](const Variadic& value) {
      PERFETTO_DCHECK(value.type == Variadic::Type::kInt);
      writer->AppendPaddedInt<'0', 3>(value.int_value);
    });
    return;
  } else if (event_name == "clock_set_rate") {
    // TODO(lalitm): this is a big hack but the best way to do this now.
    // Doing this requires overhauling how we deal with args by pushing them all
    // to an array and then reading back from that array.

    // We use the string "todo" as the name to stay consistent with old
    // trace_to_text print code.
    writer->AppendString(" todo");
    write_arg(0 /* state */, write_value);
    write_arg(1 /* cpu_id */, write_value);
    return;
  } else if (event_name == "clk_set_rate") {
    using CSR = protos::pbzero::ClkSetRateFtraceEvent;
    writer->AppendLiteral(" ");
    write_value_at_index(CSR::kNameFieldNumber - 1, write_value);
    writer->AppendLiteral(" ");
    write_value_at_index(CSR::kRateFieldNumber - 1, write_value);
    return;
  } else if (event_name == "clock_enable") {
    using CE = protos::pbzero::ClockEnableFtraceEvent;
    write_value_at_index(CE::kNameFieldNumber - 1, write_value);
    write_arg(CE::kStateFieldNumber - 1, write_value);
    write_arg(CE::kCpuIdFieldNumber - 1, write_value);
    return;
  } else if (event_name == "clock_disable") {
    using CD = protos::pbzero::ClockDisableFtraceEvent;
    write_value_at_index(CD::kNameFieldNumber - 1, write_value);
    write_arg(CD::kStateFieldNumber - 1, write_value);
    write_arg(CD::kCpuIdFieldNumber - 1, write_value);
    return;
  } else if (event_name == "binder_transaction") {
    using BT = protos::pbzero::BinderTransactionFtraceEvent;
    writer->AppendString(" transaction=");
    write_value_at_index(BT::kDebugIdFieldNumber - 1, write_value);

    writer->AppendString(" dest_node=");
    write_value_at_index(BT::kTargetNodeFieldNumber - 1, write_value);

    writer->AppendString(" dest_proc=");
    write_value_at_index(BT::kToProcFieldNumber - 1, write_value);

    writer->AppendString(" dest_thread=");
    write_value_at_index(BT::kToThreadFieldNumber - 1, write_value);

    writer->AppendString(" reply=");
    write_value_at_index(BT::kReplyFieldNumber - 1, write_value);

    writer->AppendString(" flags=0x");
    write_value_at_index(BT::kFlagsFieldNumber - 1,
                         [writer](const Variadic& value) {
                           PERFETTO_DCHECK(value.type == Variadic::Type::kUint);
                           writer->AppendHexInt(value.uint_value);
                         });

    writer->AppendString(" code=0x");
    write_value_at_index(BT::kCodeFieldNumber - 1,
                         [writer](const Variadic& value) {
                           PERFETTO_DCHECK(value.type == Variadic::Type::kUint);
                           writer->AppendHexInt(value.uint_value);
                         });
    return;
  } else if (event_name == "binder_transaction_alloc_buf") {
    using BTAB = protos::pbzero::BinderTransactionAllocBufFtraceEvent;
    writer->AppendString(" transaction=");
    write_value_at_index(BTAB::kDebugIdFieldNumber - 1, write_value);
    write_arg(BTAB::kDataSizeFieldNumber - 1, write_value);
    write_arg(BTAB::kOffsetsSizeFieldNumber - 1, write_value);
    return;
  } else if (event_name == "binder_transaction_received") {
    using BTR = protos::pbzero::BinderTransactionReceivedFtraceEvent;
    writer->AppendString(" transaction=");
    write_value_at_index(BTR::kDebugIdFieldNumber - 1, write_value);
    return;
  } else if (event_name == "mm_filemap_add_to_page_cache") {
    using MFA = protos::pbzero::MmFilemapAddToPageCacheFtraceEvent;
    writer->AppendString(" dev ");
    write_value_at_index(MFA::kSDevFieldNumber - 1,
                         [writer](const Variadic& value) {
                           PERFETTO_DCHECK(value.type == Variadic::Type::kUint);
                           writer->AppendUnsignedInt(value.uint_value >> 20);
                         });
    writer->AppendString(":");
    write_value_at_index(
        MFA::kSDevFieldNumber - 1, [writer](const Variadic& value) {
          PERFETTO_DCHECK(value.type == Variadic::Type::kUint);
          writer->AppendUnsignedInt(value.uint_value & ((1 << 20) - 1));
        });
    writer->AppendString(" ino ");
    write_value_at_index(MFA::kIInoFieldNumber - 1,
                         [writer](const Variadic& value) {
                           PERFETTO_DCHECK(value.type == Variadic::Type::kUint);
                           writer->AppendHexInt(value.uint_value);
                         });
    writer->AppendString(" page=0000000000000000");
    writer->AppendString(" pfn=");
    write_value_at_index(MFA::kPfnFieldNumber - 1, write_value);
    writer->AppendString(" ofs=");
    write_value_at_index(MFA::kIndexFieldNumber - 1,
                         [writer](const Variadic& value) {
                           PERFETTO_DCHECK(value.type == Variadic::Type::kUint);
                           writer->AppendUnsignedInt(value.uint_value << 12);
                         });
    return;
  } else if (event_name == "print") {
    // 'ip' may be the first field or it may be dropped. We only care
    // about the 'buf' field which will always appear last.
    uint32_t arg_row = rm.Get(rm.size() - 1);
    const auto& value = storage_->GetArgValue(arg_row);
    const auto& str = storage_->GetString(value.string_value);
    // If the last character is a newline in a print, just drop it.
    auto chars_to_print = !str.empty() && str.c_str()[str.size() - 1] == '\n'
                              ? str.size() - 1
                              : str.size();
    writer->AppendChar(' ');
    writer->AppendString(str.c_str(), chars_to_print);
    return;
  } else if (event_name == "sched_blocked_reason") {
    using SBR = protos::pbzero::SchedBlockedReasonFtraceEvent;
    write_arg(SBR::kPidFieldNumber - 1, write_value);
    write_arg(SBR::kIoWaitFieldNumber - 1, write_value);
    write_arg(SBR::kCallerFieldNumber - 1, [writer](const Variadic& value) {
      PERFETTO_DCHECK(value.type == Variadic::Type::kUint);
      writer->AppendHexInt(value.uint_value);
    });
    return;
  } else if (event_name == "workqueue_activate_work") {
    using WAW = protos::pbzero::WorkqueueActivateWorkFtraceEvent;
    writer->AppendString(" work struct ");
    write_value_at_index(WAW::kWorkFieldNumber - 1,
                         [writer](const Variadic& value) {
                           PERFETTO_DCHECK(value.type == Variadic::Type::kUint);
                           writer->AppendHexInt(value.uint_value);
                         });
    return;
  } else if (event_name == "workqueue_execute_start") {
    using WES = protos::pbzero::WorkqueueExecuteStartFtraceEvent;
    writer->AppendString(" work struct ");
    write_value_at_index(WES::kWorkFieldNumber - 1,
                         [writer](const Variadic& value) {
                           PERFETTO_DCHECK(value.type == Variadic::Type::kUint);
                           writer->AppendHexInt(value.uint_value);
                         });
    writer->AppendString(": function ");
    write_value_at_index(WES::kFunctionFieldNumber - 1,
                         [writer](const Variadic& value) {
                           PERFETTO_DCHECK(value.type == Variadic::Type::kUint);
                           writer->AppendHexInt(value.uint_value);
                         });
    return;
  } else if (event_name == "workqueue_execute_end") {
    using WE = protos::pbzero::WorkqueueExecuteEndFtraceEvent;
    writer->AppendString(" work struct ");
    write_value_at_index(WE::kWorkFieldNumber - 1,
                         [writer](const Variadic& value) {
                           PERFETTO_DCHECK(value.type == Variadic::Type::kUint);
                           writer->AppendHexInt(value.uint_value);
                         });
    return;
  } else if (event_name == "workqueue_queue_work") {
    using WQW = protos::pbzero::WorkqueueQueueWorkFtraceEvent;
    writer->AppendString(" work struct=");
    write_value_at_index(WQW::kWorkFieldNumber - 1,
                         [writer](const Variadic& value) {
                           PERFETTO_DCHECK(value.type == Variadic::Type::kUint);
                           writer->AppendHexInt(value.uint_value);
                         });
    write_arg(WQW::kFunctionFieldNumber - 1, [writer](const Variadic& value) {
      PERFETTO_DCHECK(value.type == Variadic::Type::kUint);
      writer->AppendHexInt(value.uint_value);
    });
    write_arg(WQW::kWorkqueueFieldNumber - 1, [writer](const Variadic& value) {
      PERFETTO_DCHECK(value.type == Variadic::Type::kUint);
      writer->AppendHexInt(value.uint_value);
    });
    write_value_at_index(WQW::kReqCpuFieldNumber - 1, write_value);
    write_value_at_index(WQW::kCpuFieldNumber - 1, write_value);
    return;
  }

  for (auto it = rm.IterateRows(); it; it.Next()) {
    write_arg(it.index(), write_value);
  }
}

bool SystraceSerializer::ParseGfpFlags(Variadic value,
                                       base::StringWriter* writer) {
  const auto& metadata_table = storage_->metadata_table();

  auto opt_name_idx = metadata_table.name().IndexOf(
      metadata::kNames[metadata::KeyIDs::system_name]);
  auto opt_release_idx = metadata_table.name().IndexOf(
      metadata::kNames[metadata::KeyIDs::system_release]);
  if (!opt_name_idx || !opt_release_idx)
    return false;

  StringId name = metadata_table.str_value()[*opt_name_idx];
  base::StringView system_name = storage_->GetString(name);
  if (system_name != "Linux")
    return false;

  StringId release = metadata_table.str_value()[*opt_release_idx];
  base::StringView system_release = storage_->GetString(release);
  auto version = ParseKernelReleaseVersion(system_release);

  WriteGfpFlag(value.uint_value, version, writer);
  return true;
}

}  // namespace

SqliteRawTable::SqliteRawTable(sqlite3* db, Context context)
    : DbSqliteTable(
          db,
          {context.cache, tables::RawTable::Schema(), TableComputation::kStatic,
           &context.storage->raw_table(), nullptr}),
      storage_(context.storage) {
  auto fn = [](sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    auto* thiz = static_cast<SqliteRawTable*>(sqlite3_user_data(ctx));
    thiz->ToSystrace(ctx, argc, argv);
  };
  sqlite3_create_function(db, "to_ftrace", 1,
                          SQLITE_UTF8 | SQLITE_DETERMINISTIC, this, fn, nullptr,
                          nullptr);
}

SqliteRawTable::~SqliteRawTable() = default;

void SqliteRawTable::RegisterTable(sqlite3* db,
                                   QueryCache* cache,
                                   const TraceStorage* storage) {
  SqliteTable::Register<SqliteRawTable, Context>(db, Context{cache, storage},
                                                 "raw");
}

void SqliteRawTable::ToSystrace(sqlite3_context* ctx,
                                int argc,
                                sqlite3_value** argv) {
  if (argc != 1 || sqlite3_value_type(argv[0]) != SQLITE_INTEGER) {
    sqlite3_result_error(ctx, "Usage: to_ftrace(id)", -1);
    return;
  }
  uint32_t row = static_cast<uint32_t>(sqlite3_value_int64(argv[0]));

  SystraceSerializer seralizer(storage_, row);
  sqlite3_result_text(ctx, seralizer.SerializeToString().release(), -1, free);
}

}  // namespace trace_processor
}  // namespace perfetto
