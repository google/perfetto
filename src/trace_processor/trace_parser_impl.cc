/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "src/trace_processor/trace_parser_impl.h"
#include <string>
#include <type_traits>
#include "perfetto/base/logging.h"
#include "src/trace_processor/importers/common/builtin_trace_importers.h"
#include "src/trace_processor/importers/ftrace/ftrace_module_impl.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/counter_tables_py.h"
#include "src/trace_processor/tables/metadata_tables_py.h"
#include "src/trace_processor/tables/sched_tables_py.h"
#include "src/trace_processor/tables/slice_tables_py.h"
#include "src/trace_processor/trace_reader_registry.h"

namespace perfetto::trace_processor {
struct TraceParserImpl::OutputState {
  explicit OutputState(Sinks sinks)
      : sinks(sinks),
        slice(this, sinks.slice),
        args(this, sinks.args),
        sched(this, sinks.sched),
        thread_states(this, sinks.thread_states) {}
  Sinks sinks;
  TraceStorage* storage = nullptr;
  base::Status status;
  TraceParserString String(const dataframe::Dataframe::SinkValue& cell) {
    if (std::holds_alternative<std::monostate>(cell))
      return {};
    auto id = std::get<StringPool::Id>(cell);
    auto text = storage->GetString(id);
    return {id.raw_id(), text.c_str()
                             ? std::string_view(text.c_str(), text.size())
                             : std::string_view{}};
  }
  template <typename T>
  T Get(const dataframe::Dataframe::SinkValue& cell) {
    return std::get<T>(cell);
  }
  template <typename T>
  std::optional<T> Optional(const dataframe::Dataframe::SinkValue& cell) {
    if (std::holds_alternative<std::monostate>(cell))
      return std::nullopt;
    if constexpr (std::is_same_v<T, TraceParserString>)
      return String(cell);
    else
      return Get<T>(cell);
  }
  struct Adapter : dataframe::Dataframe::Sink {
    Adapter(OutputState* owner, TraceParserTableSink* sink)
        : owner(owner), sink(sink) {}
    OutputState* owner;
    TraceParserTableSink* sink;
    void OnStorageFrontierAdvance(uint32_t row) override {
      if (owner->status.ok())
        owner->status = sink->OnStorageFrontierAdvance(row);
    }
    void OnFrontierAdvance(uint32_t row) override {
      if (owner->status.ok())
        owner->status = sink->OnCompletionFrontierAdvance(row);
    }
    void OnClear() override {
      if (owner->status.ok())
        owner->status = sink->OnReset();
    }
  };
  struct SliceAdapter : Adapter {
    SliceAdapter(OutputState* owner, SliceSink* sink)
        : Adapter(owner, sink), typed(sink) {}
    SliceSink* typed;
    using C = tables::SliceTable::ColumnIndex;
    void OnInsert(uint32_t row,
                  const dataframe::Dataframe::SinkValue* cells,
                  uint32_t count) override {
      if (!owner->status.ok())
        return;
      PERFETTO_CHECK(count == 13);
      SliceSink::Row value;
      value.ts = owner->Get<int64_t>(cells[C::ts]);
      value.dur = owner->Get<int64_t>(cells[C::dur]);
      value.track_id = owner->Get<uint32_t>(cells[C::track_id]);
      value.category = owner->Optional<TraceParserString>(cells[C::category]);
      value.name = owner->Optional<TraceParserString>(cells[C::name]);
      value.depth = owner->Get<uint32_t>(cells[C::depth]);
      value.parent_id = owner->Optional<uint32_t>(cells[C::parent_id]);
      value.arg_set_id = owner->Optional<uint32_t>(cells[C::arg_set_id]);
      value.thread_ts = owner->Optional<int64_t>(cells[C::thread_ts]);
      value.thread_dur = owner->Optional<int64_t>(cells[C::thread_dur]);
      value.thread_instruction_count =
          owner->Optional<int64_t>(cells[C::thread_instruction_count]);
      value.thread_instruction_delta =
          owner->Optional<int64_t>(cells[C::thread_instruction_delta]);
      owner->status = typed->OnInsert(row, value);
    }
    void OnUpdate(uint32_t row,
                  uint32_t col,
                  const dataframe::Dataframe::SinkValue& cell) override {
      if (!owner->status.ok())
        return;
      switch (col) {
        case C::ts:
          owner->status = typed->OnTsUpdate(row, owner->Get<int64_t>(cell));
          return;
        case C::dur:
          owner->status = typed->OnDurUpdate(row, owner->Get<int64_t>(cell));
          return;
        case C::track_id:
          owner->status =
              typed->OnTrackIdUpdate(row, owner->Get<uint32_t>(cell));
          return;
        case C::category:
          owner->status = typed->OnCategoryUpdate(
              row, owner->Optional<TraceParserString>(cell));
          return;
        case C::name:
          owner->status = typed->OnNameUpdate(
              row, owner->Optional<TraceParserString>(cell));
          return;
        case C::depth:
          owner->status = typed->OnDepthUpdate(row, owner->Get<uint32_t>(cell));
          return;
        case C::parent_id:
          owner->status =
              typed->OnParentIdUpdate(row, owner->Optional<uint32_t>(cell));
          return;
        case C::arg_set_id:
          owner->status =
              typed->OnArgSetIdUpdate(row, owner->Optional<uint32_t>(cell));
          return;
        case C::thread_ts:
          owner->status =
              typed->OnThreadTsUpdate(row, owner->Optional<int64_t>(cell));
          return;
        case C::thread_dur:
          owner->status =
              typed->OnThreadDurUpdate(row, owner->Optional<int64_t>(cell));
          return;
        case C::thread_instruction_count:
          owner->status = typed->OnThreadInstructionCountUpdate(
              row, owner->Optional<int64_t>(cell));
          return;
        case C::thread_instruction_delta:
          owner->status = typed->OnThreadInstructionDeltaUpdate(
              row, owner->Optional<int64_t>(cell));
          return;
        default:
          owner->status = base::ErrStatus("Unsupported table update");
          return;
      }
    }
  };
  SliceAdapter slice;
  struct ArgAdapter : Adapter {
    ArgAdapter(OutputState* owner, ArgSink* sink)
        : Adapter(owner, sink), typed(sink) {}
    ArgSink* typed;
    using C = tables::ArgTable::ColumnIndex;
    void OnInsert(uint32_t row,
                  const dataframe::Dataframe::SinkValue* cells,
                  uint32_t count) override {
      if (!owner->status.ok())
        return;
      PERFETTO_CHECK(count == 8);
      ArgSink::Row value;
      value.arg_set_id = owner->Get<uint32_t>(cells[C::arg_set_id]);
      value.flat_key = owner->String(cells[C::flat_key]);
      value.key = owner->String(cells[C::key]);
      value.int_value = owner->Optional<int64_t>(cells[C::int_value]);
      value.string_value =
          owner->Optional<TraceParserString>(cells[C::string_value]);
      value.real_value = owner->Optional<double>(cells[C::real_value]);
      value.value_type = owner->String(cells[C::value_type]);
      owner->status = typed->OnInsert(row, value);
    }
    void OnUpdate(uint32_t row,
                  uint32_t col,
                  const dataframe::Dataframe::SinkValue& cell) override {
      if (!owner->status.ok())
        return;
      switch (col) {
        case C::arg_set_id:
          owner->status =
              typed->OnArgSetIdUpdate(row, owner->Get<uint32_t>(cell));
          return;
        case C::flat_key:
          owner->status = typed->OnFlatKeyUpdate(row, owner->String(cell));
          return;
        case C::key:
          owner->status = typed->OnKeyUpdate(row, owner->String(cell));
          return;
        case C::int_value:
          owner->status =
              typed->OnIntValueUpdate(row, owner->Optional<int64_t>(cell));
          return;
        case C::string_value:
          owner->status = typed->OnStringValueUpdate(
              row, owner->Optional<TraceParserString>(cell));
          return;
        case C::real_value:
          owner->status =
              typed->OnRealValueUpdate(row, owner->Optional<double>(cell));
          return;
        case C::value_type:
          owner->status = typed->OnValueTypeUpdate(row, owner->String(cell));
          return;
        default:
          owner->status = base::ErrStatus("Unsupported table update");
          return;
      }
    }
  };
  ArgAdapter args;
  struct SchedAdapter : Adapter {
    SchedAdapter(OutputState* owner, SchedSink* sink)
        : Adapter(owner, sink), typed(sink) {}
    SchedSink* typed;
    using C = tables::SchedSliceTable::ColumnIndex;
    void OnInsert(uint32_t row,
                  const dataframe::Dataframe::SinkValue* cells,
                  uint32_t count) override {
      if (!owner->status.ok())
        return;
      PERFETTO_CHECK(count == 7);
      SchedSink::Row value;
      value.ts = owner->Get<int64_t>(cells[C::ts]);
      value.dur = owner->Get<int64_t>(cells[C::dur]);
      value.utid = owner->Get<uint32_t>(cells[C::utid]);
      value.end_state = owner->String(cells[C::end_state]);
      value.priority = owner->Get<int32_t>(cells[C::priority]);
      value.ucpu = owner->Get<uint32_t>(cells[C::ucpu]);
      owner->status = typed->OnInsert(row, value);
    }
    void OnUpdate(uint32_t row,
                  uint32_t col,
                  const dataframe::Dataframe::SinkValue& cell) override {
      if (!owner->status.ok())
        return;
      switch (col) {
        case C::ts:
          owner->status = typed->OnTsUpdate(row, owner->Get<int64_t>(cell));
          return;
        case C::dur:
          owner->status = typed->OnDurUpdate(row, owner->Get<int64_t>(cell));
          return;
        case C::utid:
          owner->status = typed->OnUtidUpdate(row, owner->Get<uint32_t>(cell));
          return;
        case C::end_state:
          owner->status = typed->OnEndStateUpdate(row, owner->String(cell));
          return;
        case C::priority:
          owner->status =
              typed->OnPriorityUpdate(row, owner->Get<int32_t>(cell));
          return;
        case C::ucpu:
          owner->status = typed->OnUcpuUpdate(row, owner->Get<uint32_t>(cell));
          return;
        default:
          owner->status = base::ErrStatus("Unsupported table update");
          return;
      }
    }
  };
  SchedAdapter sched;
  struct ThreadStateAdapter : Adapter {
    ThreadStateAdapter(OutputState* owner, ThreadStateSink* sink)
        : Adapter(owner, sink), typed(sink) {}
    ThreadStateSink* typed;
    using C = tables::ThreadStateTable::ColumnIndex;
    void OnInsert(uint32_t row,
                  const dataframe::Dataframe::SinkValue* cells,
                  uint32_t count) override {
      if (!owner->status.ok())
        return;
      PERFETTO_CHECK(count == 11);
      ThreadStateSink::Row value;
      value.ts = owner->Get<int64_t>(cells[C::ts]);
      value.dur = owner->Get<int64_t>(cells[C::dur]);
      value.utid = owner->Get<uint32_t>(cells[C::utid]);
      value.state = owner->String(cells[C::state]);
      value.io_wait = owner->Optional<uint32_t>(cells[C::io_wait]);
      value.blocked_function =
          owner->Optional<TraceParserString>(cells[C::blocked_function]);
      value.waker_utid = owner->Optional<uint32_t>(cells[C::waker_utid]);
      value.waker_id = owner->Optional<uint32_t>(cells[C::waker_id]);
      value.irq_context = owner->Optional<uint32_t>(cells[C::irq_context]);
      value.ucpu = owner->Optional<uint32_t>(cells[C::ucpu]);
      owner->status = typed->OnInsert(row, value);
    }
    void OnUpdate(uint32_t row,
                  uint32_t col,
                  const dataframe::Dataframe::SinkValue& cell) override {
      if (!owner->status.ok())
        return;
      switch (col) {
        case C::ts:
          owner->status = typed->OnTsUpdate(row, owner->Get<int64_t>(cell));
          return;
        case C::dur:
          owner->status = typed->OnDurUpdate(row, owner->Get<int64_t>(cell));
          return;
        case C::utid:
          owner->status = typed->OnUtidUpdate(row, owner->Get<uint32_t>(cell));
          return;
        case C::state:
          owner->status = typed->OnStateUpdate(row, owner->String(cell));
          return;
        case C::io_wait:
          owner->status =
              typed->OnIoWaitUpdate(row, owner->Optional<uint32_t>(cell));
          return;
        case C::blocked_function:
          owner->status = typed->OnBlockedFunctionUpdate(
              row, owner->Optional<TraceParserString>(cell));
          return;
        case C::waker_utid:
          owner->status =
              typed->OnWakerUtidUpdate(row, owner->Optional<uint32_t>(cell));
          return;
        case C::waker_id:
          owner->status =
              typed->OnWakerIdUpdate(row, owner->Optional<uint32_t>(cell));
          return;
        case C::irq_context:
          owner->status =
              typed->OnIrqContextUpdate(row, owner->Optional<uint32_t>(cell));
          return;
        case C::ucpu:
          owner->status =
              typed->OnUcpuUpdate(row, owner->Optional<uint32_t>(cell));
          return;
        default:
          owner->status = base::ErrStatus("Unsupported table update");
          return;
      }
    }
  };
  ThreadStateAdapter thread_states;
  struct CounterAdapter : Adapter {
    CounterAdapter(OutputState* owner, CounterSink* sink)
        : Adapter(owner, sink), typed(sink) {}
    CounterSink* typed;
    using C = tables::CounterTable::ColumnIndex;
    void OnInsert(uint32_t row,
                  const dataframe::Dataframe::SinkValue* cells,
                  uint32_t count) override {
      if (!owner->status.ok())
        return;
      PERFETTO_CHECK(count == 5);
      CounterSink::Row value;
      value.ts = owner->Get<int64_t>(cells[C::ts]);
      value.track_id = owner->Get<uint32_t>(cells[C::track_id]);
      value.value = owner->Get<double>(cells[C::value]);
      value.arg_set_id = owner->Optional<uint32_t>(cells[C::arg_set_id]);
      owner->status = typed->OnInsert(row, value);
    }
    void OnUpdate(uint32_t row,
                  uint32_t col,
                  const dataframe::Dataframe::SinkValue& cell) override {
      if (!owner->status.ok())
        return;
      switch (col) {
        case C::ts:
          owner->status = typed->OnTsUpdate(row, owner->Get<int64_t>(cell));
          return;
        case C::track_id:
          owner->status =
              typed->OnTrackIdUpdate(row, owner->Get<uint32_t>(cell));
          return;
        case C::value:
          owner->status = typed->OnValueUpdate(row, owner->Get<double>(cell));
          return;
        case C::arg_set_id:
          owner->status =
              typed->OnArgSetIdUpdate(row, owner->Optional<uint32_t>(cell));
          return;
        default:
          owner->status = base::ErrStatus("Unsupported counter update");
          return;
      }
    }
  } counters{this, sinks.counters};
  dataframe::Dataframe::Sink* GetSink(const char* name) {
    if (sinks.slice && std::string_view(name) == tables::SliceTable::Name())
      return &slice;
    if (sinks.args && std::string_view(name) == tables::ArgTable::Name())
      return &args;
    if (sinks.sched &&
        std::string_view(name) == tables::SchedSliceTable::Name())
      return &sched;
    if (sinks.thread_states &&
        std::string_view(name) == tables::ThreadStateTable::Name())
      return &thread_states;
    if (sinks.counters &&
        std::string_view(name) == tables::CounterTable::Name())
      return &counters;
    return nullptr;
  }
  base::Status EndOfTrace() {
    if (status.ok() && sinks.slice)
      status = sinks.slice->OnEndOfTrace();
    if (status.ok() && sinks.args)
      status = sinks.args->OnEndOfTrace();
    if (status.ok() && sinks.sched)
      status = sinks.sched->OnEndOfTrace();
    if (status.ok() && sinks.thread_states)
      status = sinks.thread_states->OnEndOfTrace();
    if (status.ok() && sinks.counters)
      status = sinks.counters->OnEndOfTrace();
    return status;
  }
};

TraceParserOptions TraceParserImpl::MakeOptions(const TraceParserConfig& cfg,
                                                OutputState* output) {
  TraceParserOptions options;
  options.parsing_config.sorting_mode = cfg.sorting_mode;
  options.parsing_config.drop_ftrace_data_before = cfg.drop_ftrace_data_before;
  options.parsing_config.soft_drop_ftrace_data_before =
      cfg.soft_drop_ftrace_data_before;
  options.parsing_config.drop_track_event_data_before =
      cfg.drop_track_event_data_before;
  options.parsing_config.extra_parsing_descriptors =
      cfg.extra_parsing_descriptors;
  options.parsing_config.ingest_ftrace_in_raw_table = false;
  options.experimental_slice_args_streaming = true;
  options.counter_sink_only = true;
  options.on_trace_storage_created = [output](TraceStorage* storage) {
    output->storage = storage;
    storage->SetTableSinks(
        [output](const char* name) { return output->GetSink(name); }, true);
  };
  return options;
}

TraceParserImpl::TraceParserImpl(const TraceParserConfig& config, Sinks sinks)
    : output_(std::make_unique<OutputState>(sinks)),
      core_(MakeOptions(config, output_.get())) {
  InitializeImporters();
  // Proto uses the audited ftrace scheduling policy. JSON systemTraceEvents
  // can use a different scheduling producer, so retain those cells for now.
  context()->parser_config.on_trace_type_detected = [this](const char* name) {
    if (std::string_view(name) == "proto") {
      context()->parser_config.experimental_ftrace_sched_frontier = true;
      context()->storage->EnableFtraceStreaming();
    }
  };
  CheckOutputStatus();
}

TraceParserImpl::TraceParserImpl(const TraceParserOptions& config)
    : core_(config) {
  PERFETTO_CHECK(config.streaming_frontier_batch_rows > 0);
  PERFETTO_CHECK(config.drop_unread_table_columns ||
                 (!config.experimental_ftrace_sched_frontier &&
                  !config.experimental_slice_args_streaming));
  InitializeImporters();
}
void TraceParserImpl::InitializeImporters() {
  context()->reader_registry->Register(CreateJsonImporter());
  context()->register_additional_proto_modules =
      [](ProtoImporterModuleContext* modules, TraceProcessorContext* context) {
        auto module = std::make_unique<FtraceModuleImpl>(modules, context);
        modules->ftrace_module = module.get();
        modules->modules.emplace_back(std::move(module));
      };
}
TraceParserImpl::~TraceParserImpl() = default;
base::Status TraceParserImpl::Parse(TraceBlobView blob) {
  if (!status_.ok())
    return status_;
  if (finished_)
    return base::ErrStatus("Parse() called after NotifyEndOfFile()");
  status_ = core_.Parse(std::move(blob));
  return CheckOutputStatus();
}
base::Status TraceParserImpl::NotifyEndOfFile() {
  if (!status_.ok())
    return status_;
  if (finished_)
    return base::ErrStatus("NotifyEndOfFile() called more than once");
  finished_ = true;
  status_ = core_.NotifyEndOfFile();
  CheckOutputStatus();
  if (status_.ok() && output_)
    status_ = output_->EndOfTrace();
  return status_;
}
base::Status TraceParserImpl::CheckOutputStatus() {
  if (status_.ok() && output_ && !output_->status.ok())
    status_ = output_->status;
  return status_;
}
}  // namespace perfetto::trace_processor
