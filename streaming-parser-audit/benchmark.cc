// Parser-only performance probe: no TraceProcessorImpl or SQL engine instance.
#include <sys/resource.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/metadata_tables_py.h"
#include "src/trace_processor/tables/sched_tables_py.h"
#include "src/trace_processor/tables/slice_tables_py.h"
#include "src/trace_processor/trace_parser_impl.h"

using namespace perfetto::trace_processor;
namespace base = perfetto::base;
class ChecksumSink : public dataframe::Dataframe::Sink {
 public:
  uint64_t hash = 1469598103934665603ull, inserts = 0, updates = 0;
  void Mix(uint64_t value) { hash = (hash ^ value) * 1099511628211ull; }
  void Cell(const dataframe::Dataframe::SinkValue& value) {
    Mix(value.index());
    std::visit(
        [&](auto cell) {
          using T = decltype(cell);
          if constexpr (std::is_same_v<T, StringPool::Id>) {
            Mix(cell.raw_id());
          } else if constexpr (!std::is_same_v<T, std::monostate>) {
            uint64_t bits = 0;
            memcpy(&bits, &cell, sizeof(cell));
            Mix(bits);
          }
        },
        value);
  }
  void OnInsert(uint32_t row,
                const dataframe::Dataframe::SinkValue* values,
                uint32_t count) override {
    ++inserts;
    Mix(row);
    Mix(count);
    for (uint32_t i = 0; i < count; ++i)
      Cell(values[i]);
  }
  void OnUpdate(uint32_t row,
                uint32_t col,
                const dataframe::Dataframe::SinkValue& value) override {
    ++updates;
    Mix(row);
    Mix(col);
    Cell(value);
  }
};
struct ChecksumOutput {
  std::map<std::string, std::unique_ptr<ChecksumSink>>& sinks;
  explicit ChecksumOutput(
      std::map<std::string, std::unique_ptr<ChecksumSink>>& sinks)
      : sinks(sinks) {
    sinks[tables::CounterTable::Name()] = std::make_unique<ChecksumSink>();
    sinks[tables::SliceTable::Name()] = std::make_unique<ChecksumSink>();
    sinks[tables::ArgTable::Name()] = std::make_unique<ChecksumSink>();
    sinks[tables::SchedSliceTable::Name()] = std::make_unique<ChecksumSink>();
    sinks[tables::ThreadStateTable::Name()] = std::make_unique<ChecksumSink>();
  }
  dataframe::Dataframe::SinkValue Cell(TraceParserString v) {
    return StringPool::Id::Raw(v.id);
  }
  template <typename T>
  dataframe::Dataframe::SinkValue Cell(T v) {
    return v;
  }
  template <typename T>
  dataframe::Dataframe::SinkValue Cell(std::optional<T> v) {
    return v ? Cell(*v) : dataframe::Dataframe::SinkValue{std::monostate{}};
  }
  struct Slice : SliceSink {
    explicit Slice(ChecksumOutput* owner) : owner(owner) {}
    ChecksumOutput* owner;
    using C = tables::SliceTable::ColumnIndex;
    base::Status OnInsert(uint32_t id, const Row& row) override {
      dataframe::Dataframe::SinkValue cells[] = {
          id,
          owner->Cell(row.ts),
          owner->Cell(row.dur),
          owner->Cell(row.track_id),
          owner->Cell(row.category),
          owner->Cell(row.name),
          owner->Cell(row.depth),
          owner->Cell(row.parent_id),
          owner->Cell(row.arg_set_id),
          owner->Cell(row.thread_ts),
          owner->Cell(row.thread_dur),
          owner->Cell(row.thread_instruction_count),
          owner->Cell(row.thread_instruction_delta)};
      owner->sinks.at(tables::SliceTable::Name())->OnInsert(id, cells, 13);
      return base::OkStatus();
    }
    base::Status OnTsUpdate(uint32_t id, int64_t value) override {
      owner->sinks.at(tables::SliceTable::Name())
          ->OnUpdate(id, C::ts, owner->Cell(value));
      return base::OkStatus();
    }
    base::Status OnDurUpdate(uint32_t id, int64_t value) override {
      owner->sinks.at(tables::SliceTable::Name())
          ->OnUpdate(id, C::dur, owner->Cell(value));
      return base::OkStatus();
    }
    base::Status OnTrackIdUpdate(uint32_t id, uint32_t value) override {
      owner->sinks.at(tables::SliceTable::Name())
          ->OnUpdate(id, C::track_id, owner->Cell(value));
      return base::OkStatus();
    }
    base::Status OnCategoryUpdate(
        uint32_t id,
        std::optional<TraceParserString> value) override {
      owner->sinks.at(tables::SliceTable::Name())
          ->OnUpdate(id, C::category, owner->Cell(value));
      return base::OkStatus();
    }
    base::Status OnNameUpdate(uint32_t id,
                              std::optional<TraceParserString> value) override {
      owner->sinks.at(tables::SliceTable::Name())
          ->OnUpdate(id, C::name, owner->Cell(value));
      return base::OkStatus();
    }
    base::Status OnDepthUpdate(uint32_t id, uint32_t value) override {
      owner->sinks.at(tables::SliceTable::Name())
          ->OnUpdate(id, C::depth, owner->Cell(value));
      return base::OkStatus();
    }
    base::Status OnParentIdUpdate(uint32_t id,
                                  std::optional<uint32_t> value) override {
      owner->sinks.at(tables::SliceTable::Name())
          ->OnUpdate(id, C::parent_id, owner->Cell(value));
      return base::OkStatus();
    }
    base::Status OnArgSetIdUpdate(uint32_t id,
                                  std::optional<uint32_t> value) override {
      owner->sinks.at(tables::SliceTable::Name())
          ->OnUpdate(id, C::arg_set_id, owner->Cell(value));
      return base::OkStatus();
    }
    base::Status OnThreadTsUpdate(uint32_t id,
                                  std::optional<int64_t> value) override {
      owner->sinks.at(tables::SliceTable::Name())
          ->OnUpdate(id, C::thread_ts, owner->Cell(value));
      return base::OkStatus();
    }
    base::Status OnThreadDurUpdate(uint32_t id,
                                   std::optional<int64_t> value) override {
      owner->sinks.at(tables::SliceTable::Name())
          ->OnUpdate(id, C::thread_dur, owner->Cell(value));
      return base::OkStatus();
    }
    base::Status OnThreadInstructionCountUpdate(
        uint32_t id,
        std::optional<int64_t> value) override {
      owner->sinks.at(tables::SliceTable::Name())
          ->OnUpdate(id, C::thread_instruction_count, owner->Cell(value));
      return base::OkStatus();
    }
    base::Status OnThreadInstructionDeltaUpdate(
        uint32_t id,
        std::optional<int64_t> value) override {
      owner->sinks.at(tables::SliceTable::Name())
          ->OnUpdate(id, C::thread_instruction_delta, owner->Cell(value));
      return base::OkStatus();
    }
  } slice{this};
  struct Arg : ArgSink {
    explicit Arg(ChecksumOutput* owner) : owner(owner) {}
    ChecksumOutput* owner;
    using C = tables::ArgTable::ColumnIndex;
    base::Status OnInsert(uint32_t id, const Row& row) override {
      dataframe::Dataframe::SinkValue cells[] = {id,
                                                 owner->Cell(row.arg_set_id),
                                                 owner->Cell(row.flat_key),
                                                 owner->Cell(row.key),
                                                 owner->Cell(row.int_value),
                                                 owner->Cell(row.string_value),
                                                 owner->Cell(row.real_value),
                                                 owner->Cell(row.value_type)};
      owner->sinks.at(tables::ArgTable::Name())->OnInsert(id, cells, 8);
      return base::OkStatus();
    }
    base::Status OnArgSetIdUpdate(uint32_t id, uint32_t value) override {
      owner->sinks.at(tables::ArgTable::Name())
          ->OnUpdate(id, C::arg_set_id, owner->Cell(value));
      return base::OkStatus();
    }
    base::Status OnFlatKeyUpdate(uint32_t id,
                                 TraceParserString value) override {
      owner->sinks.at(tables::ArgTable::Name())
          ->OnUpdate(id, C::flat_key, owner->Cell(value));
      return base::OkStatus();
    }
    base::Status OnKeyUpdate(uint32_t id, TraceParserString value) override {
      owner->sinks.at(tables::ArgTable::Name())
          ->OnUpdate(id, C::key, owner->Cell(value));
      return base::OkStatus();
    }
    base::Status OnIntValueUpdate(uint32_t id,
                                  std::optional<int64_t> value) override {
      owner->sinks.at(tables::ArgTable::Name())
          ->OnUpdate(id, C::int_value, owner->Cell(value));
      return base::OkStatus();
    }
    base::Status OnStringValueUpdate(
        uint32_t id,
        std::optional<TraceParserString> value) override {
      owner->sinks.at(tables::ArgTable::Name())
          ->OnUpdate(id, C::string_value, owner->Cell(value));
      return base::OkStatus();
    }
    base::Status OnRealValueUpdate(uint32_t id,
                                   std::optional<double> value) override {
      owner->sinks.at(tables::ArgTable::Name())
          ->OnUpdate(id, C::real_value, owner->Cell(value));
      return base::OkStatus();
    }
    base::Status OnValueTypeUpdate(uint32_t id,
                                   TraceParserString value) override {
      owner->sinks.at(tables::ArgTable::Name())
          ->OnUpdate(id, C::value_type, owner->Cell(value));
      return base::OkStatus();
    }
  } args{this};
  struct Sched : SchedSink {
    explicit Sched(ChecksumOutput* owner) : owner(owner) {}
    ChecksumOutput* owner;
    using C = tables::SchedSliceTable::ColumnIndex;
    base::Status OnInsert(uint32_t id, const Row& row) override {
      dataframe::Dataframe::SinkValue cells[] = {id,
                                                 owner->Cell(row.ts),
                                                 owner->Cell(row.dur),
                                                 owner->Cell(row.utid),
                                                 owner->Cell(row.end_state),
                                                 owner->Cell(row.priority),
                                                 owner->Cell(row.ucpu)};
      owner->sinks.at(tables::SchedSliceTable::Name())->OnInsert(id, cells, 7);
      return base::OkStatus();
    }
    base::Status OnTsUpdate(uint32_t id, int64_t value) override {
      owner->sinks.at(tables::SchedSliceTable::Name())
          ->OnUpdate(id, C::ts, owner->Cell(value));
      return base::OkStatus();
    }
    base::Status OnDurUpdate(uint32_t id, int64_t value) override {
      owner->sinks.at(tables::SchedSliceTable::Name())
          ->OnUpdate(id, C::dur, owner->Cell(value));
      return base::OkStatus();
    }
    base::Status OnUtidUpdate(uint32_t id, uint32_t value) override {
      owner->sinks.at(tables::SchedSliceTable::Name())
          ->OnUpdate(id, C::utid, owner->Cell(value));
      return base::OkStatus();
    }
    base::Status OnEndStateUpdate(uint32_t id,
                                  TraceParserString value) override {
      owner->sinks.at(tables::SchedSliceTable::Name())
          ->OnUpdate(id, C::end_state, owner->Cell(value));
      return base::OkStatus();
    }
    base::Status OnPriorityUpdate(uint32_t id, int32_t value) override {
      owner->sinks.at(tables::SchedSliceTable::Name())
          ->OnUpdate(id, C::priority, owner->Cell(value));
      return base::OkStatus();
    }
    base::Status OnUcpuUpdate(uint32_t id, uint32_t value) override {
      owner->sinks.at(tables::SchedSliceTable::Name())
          ->OnUpdate(id, C::ucpu, owner->Cell(value));
      return base::OkStatus();
    }
  } sched{this};
  struct ThreadState : ThreadStateSink {
    explicit ThreadState(ChecksumOutput* owner) : owner(owner) {}
    ChecksumOutput* owner;
    using C = tables::ThreadStateTable::ColumnIndex;
    base::Status OnInsert(uint32_t id, const Row& row) override {
      dataframe::Dataframe::SinkValue cells[] = {
          id,
          owner->Cell(row.ts),
          owner->Cell(row.dur),
          owner->Cell(row.utid),
          owner->Cell(row.state),
          owner->Cell(row.io_wait),
          owner->Cell(row.blocked_function),
          owner->Cell(row.waker_utid),
          owner->Cell(row.waker_id),
          owner->Cell(row.irq_context),
          owner->Cell(row.ucpu)};
      owner->sinks.at(tables::ThreadStateTable::Name())
          ->OnInsert(id, cells, 11);
      return base::OkStatus();
    }
    base::Status OnTsUpdate(uint32_t id, int64_t value) override {
      owner->sinks.at(tables::ThreadStateTable::Name())
          ->OnUpdate(id, C::ts, owner->Cell(value));
      return base::OkStatus();
    }
    base::Status OnDurUpdate(uint32_t id, int64_t value) override {
      owner->sinks.at(tables::ThreadStateTable::Name())
          ->OnUpdate(id, C::dur, owner->Cell(value));
      return base::OkStatus();
    }
    base::Status OnUtidUpdate(uint32_t id, uint32_t value) override {
      owner->sinks.at(tables::ThreadStateTable::Name())
          ->OnUpdate(id, C::utid, owner->Cell(value));
      return base::OkStatus();
    }
    base::Status OnStateUpdate(uint32_t id, TraceParserString value) override {
      owner->sinks.at(tables::ThreadStateTable::Name())
          ->OnUpdate(id, C::state, owner->Cell(value));
      return base::OkStatus();
    }
    base::Status OnIoWaitUpdate(uint32_t id,
                                std::optional<uint32_t> value) override {
      owner->sinks.at(tables::ThreadStateTable::Name())
          ->OnUpdate(id, C::io_wait, owner->Cell(value));
      return base::OkStatus();
    }
    base::Status OnBlockedFunctionUpdate(
        uint32_t id,
        std::optional<TraceParserString> value) override {
      owner->sinks.at(tables::ThreadStateTable::Name())
          ->OnUpdate(id, C::blocked_function, owner->Cell(value));
      return base::OkStatus();
    }
    base::Status OnWakerUtidUpdate(uint32_t id,
                                   std::optional<uint32_t> value) override {
      owner->sinks.at(tables::ThreadStateTable::Name())
          ->OnUpdate(id, C::waker_utid, owner->Cell(value));
      return base::OkStatus();
    }
    base::Status OnWakerIdUpdate(uint32_t id,
                                 std::optional<uint32_t> value) override {
      owner->sinks.at(tables::ThreadStateTable::Name())
          ->OnUpdate(id, C::waker_id, owner->Cell(value));
      return base::OkStatus();
    }
    base::Status OnIrqContextUpdate(uint32_t id,
                                    std::optional<uint32_t> value) override {
      owner->sinks.at(tables::ThreadStateTable::Name())
          ->OnUpdate(id, C::irq_context, owner->Cell(value));
      return base::OkStatus();
    }
    base::Status OnUcpuUpdate(uint32_t id,
                              std::optional<uint32_t> value) override {
      owner->sinks.at(tables::ThreadStateTable::Name())
          ->OnUpdate(id, C::ucpu, owner->Cell(value));
      return base::OkStatus();
    }
  } thread_states{this};
  struct Counter : CounterSink {
    explicit Counter(ChecksumOutput* owner) : owner(owner) {}
    ChecksumOutput* owner;
    using C = tables::CounterTable::ColumnIndex;
    base::Status OnInsert(uint32_t id, const Row& row) override {
      dataframe::Dataframe::SinkValue cells[] = {
          id, row.ts, row.track_id, row.value, owner->Cell(row.arg_set_id)};
      owner->sinks.at(tables::CounterTable::Name())->OnInsert(id, cells, 5);
      return base::OkStatus();
    }
    base::Status OnTsUpdate(uint32_t id, int64_t value) override {
      owner->sinks.at(tables::CounterTable::Name())
          ->OnUpdate(id, C::ts, owner->Cell(value));
      return base::OkStatus();
    }
    base::Status OnTrackIdUpdate(uint32_t id, uint32_t value) override {
      owner->sinks.at(tables::CounterTable::Name())
          ->OnUpdate(id, C::track_id, owner->Cell(value));
      return base::OkStatus();
    }
    base::Status OnValueUpdate(uint32_t id, double value) override {
      owner->sinks.at(tables::CounterTable::Name())
          ->OnUpdate(id, C::value, owner->Cell(value));
      return base::OkStatus();
    }
    base::Status OnArgSetIdUpdate(uint32_t id,
                                  std::optional<uint32_t> value) override {
      owner->sinks.at(tables::CounterTable::Name())
          ->OnUpdate(id, C::arg_set_id, owner->Cell(value));
      return base::OkStatus();
    }
  } counters{this};
  TraceParser::Sinks output() {
    return {&slice, &args, &sched, &thread_states, &counters};
  }
};
int main(int argc, char** argv) {
  if (argc != 3)
    return 2;
  std::string mode = argv[1];
  bool sink_enabled = mode != "full";
  bool drop = mode == "drop" || mode == "frontier" || mode == "slice";
  TraceParserOptions config;
  TraceStorage* storage = nullptr;
  config.drop_unread_table_columns = drop;
  config.experimental_ftrace_sched_frontier = mode == "frontier";
  config.experimental_slice_args_streaming = mode == "slice";
  config.parsing_config.ingest_ftrace_in_raw_table = false;
  config.parsing_config.drop_ftrace_data_before = DropFtraceDataBefore::kNoDrop;
  config.parsing_config.soft_drop_ftrace_data_before =
      SoftDropFtraceDataBefore::kNoDrop;
  std::map<std::string, std::unique_ptr<ChecksumSink>> sinks;
  config.on_trace_storage_created = [&](TraceStorage* output_storage) {
    storage = output_storage;
    if (sink_enabled) {
      output_storage->SetTableSinks(
          [&](const char* name) -> dataframe::Dataframe::Sink* {
            if (std::string_view(name) != tables::SliceTable::Name() &&
                std::string_view(name) != tables::ArgTable::Name() &&
                std::string_view(name) != tables::SchedSliceTable::Name() &&
                std::string_view(name) != tables::ThreadStateTable::Name() &&
                std::string_view(name) != tables::CounterTable::Name())
              return nullptr;
            auto& sink = sinks[name];
            sink = std::make_unique<ChecksumSink>();
            return sink.get();
          },
          drop);
    }
  };
  std::unique_ptr<ChecksumOutput> output;
  std::unique_ptr<TraceParser> tp;
  if (mode == "frontier" || mode == "slice") {
    output = std::make_unique<ChecksumOutput>(sinks);
    TraceParser::Config public_config;
    public_config.drop_ftrace_data_before = DropFtraceDataBefore::kNoDrop;
    public_config.soft_drop_ftrace_data_before =
        SoftDropFtraceDataBefore::kNoDrop;
    tp = TraceParser::CreateInstance(public_config, output->output());
    // Internal diagnostics only: no storage accessor in the public API.
    storage = static_cast<TraceParserImpl*>(tp.get())->context()->storage.get();
  } else {
    // Retention controls exist only inside this benchmark's baseline setup.
    tp = std::make_unique<TraceParserImpl>(config);
  }
  std::ifstream input(argv[2], std::ios::binary);
  if (!input)
    return 3;
  std::vector<char> buffer(1024 * 1024);
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    size_t count = static_cast<size_t>(input.gcount());
    if (!count)
      break;
    auto status =
        tp->Parse(TraceBlobView(TraceBlob::CopyFrom(buffer.data(), count)));
    if (!status.ok()) {
      fprintf(stderr, "%s\n", status.message().c_str());
      return 4;
    }
  }
  auto status = tp->NotifyEndOfFile();
  if (!status.ok()) {
    fprintf(stderr, "%s\n", status.message().c_str());
    return 5;
  }
  auto& sched = storage->sched_slice_table();
  auto& states = storage->thread_state_table();
  auto& slices = storage->slice_table();
  auto& args = storage->arg_table();
  uint64_t hash = 0, inserts = 0, updates = 0;
  for (const auto& entry : sinks) {
    hash ^= entry.second->hash;
    inserts += entry.second->inserts;
    updates += entry.second->updates;
  }
  rusage usage{};
  getrusage(RUSAGE_SELF, &usage);
  double cpu = usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6 +
               usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6;
  printf(
      "{\"mode\":\"%s\",\"cpu_s\":%.6f,\"peak_rss_bytes\":%ld,"
      "\"sched_rows\":%u,\"sched_frontier\":%u,\"completion_frontier\":%u,"
      "\"thread_state_rows\":%u,\"thread_state_frontier\":%u,"
      "\"thread_state_ts_capacity_bytes\":%llu,"
      "\"slice_rows\":%u,\"slice_frontier\":%u,\"arg_rows\":%u,\"arg_"
      "frontier\":%u,\"inserts\":%llu,"
      "\"counter_rows\":%u,\"counter_frontier\":%u,\"updates\":%llu,"
      "\"checksum\":\"%llx\"}\n",
      mode.c_str(), cpu, usage.ru_maxrss, sched.row_count(),
      sched.dataframe().first_retained_row(),
      sched.dataframe().completion_frontier(), states.row_count(),
      states.dataframe().first_retained_row(),
      (unsigned long long)(states.dataframe()
                               .column(
                                   tables::ThreadStateTable::ColumnIndex::ts)
                               .storage.unchecked_get<dataframe::Int64>()
                               .capacity() *
                           sizeof(int64_t)),
      slices.row_count(), slices.dataframe().first_retained_row(),
      args.row_count(), args.dataframe().first_retained_row(),
      (unsigned long long)inserts, storage->counter_table().row_count(),
      storage->counter_table().dataframe().first_retained_row(),
      (unsigned long long)updates, (unsigned long long)hash);
}
