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

#include "src/trace_processor/importers/proto/proto_trace_parser_impl.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/core/dataframe/specs.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/args_translation_table.h"
#include "src/trace_processor/importers/common/clock_tracker.h"
#include "src/trace_processor/importers/common/cpu_tracker.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/flow_tracker.h"
#include "src/trace_processor/importers/common/global_metadata_tracker.h"
#include "src/trace_processor/importers/common/global_stats_tracker.h"
#include "src/trace_processor/importers/common/gpu_tracker.h"
#include "src/trace_processor/importers/common/import_logs_tracker.h"
#include "src/trace_processor/importers/common/machine_tracker.h"
#include "src/trace_processor/importers/common/mapping_tracker.h"
#include "src/trace_processor/importers/common/metadata_tracker.h"
#include "src/trace_processor/importers/common/process_track_translation_table.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/profiler_sample_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/slice_translation_table.h"
#include "src/trace_processor/importers/common/stack_profile_tracker.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/importers/common/trace_diagnostics_tracker.h"
#include "src/trace_processor/importers/common/track_compressor.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/importers/ftrace/ftrace_sched_event_tracker.h"
#include "src/trace_processor/importers/proto/additional_modules.h"
#include "src/trace_processor/importers/proto/heap_graph_tracker.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/importers/proto/proto_trace_reader.h"
#include "src/trace_processor/sorter/trace_sorter.h"
#include "src/trace_processor/storage/metadata.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/log_tables_py.h"
#include "src/trace_processor/tables/metadata_tables_py.h"
#include "src/trace_processor/tables/slice_tables_py.h"
#include "src/trace_processor/tables/track_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/types/trace_processor_context_ptr.h"
#include "src/trace_processor/types/variadic.h"
#include "src/trace_processor/util/args_utils.h"
#include "src/trace_processor/util/clock_synchronizer.h"
#include "src/trace_processor/util/descriptors.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/common/builtin_clock.pbzero.h"
#include "protos/perfetto/common/perf_events.pbzero.h"
#include "protos/perfetto/common/sys_stats_counters.pbzero.h"
#include "protos/perfetto/common/system_info.pbzero.h"
#include "protos/perfetto/common/trace_attributes.pbzero.h"
#include "protos/perfetto/config/trace_config.pbzero.h"
#include "protos/perfetto/trace/android/packages_list.pbzero.h"
#include "protos/perfetto/trace/android/recovered_trace_info.pbzero.h"
#include "protos/perfetto/trace/chrome/chrome_benchmark_metadata.pbzero.h"
#include "protos/perfetto/trace/chrome/chrome_trace_event.pbzero.h"
#include "protos/perfetto/trace/clock_snapshot.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "protos/perfetto/trace/ftrace/generic.pbzero.h"
#include "protos/perfetto/trace/ftrace/power.pbzero.h"
#include "protos/perfetto/trace/ftrace/sched.pbzero.h"
#include "protos/perfetto/trace/ftrace/task.pbzero.h"
#include "protos/perfetto/trace/interned_data/interned_data.pbzero.h"
#include "protos/perfetto/trace/profiling/profile_common.pbzero.h"
#include "protos/perfetto/trace/profiling/profile_packet.pbzero.h"
#include "protos/perfetto/trace/ps/process_tree.pbzero.h"
#include "protos/perfetto/trace/sys_stats/sys_stats.pbzero.h"
#include "protos/perfetto/trace/system_info/cpu_info.pbzero.h"
#include "protos/perfetto/trace/trace.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "protos/perfetto/trace/trace_uuid.pbzero.h"
#include "protos/perfetto/trace/track_event/chrome_thread_descriptor.pbzero.h"
#include "protos/perfetto/trace/track_event/counter_descriptor.pbzero.h"
#include "protos/perfetto/trace/track_event/debug_annotation.pbzero.h"
#include "protos/perfetto/trace/track_event/log_message.pbzero.h"
#include "protos/perfetto/trace/track_event/process_descriptor.pbzero.h"
#include "protos/perfetto/trace/track_event/source_location.pbzero.h"
#include "protos/perfetto/trace/track_event/task_execution.pbzero.h"
#include "protos/perfetto/trace/track_event/thread_descriptor.pbzero.h"
#include "protos/perfetto/trace/track_event/track_descriptor.pbzero.h"
#include "protos/perfetto/trace/track_event/track_event.pbzero.h"
#include "protos/third_party/chromium/chrome_enums.pbzero.h"

namespace perfetto::trace_processor {
namespace {

using ::std::make_pair;
using ::testing::_;
using ::testing::Args;
using ::testing::AtLeast;
using ::testing::DoAll;
using ::testing::ElementsAreArray;
using ::testing::Eq;
using ::testing::HasSubstr;
using ::testing::IgnoreResult;
using ::testing::InSequence;
using ::testing::InvokeArgument;
using ::testing::NiceMock;
using ::testing::Pointwise;
using ::testing::Return;
using ::testing::ReturnRef;
using ::testing::UnorderedElementsAreArray;

namespace {
MATCHER_P(DoubleEq, exp, "Double matcher that satisfies -Wfloat-equal") {
  // The IEEE standard says that any comparison operation involving
  // a NAN must return false.
  double d_exp = exp;
  double d_arg = arg;
  if (std::isnan(d_exp) || std::isnan(d_arg))
    return false;
  return fabs(d_arg - d_exp) < 1e-128;
}
}  // namespace

class MockSchedEventTracker : public FtraceSchedEventTracker {
 public:
  explicit MockSchedEventTracker(TraceProcessorContext* context)
      : FtraceSchedEventTracker(context) {}

  MOCK_METHOD(void,
              PushSchedSwitch,
              (uint32_t cpu,
               int64_t timestamp,
               int64_t prev_pid,
               base::StringView prev_comm,
               int32_t prev_prio,
               int64_t prev_state,
               int64_t next_pid,
               base::StringView next_comm,
               int32_t next_prio),
              (override));
};

class MockEventTracker : public EventTracker {
 public:
  explicit MockEventTracker(TraceProcessorContext* context)
      : EventTracker(context) {}
  ~MockEventTracker() override = default;

  MOCK_METHOD(void,
              PushSchedSwitch,
              (uint32_t cpu,
               int64_t timestamp,
               uint32_t prev_pid,
               base::StringView prev_comm,
               int32_t prev_prio,
               int64_t prev_state,
               uint32_t next_pid,
               base::StringView next_comm,
               int32_t next_prio));

  MOCK_METHOD(std::optional<CounterId>,
              PushCounter,
              (int64_t timestamp, double value, TrackId track_id),
              (override));
};

class MockProcessTracker : public ProcessTracker {
 public:
  explicit MockProcessTracker(TraceProcessorContext* context)
      : ProcessTracker(context) {}

  MOCK_METHOD(UniquePid,
              StartNewProcess,
              (std::optional<int64_t> timestamp,
               std::optional<UniquePid> parent_upid,
               int64_t pid,
               StringId process_name,
               ThreadNamePriority priority),
              (override));

  MOCK_METHOD(void,
              SetProcessParent,
              (UniquePid upid,
               UniquePid pupid,
               std::optional<int64_t> timestamp),
              (override));

  MOCK_METHOD(void,
              SetProcessMetadata,
              (UniquePid upid,
               base::StringView process_name,
               base::StringView cmdline),
              (override));

  MOCK_METHOD(void,
              UpdateThreadName,
              (UniqueTid utid,
               StringId thread_name_id,
               ThreadNamePriority priority),
              (override));
  MOCK_METHOD(UniqueTid, UpdateThread, (int64_t tid, int64_t tgid), (override));

  MOCK_METHOD(UniquePid, GetOrCreateProcess, (int64_t pid), (override));

  MOCK_METHOD(void,
              UpdateProcessName,
              (UniquePid upid,
               StringId process_name_id,
               ProcessNamePriority priority),
              (override));
};

class ProtoTraceParserTest : public ::testing::Test {
 public:
  ProtoTraceParserTest() {
    context_.register_additional_proto_modules = &RegisterAdditionalModules;
    storage_ = new TraceStorage();
    context_.storage.reset(storage_);
    context_.machine_tracker.reset(
        new MachineTracker(&context_, kDefaultMachineId));
    context_.track_tracker = std::make_unique<TrackTracker>(&context_);
    context_.args_tracker =
        std::make_unique<ArgsTracker>(context_.storage.get());
    context_.global_metadata_tracker =
        std::make_unique<GlobalMetadataTracker>(context_.storage.get());
    context_.global_stats_tracker =
        std::make_unique<GlobalStatsTracker>(context_.storage.get());
    context_.import_logs_tracker =
        std::make_unique<ImportLogsTracker>(&context_, TraceId(1));
    context_.mapping_tracker.reset(new MappingTracker(&context_));
    context_.trace_state =
        TraceProcessorContextPtr<TraceProcessorContext::TraceState>::MakeRoot(
            TraceProcessorContext::TraceState{TraceId(0)});
    context_.stack_profile_tracker =
        std::make_unique<StackProfileTracker>(&context_);
    context_.args_translation_table.reset(new ArgsTranslationTable(storage_));
    context_.metadata_tracker.reset(new MetadataTracker(&context_));
    context_.cpu_tracker.reset(new CpuTracker(&context_));
    context_.gpu_tracker.reset(new GpuTracker(&context_));
    event_ = new MockEventTracker(&context_);
    context_.event_tracker.reset(event_);
    sched_ = new MockSchedEventTracker(&context_);
    context_.ftrace_sched_tracker.reset(sched_);
    process_ = new NiceMock<MockProcessTracker>(&context_);
    context_.process_tracker.reset(process_);
    context_.process_track_translation_table.reset(
        new ProcessTrackTranslationTable(storage_));
    context_.slice_tracker = std::make_unique<SliceTracker>(&context_);
    context_.slice_translation_table =
        std::make_unique<SliceTranslationTable>(storage_);
    context_.trace_time_state = std::make_unique<TraceTimeState>(
        ClockId::Machine(protos::pbzero::BUILTIN_CLOCK_BOOTTIME));
    primary_sync_ = std::make_unique<ClockSynchronizer>(
        context_.trace_time_state.get(),
        std::make_unique<ClockSynchronizerListenerImpl>(&context_));
    context_.clock_tracker = std::make_unique<ClockTracker>(
        &context_, primary_sync_.get(), /*is_primary=*/true);
    context_.stats_tracker = std::make_unique<StatsTracker>(&context_);
    context_.profiler_sample_tracker =
        std::make_unique<ProfilerSampleTracker>(&context_);
    context_.trace_diagnostics_tracker =
        std::make_unique<TraceDiagnosticsTracker>(&context_);
    context_.flow_tracker = std::make_unique<FlowTracker>(&context_);
    context_.sorter = std::make_unique<TraceSorter>(
        &context_, TraceSorter::SortingMode::kFullSort);
    context_.descriptor_pool_ = std::make_unique<DescriptorPool>();
    context_.uuid_state = std::make_unique<TraceProcessorContext::UuidState>();
    context_.heap_graph_tracker = std::make_unique<HeapGraphTracker>(
        storage_, context_.global_stats_tracker.get());
    context_.trace_diagnostics_tracker =
        std::make_unique<TraceDiagnosticsTracker>(&context_);
    context_.track_compressor.reset(new TrackCompressor(&context_));
    context_.track_group_idx_state =
        std::make_unique<TrackCompressorGroupIdxState>();

    clock_ = context_.clock_tracker.get();
    reader_ = std::make_unique<ProtoTraceReader>(&context_);
  }

  void ResetTraceBuffers() { trace_.Reset(); }

  void SetUp() override { ResetTraceBuffers(); }

  base::Status Tokenize() {
    trace_->Finalize();
    std::vector<uint8_t> trace_bytes = trace_.SerializeAsArray();
    std::unique_ptr<uint8_t[]> raw_trace(new uint8_t[trace_bytes.size()]);
    memcpy(raw_trace.get(), trace_bytes.data(), trace_bytes.size());

    auto status = reader_->Parse(TraceBlobView(
        TraceBlob::TakeOwnership(std::move(raw_trace), trace_bytes.size())));
    if (status.ok()) {
      status = reader_->OnPushDataToSorter();
    }
    if (status.ok()) {
      reader_->OnEventsFullyExtracted();
    }

    ResetTraceBuffers();
    return status;
  }

  bool HasArg(ArgSetId set_id, StringId key_id, Variadic value) {
    const auto& args = storage_->arg_table();
    auto cursor = args.CreateCursor({
        dataframe::FilterSpec{
            tables::ArgTable::ColumnIndex::arg_set_id,
            0,
            dataframe::Eq{},
            std::nullopt,
        },
        dataframe::FilterSpec{
            tables::ArgTable::ColumnIndex::key,
            1,
            dataframe::Eq{},
            std::nullopt,
        },
    });
    cursor.SetFilterValueUnchecked(0, set_id);
    cursor.SetFilterValueUnchecked(1, storage_->GetString(key_id).data());

    bool found = false;
    for (cursor.Execute(); !cursor.Eof(); cursor.Next()) {
      EXPECT_EQ(cursor.flat_key(), key_id);
      if (GetArgValue(*storage_, cursor) == value) {
        found = true;
        break;
      }
    }
    return found;
  }

 protected:
  protozero::HeapBuffered<protos::pbzero::Trace> trace_;
  TraceProcessorContext context_;
  std::unique_ptr<ClockSynchronizer> primary_sync_;
  MockEventTracker* event_;
  MockSchedEventTracker* sched_;
  MockProcessTracker* process_;
  ClockTracker* clock_;
  TraceStorage* storage_;
  std::unique_ptr<ProtoTraceReader> reader_;
};

// TODO(eseckler): Refactor these into a new file for ftrace tests.

TEST_F(ProtoTraceParserTest, LoadSingleEvent) {
  auto* bundle = trace_->add_packet()->set_ftrace_events();
  bundle->set_cpu(10);

  auto* event = bundle->add_event();
  event->set_timestamp(1000);
  event->set_pid(12);

  static const char kProc1Name[] = "proc1";
  static const char kProc2Name[] = "proc2";
  auto* sched_switch = event->set_sched_switch();
  sched_switch->set_prev_pid(10);
  sched_switch->set_prev_comm(kProc2Name);
  sched_switch->set_prev_prio(256);
  sched_switch->set_prev_state(32);
  sched_switch->set_next_comm(kProc1Name);
  sched_switch->set_next_pid(100);
  sched_switch->set_next_prio(1024);

  EXPECT_CALL(*sched_,
              PushSchedSwitch(10, 1000, 10, base::StringView(kProc2Name), 256,
                              32, 100, base::StringView(kProc1Name), 1024));
  Tokenize();
  context_.sorter->ExtractEventsForced();
}

TEST_F(ProtoTraceParserTest, LoadMultipleEvents) {
  auto* bundle = trace_->add_packet()->set_ftrace_events();
  bundle->set_cpu(10);

  auto* event = bundle->add_event();
  event->set_timestamp(1000);
  event->set_pid(12);

  static const char kProcName1[] = "proc1";
  static const char kProcName2[] = "proc2";
  auto* sched_switch = event->set_sched_switch();
  sched_switch->set_prev_pid(10);
  sched_switch->set_prev_comm(kProcName2);
  sched_switch->set_prev_prio(256);
  sched_switch->set_prev_state(32);
  sched_switch->set_next_comm(kProcName1);
  sched_switch->set_next_pid(100);
  sched_switch->set_next_prio(1024);

  event = bundle->add_event();
  event->set_timestamp(1001);
  event->set_pid(12);

  sched_switch = event->set_sched_switch();
  sched_switch->set_prev_pid(100);
  sched_switch->set_prev_comm(kProcName1);
  sched_switch->set_prev_prio(256);
  sched_switch->set_prev_state(32);
  sched_switch->set_next_comm(kProcName2);
  sched_switch->set_next_pid(10);
  sched_switch->set_next_prio(512);

  EXPECT_CALL(*sched_,
              PushSchedSwitch(10, 1000, 10, base::StringView(kProcName2), 256,
                              32, 100, base::StringView(kProcName1), 1024));

  EXPECT_CALL(*sched_,
              PushSchedSwitch(10, 1001, 100, base::StringView(kProcName1), 256,
                              32, 10, base::StringView(kProcName2), 512));

  Tokenize();
  context_.sorter->ExtractEventsForced();
}

TEST_F(ProtoTraceParserTest, LoadMultiplePackets) {
  auto* bundle = trace_->add_packet()->set_ftrace_events();
  bundle->set_cpu(10);

  auto* event = bundle->add_event();
  event->set_timestamp(1000);
  event->set_pid(12);

  static const char kProcName1[] = "proc1";
  static const char kProcName2[] = "proc2";
  auto* sched_switch = event->set_sched_switch();
  sched_switch->set_prev_pid(10);
  sched_switch->set_prev_comm(kProcName2);
  sched_switch->set_prev_prio(256);
  sched_switch->set_prev_state(32);
  sched_switch->set_next_comm(kProcName1);
  sched_switch->set_next_pid(100);
  sched_switch->set_next_prio(1024);

  bundle = trace_->add_packet()->set_ftrace_events();
  bundle->set_cpu(10);

  event = bundle->add_event();
  event->set_timestamp(1001);
  event->set_pid(12);

  sched_switch = event->set_sched_switch();
  sched_switch->set_prev_pid(100);
  sched_switch->set_prev_comm(kProcName1);
  sched_switch->set_prev_prio(256);
  sched_switch->set_prev_state(32);
  sched_switch->set_next_comm(kProcName2);
  sched_switch->set_next_pid(10);
  sched_switch->set_next_prio(512);

  EXPECT_CALL(*sched_,
              PushSchedSwitch(10, 1000, 10, base::StringView(kProcName2), 256,
                              32, 100, base::StringView(kProcName1), 1024));

  EXPECT_CALL(*sched_,
              PushSchedSwitch(10, 1001, 100, base::StringView(kProcName1), 256,
                              32, 10, base::StringView(kProcName2), 512));
  Tokenize();
  context_.sorter->ExtractEventsForced();
}

TEST_F(ProtoTraceParserTest, RepeatedLoadSinglePacket) {
  auto* bundle = trace_->add_packet()->set_ftrace_events();
  bundle->set_cpu(10);
  auto* event = bundle->add_event();
  event->set_timestamp(1000);
  event->set_pid(12);
  static const char kProcName1[] = "proc1";
  static const char kProcName2[] = "proc2";
  auto* sched_switch = event->set_sched_switch();
  sched_switch->set_prev_pid(10);
  sched_switch->set_prev_comm(kProcName2);
  sched_switch->set_prev_prio(256);
  sched_switch->set_prev_state(32);
  sched_switch->set_next_comm(kProcName1);
  sched_switch->set_next_pid(100);
  sched_switch->set_next_prio(1024);
  EXPECT_CALL(*sched_,
              PushSchedSwitch(10, 1000, 10, base::StringView(kProcName2), 256,
                              32, 100, base::StringView(kProcName1), 1024));
  Tokenize();
  context_.sorter->ExtractEventsForced();

  bundle = trace_->add_packet()->set_ftrace_events();
  bundle->set_cpu(10);
  event = bundle->add_event();
  event->set_timestamp(1001);
  event->set_pid(12);
  sched_switch = event->set_sched_switch();
  sched_switch->set_prev_pid(100);
  sched_switch->set_prev_comm(kProcName1);
  sched_switch->set_prev_prio(256);
  sched_switch->set_prev_state(32);
  sched_switch->set_next_comm(kProcName2);
  sched_switch->set_next_pid(10);
  sched_switch->set_next_prio(512);

  EXPECT_CALL(*sched_,
              PushSchedSwitch(10, 1001, 100, base::StringView(kProcName1), 256,
                              32, 10, base::StringView(kProcName2), 512));
  Tokenize();
  context_.sorter->ExtractEventsForced();
}

TEST_F(ProtoTraceParserTest, LoadCpuFreq) {
  auto* bundle = trace_->add_packet()->set_ftrace_events();
  bundle->set_cpu(12);
  auto* event = bundle->add_event();
  event->set_timestamp(1000);
  event->set_pid(12);
  auto* cpu_freq = event->set_cpu_frequency();
  cpu_freq->set_cpu_id(10);
  cpu_freq->set_state(2000);

  EXPECT_CALL(*event_, PushCounter(1000, DoubleEq(2000), TrackId{0}));
  Tokenize();
  context_.sorter->ExtractEventsForced();

  auto dim_set_id = context_.storage->track_table()[0].dimension_arg_set_id();
  ASSERT_TRUE(dim_set_id.has_value());
  ArgExtractor args_cursor(context_.storage->arg_table());
  uint32_t row = args_cursor.Get(*dim_set_id, "cpu");
  ASSERT_NE(row, std::numeric_limits<uint32_t>::max());
  Variadic cpu = GetArgValue(*context_.storage, args_cursor.cursor());
  EXPECT_EQ(cpu.int_value, 10u);
}

TEST_F(ProtoTraceParserTest, LoadCpuFreqKHz) {
  auto* packet = trace_->add_packet();
  uint64_t ts = 1000;
  packet->set_timestamp(ts);
  auto* bundle = packet->set_sys_stats();
  bundle->add_cpufreq_khz(2650000u);
  bundle->add_cpufreq_khz(3698200u);

  EXPECT_CALL(*event_, PushCounter(static_cast<int64_t>(ts), DoubleEq(2650000u),
                                   TrackId{0u}));
  EXPECT_CALL(*event_, PushCounter(static_cast<int64_t>(ts), DoubleEq(3698200u),
                                   TrackId{1u}));
  Tokenize();
  context_.sorter->ExtractEventsForced();

  EXPECT_EQ(context_.storage->track_table().row_count(), 2u);

  auto track_row = context_.storage->track_table()[TrackId(0)];
  EXPECT_EQ(context_.storage->GetString(track_row.name()), "cpufreq");
  ArgExtractor args_cursor(context_.storage->arg_table());
  uint32_t arg_row =
      args_cursor.Get(track_row.dimension_arg_set_id().value(), "cpu");
  ASSERT_NE(arg_row, std::numeric_limits<uint32_t>::max());
  Variadic cpu = GetArgValue(*context_.storage, args_cursor.cursor());
  ASSERT_EQ(cpu.type, Variadic::Type::kInt);
  EXPECT_EQ(cpu.uint_value, 0u);

  track_row = context_.storage->track_table()[TrackId(1)];
  arg_row = args_cursor.Get(track_row.dimension_arg_set_id().value(), "cpu");
  ASSERT_NE(arg_row, std::numeric_limits<uint32_t>::max());
  cpu = GetArgValue(*context_.storage, args_cursor.cursor());
  ASSERT_EQ(cpu.type, Variadic::Type::kInt);
  EXPECT_EQ(cpu.uint_value, 1u);
}

TEST_F(ProtoTraceParserTest, LoadCpuIdleStats) {
  auto* packet = trace_->add_packet();
  uint64_t ts = 1000;
  packet->set_timestamp(ts);
  auto* bundle = packet->set_sys_stats();
  auto* cpuidle_state = bundle->add_cpuidle_state();
  cpuidle_state->set_cpu_id(0);
  auto* cpuidle_state_entry = cpuidle_state->add_cpuidle_state_entry();
  cpuidle_state_entry->set_state("mock_state0");
  cpuidle_state_entry->set_duration_us(20000);
  EXPECT_CALL(*event_, PushCounter(static_cast<int64_t>(ts),
                                   static_cast<double>(20000), TrackId{0u}));
  Tokenize();
  context_.sorter->ExtractEventsForced();

  EXPECT_EQ(context_.storage->track_table().row_count(), 1u);
}

TEST_F(ProtoTraceParserTest, LoadGpuFreqStats) {
  auto* packet = trace_->add_packet();
  uint64_t ts = 1000;
  packet->set_timestamp(ts);
  auto* bundle = packet->set_sys_stats();
  bundle->add_gpufreq_mhz(300);
  EXPECT_CALL(*event_, PushCounter(static_cast<int64_t>(ts),
                                   static_cast<double>(300), TrackId{0u}));
  Tokenize();
  context_.sorter->ExtractEventsForced();

  EXPECT_EQ(context_.storage->track_table().row_count(), 1u);
}

TEST_F(ProtoTraceParserTest, LoadMemInfo) {
  auto* packet = trace_->add_packet();
  uint64_t ts = 1000;
  packet->set_timestamp(ts);
  auto* bundle = packet->set_sys_stats();
  auto* meminfo = bundle->add_meminfo();
  meminfo->set_key(protos::pbzero::MEMINFO_MEM_TOTAL);
  uint32_t value = 10;
  meminfo->set_value(value);

  EXPECT_CALL(*event_, PushCounter(static_cast<int64_t>(ts),
                                   DoubleEq(value * 1024.0), TrackId{0u}));
  Tokenize();
  context_.sorter->ExtractEventsForced();

  EXPECT_EQ(context_.storage->track_table().row_count(), 1u);
}

TEST_F(ProtoTraceParserTest, LoadVmStats) {
  auto* packet = trace_->add_packet();
  uint64_t ts = 1000;
  packet->set_timestamp(ts);
  auto* bundle = packet->set_sys_stats();
  auto* meminfo = bundle->add_vmstat();
  meminfo->set_key(protos::pbzero::VMSTAT_COMPACT_SUCCESS);
  uint32_t value = 10;
  meminfo->set_value(value);

  EXPECT_CALL(*event_, PushCounter(static_cast<int64_t>(ts), DoubleEq(value),
                                   TrackId{0u}));
  Tokenize();
  context_.sorter->ExtractEventsForced();

  EXPECT_EQ(context_.storage->track_table().row_count(), 1u);
}

TEST_F(ProtoTraceParserTest, LoadThermal) {
  auto* packet = trace_->add_packet();
  uint64_t ts = 1000;
  packet->set_timestamp(ts);
  auto* bundle = packet->set_sys_stats();
  auto* thermal_zone = bundle->add_thermal_zone();
  thermal_zone->set_type("MOCKTYPE");
  uint64_t temp = 10000;
  thermal_zone->set_temp(temp);

  EXPECT_CALL(*event_,
              PushCounter(static_cast<int64_t>(ts),
                          DoubleEq(static_cast<double>(temp)), TrackId{0u}));
  Tokenize();
  context_.sorter->ExtractEventsForced();

  EXPECT_EQ(context_.storage->track_table().row_count(), 1u);
}

TEST_F(ProtoTraceParserTest, LoadProcessPacket) {
  auto* tree = trace_->add_packet()->set_process_tree();
  auto* process = tree->add_processes();
  static const char kProcName1[] = "proc1";

  process->add_cmdline(kProcName1);
  process->set_pid(1);
  process->set_ppid(3);

  EXPECT_CALL(*process_, GetOrCreateProcess(3)).WillOnce(testing::Return(2u));
  EXPECT_CALL(*process_, GetOrCreateProcess(1)).WillOnce(testing::Return(4u));
  EXPECT_CALL(*process_, SetProcessParent(4u, 2u, _));
  EXPECT_CALL(*process_, SetProcessMetadata(4u, base::StringView(kProcName1),
                                            base::StringView(kProcName1)));
  Tokenize();
  context_.sorter->ExtractEventsForced();
}

TEST_F(ProtoTraceParserTest, LoadProcessPacket_FirstCmdline) {
  auto* tree = trace_->add_packet()->set_process_tree();
  auto* process = tree->add_processes();
  static const char kProcName1[] = "proc1";
  static const char kProcName2[] = "proc2";

  process->add_cmdline(kProcName1);
  process->add_cmdline(kProcName2);
  process->set_pid(1);
  process->set_ppid(3);

  EXPECT_CALL(*process_, GetOrCreateProcess(3)).WillOnce(testing::Return(2u));
  EXPECT_CALL(*process_, GetOrCreateProcess(1)).WillOnce(testing::Return(4u));
  EXPECT_CALL(*process_, SetProcessParent(4u, 2u, _));
  EXPECT_CALL(*process_, SetProcessMetadata(4u, base::StringView(kProcName1),
                                            base::StringView("proc1 proc2")));
  Tokenize();
  context_.sorter->ExtractEventsForced();
}

TEST_F(ProtoTraceParserTest, LoadThreadPacket) {
  auto* tree = trace_->add_packet()->set_process_tree();
  auto* thread = tree->add_threads();
  thread->set_tid(1);
  thread->set_tgid(2);

  EXPECT_CALL(*process_, UpdateThread(1, 2));
  Tokenize();
  context_.sorter->ExtractEventsForced();
}

TEST_F(ProtoTraceParserTest, ProcessNameFromProcessDescriptor) {
  {
    auto* packet = trace_->add_packet();
    packet->set_trusted_packet_sequence_id(1);
    packet->set_incremental_state_cleared(true);
    auto* process_desc = packet->set_process_descriptor();
    process_desc->set_pid(15);
    process_desc->set_process_name("OldProcessName");
  }
  {
    auto* packet = trace_->add_packet();
    packet->set_trusted_packet_sequence_id(1);
    packet->set_incremental_state_cleared(true);
    auto* process_desc = packet->set_process_descriptor();
    process_desc->set_pid(15);
    process_desc->set_process_name("NewProcessName");
  }
  {
    auto* packet = trace_->add_packet();
    packet->set_trusted_packet_sequence_id(2);
    packet->set_incremental_state_cleared(true);
    auto* process_desc = packet->set_process_descriptor();
    process_desc->set_pid(16);
    process_desc->set_process_name("DifferentProcessName");
  }

  EXPECT_CALL(*process_, GetOrCreateProcess(15))
      .WillRepeatedly(testing::Return(1u));
  EXPECT_CALL(*process_, GetOrCreateProcess(16)).WillOnce(testing::Return(2u));

  EXPECT_CALL(*process_,
              UpdateProcessName(1u, storage_->InternString("OldProcessName"),
                                ProcessNamePriority::kTrackDescriptor));
  // Packet with same thread, but different name should update the name.
  EXPECT_CALL(*process_,
              UpdateProcessName(1u, storage_->InternString("NewProcessName"),
                                ProcessNamePriority::kTrackDescriptor));
  EXPECT_CALL(*process_, UpdateProcessName(
                             2u, storage_->InternString("DifferentProcessName"),
                             ProcessNamePriority::kTrackDescriptor));

  Tokenize();
  context_.sorter->ExtractEventsForced();
}

TEST_F(ProtoTraceParserTest, ThreadNameFromThreadDescriptor) {
  {
    auto* packet = trace_->add_packet();
    packet->set_trusted_packet_sequence_id(1);
    packet->set_incremental_state_cleared(true);
    auto* thread_desc = packet->set_thread_descriptor();
    thread_desc->set_pid(15);
    thread_desc->set_tid(16);
    thread_desc->set_reference_timestamp_us(1000);
    thread_desc->set_reference_thread_time_us(2000);
    thread_desc->set_thread_name("OldThreadName");
  }
  {
    auto* packet = trace_->add_packet();
    packet->set_trusted_packet_sequence_id(1);
    packet->set_incremental_state_cleared(true);
    auto* thread_desc = packet->set_thread_descriptor();
    thread_desc->set_pid(15);
    thread_desc->set_tid(16);
    thread_desc->set_reference_timestamp_us(1000);
    thread_desc->set_reference_thread_time_us(2000);
    thread_desc->set_thread_name("NewThreadName");
  }
  {
    auto* packet = trace_->add_packet();
    packet->set_trusted_packet_sequence_id(2);
    packet->set_incremental_state_cleared(true);
    auto* thread_desc = packet->set_thread_descriptor();
    thread_desc->set_pid(15);
    thread_desc->set_tid(11);
    thread_desc->set_reference_timestamp_us(1000);
    thread_desc->set_reference_thread_time_us(2000);
    thread_desc->set_thread_name("DifferentThreadName");
  }

  EXPECT_CALL(*process_, UpdateThread(16, 15))
      .WillRepeatedly(testing::Return(1u));
  EXPECT_CALL(*process_, UpdateThread(11, 15)).WillOnce(testing::Return(2u));

  EXPECT_CALL(*process_,
              UpdateThreadName(1u, storage_->InternString("OldThreadName"),
                               ThreadNamePriority::kTrackDescriptor));
  // Packet with same thread, but different name should update the name.
  EXPECT_CALL(*process_,
              UpdateThreadName(1u, storage_->InternString("NewThreadName"),
                               ThreadNamePriority::kTrackDescriptor));
  EXPECT_CALL(*process_, UpdateThreadName(
                             2u, storage_->InternString("DifferentThreadName"),
                             ThreadNamePriority::kTrackDescriptor));

  Tokenize();
  context_.sorter->ExtractEventsForced();
}

TEST_F(ProtoTraceParserTest, TrackEventAsyncEvents) {
  {
    auto* packet = trace_->add_packet();
    packet->set_trusted_packet_sequence_id(1);
    packet->set_incremental_state_cleared(true);
    auto* thread_desc = packet->set_thread_descriptor();
    thread_desc->set_pid(15);
    thread_desc->set_tid(16);
    thread_desc->set_reference_timestamp_us(1000);
    thread_desc->set_reference_thread_time_us(2000);
    thread_desc->set_reference_thread_instruction_count(3000);
  }
  {
    auto* packet = trace_->add_packet();
    packet->set_trusted_packet_sequence_id(1);
    auto* event = packet->set_track_event();
    event->set_timestamp_delta_us(10);              // absolute: 1010.
    event->set_thread_time_delta_us(5);             // absolute: 2005.
    event->set_thread_instruction_count_delta(20);  // absolute: 3020.
    event->add_category_iids(1);
    auto* legacy_event = event->set_legacy_event();
    legacy_event->set_name_iid(1);
    legacy_event->set_phase('b');
    legacy_event->set_global_id(10);
    legacy_event->set_use_async_tts(true);

    auto* interned_data = packet->set_interned_data();
    auto* cat1 = interned_data->add_event_categories();
    cat1->set_iid(1);
    cat1->set_name("cat1");
    auto* ev1 = interned_data->add_event_names();
    ev1->set_iid(1);
    ev1->set_name("ev1");
  }
  {
    auto* packet = trace_->add_packet();
    packet->set_trusted_packet_sequence_id(1);
    auto* event = packet->set_track_event();
    event->set_timestamp_delta_us(10);              // absolute: 1020.
    event->set_thread_time_delta_us(5);             // absolute: 2010.
    event->set_thread_instruction_count_delta(20);  // absolute: 3040.
    event->add_category_iids(1);
    auto* legacy_event = event->set_legacy_event();
    legacy_event->set_name_iid(1);
    legacy_event->set_phase('e');
    legacy_event->set_global_id(10);
    legacy_event->set_use_async_tts(true);
  }
  {
    auto* packet = trace_->add_packet();
    packet->set_trusted_packet_sequence_id(1);
    auto* event = packet->set_track_event();
    event->set_timestamp_absolute_us(1015);
    event->add_category_iids(1);
    auto* legacy_event = event->set_legacy_event();
    legacy_event->set_name_iid(2);
    legacy_event->set_phase('n');
    legacy_event->set_global_id(10);

    auto* interned_data = packet->set_interned_data();
    auto* ev2 = interned_data->add_event_names();
    ev2->set_iid(2);
    ev2->set_name("ev2");
  }
  {
    // Different category but same global_id -> separate track.
    auto* packet = trace_->add_packet();
    packet->set_trusted_packet_sequence_id(1);
    auto* event = packet->set_track_event();
    event->set_timestamp_absolute_us(1018);
    event->add_category_iids(2);
    auto* legacy_event = event->set_legacy_event();
    legacy_event->set_name_iid(2);
    legacy_event->set_phase('n');
    legacy_event->set_global_id(15);

    auto* interned_data = packet->set_interned_data();
    auto* cat2 = interned_data->add_event_categories();
    cat2->set_iid(2);
    cat2->set_name("cat2");
  }
  {
    auto* packet = trace_->add_packet();
    packet->set_trusted_packet_sequence_id(1);
    auto* event = packet->set_track_event();
    event->set_timestamp_absolute_us(1030);
    event->add_category_iids(2);
    auto* legacy_event = event->set_legacy_event();
    legacy_event->set_name_iid(2);
    legacy_event->set_phase('n');
    legacy_event->set_local_id(15);
    legacy_event->set_id_scope("scope1");
  }

  Tokenize();

  EXPECT_CALL(*process_, UpdateThread(16, 15)).WillRepeatedly(Return(1u));

  tables::ThreadTable::Row row(16);
  row.upid = 1u;
  storage_->mutable_thread_table()->Insert(row);

  StringId ev_1 = storage_->InternString("ev1");
  StringId ev_2 = storage_->InternString("ev2");

  TrackId thread_time_track{0u};
  TrackId thread_instruction_count_track{1u};

  InSequence in_sequence;  // Below slices should be sorted by timestamp.

  EXPECT_CALL(*event_, PushCounter(1010000, testing::DoubleEq(2005000),
                                   thread_time_track));
  EXPECT_CALL(*event_, PushCounter(1010000, testing::DoubleEq(3020),
                                   thread_instruction_count_track));
  EXPECT_CALL(*event_, PushCounter(1020000, testing::DoubleEq(2010000),
                                   thread_time_track));
  EXPECT_CALL(*event_, PushCounter(1020000, testing::DoubleEq(3040),
                                   thread_instruction_count_track));

  context_.sorter->ExtractEventsForced();

  // First track is for the thread; second first async, third and fourth for
  // thread time and instruction count, others are the async event tracks.
  EXPECT_EQ(storage_->track_table().row_count(), 5u);
  EXPECT_EQ(storage_->track_table()[2].name(), ev_1);
  EXPECT_EQ(storage_->track_table()[3].name(), ev_2);
  EXPECT_EQ(storage_->track_table()[4].name(), ev_2);

  EXPECT_EQ(storage_->track_table()[2].upid(), std::nullopt);
  EXPECT_EQ(storage_->track_table()[3].upid(), std::nullopt);
  EXPECT_EQ(storage_->track_table()[4].upid(), 1u);

  EXPECT_EQ(storage_->virtual_track_slices().slice_count(), 1u);
  EXPECT_EQ(storage_->virtual_track_slices().slice_ids()[0], SliceId(0u));
  EXPECT_EQ(storage_->virtual_track_slices().thread_timestamp_ns()[0], 2005000);
  EXPECT_EQ(storage_->virtual_track_slices().thread_duration_ns()[0], 5000);
  EXPECT_EQ(storage_->virtual_track_slices().thread_instruction_counts()[0],
            3020);
  EXPECT_EQ(storage_->virtual_track_slices().thread_instruction_deltas()[0],
            20);
}

// TODO(eseckler): Also test instant events on separate tracks.
TEST_F(ProtoTraceParserTest, ParseEventWithClockIdButWithoutClockSnapshot) {
  {
    auto* packet = trace_->add_packet();
    packet->set_timestamp(1000);
    packet->set_timestamp_clock_id(3);
    packet->set_trusted_packet_sequence_id(1);
    auto* bundle = packet->set_chrome_events();
    auto* metadata = bundle->add_metadata();
    metadata->set_name("test");
    metadata->set_int_value(23);
  }

  base::Status status = Tokenize();
  EXPECT_TRUE(status.ok());
  context_.sorter->ExtractEventsForced();

  // Metadata should have created a raw event.
  const auto& raw_table = storage_->chrome_raw_table();
  EXPECT_EQ(raw_table.row_count(), 1u);
}

TEST_F(ProtoTraceParserTest, LoadChromeBenchmarkMetadata) {
  static const char kName[] = "name";
  static const char kTag1[] = "tag1";
  static const char kTag2[] = "tag2";

  auto* metadata = trace_->add_packet()->set_chrome_benchmark_metadata();
  metadata->set_benchmark_name(kName);
  metadata->add_story_tags(kTag1);
  metadata->add_story_tags(kTag2);

  Tokenize();

  base::StringView benchmark = metadata::kNames[metadata::benchmark_name];
  base::StringView tags = metadata::kNames[metadata::benchmark_story_tags];

  context_.sorter->ExtractEventsForced();

  std::vector<std::pair<base::StringView, base::StringView>> meta_entries;
  for (auto it = storage_->metadata_table().IterateRows(); it; ++it) {
    base::StringView name = storage_->GetString(it.name());
    if (name == metadata::kNames[metadata::trace_time_clock_id])
      continue;
    meta_entries.emplace_back(name, storage_->GetString(*it.str_value()));
  }
  EXPECT_THAT(meta_entries,
              UnorderedElementsAreArray({make_pair(benchmark, kName),
                                         std::make_pair(tags, kTag1),
                                         std::make_pair(tags, kTag2)}));
}

TEST_F(ProtoTraceParserTest, LoadChromeMetadata) {
  auto* track_event = trace_->add_packet()->set_chrome_events();
  {
    auto* metadata = track_event->add_metadata();
    metadata->set_name("str_name");
    metadata->set_string_value("foostr");
  }

  {
    auto* metadata = track_event->add_metadata();
    metadata->set_name("int_name");
    metadata->set_int_value(42);
  }

  {
    auto* metadata = track_event->add_metadata();
    metadata->set_name("bool_name");
    metadata->set_bool_value(true);
  }

  {
    auto* metadata = track_event->add_metadata();
    metadata->set_name("json_name");
    metadata->set_json_value("{key: value}");
  }

  Tokenize();
  context_.sorter->ExtractEventsForced();

  const auto& metadata = storage_->metadata_table();

  EXPECT_STREQ(storage_->GetString(metadata[0].name()).c_str(), "cr-str_name");
  EXPECT_STREQ(storage_->GetString(*metadata[0].str_value()).c_str(), "foostr");

  EXPECT_STREQ(storage_->GetString(metadata[1].name()).c_str(), "cr-int_name");
  EXPECT_EQ(metadata[1].int_value(), 42);

  EXPECT_STREQ(storage_->GetString(metadata[2].name()).c_str(), "cr-bool_name");
  EXPECT_EQ(metadata[2].int_value(), 1);

  EXPECT_STREQ(storage_->GetString(metadata[3].name()).c_str(), "cr-json_name");
  EXPECT_STREQ(storage_->GetString(*metadata[3].str_value()).c_str(),
               "{key: value}");
}

TEST_F(ProtoTraceParserTest, AndroidPackagesList) {
  auto* packet = trace_->add_packet();
  auto* pkg_list = packet->set_packages_list();

  pkg_list->set_read_error(false);
  pkg_list->set_parse_error(true);
  {
    auto* pkg = pkg_list->add_packages();
    pkg->set_name("com.test.app");
    pkg->set_uid(1000);
    pkg->set_debuggable(false);
    pkg->set_profileable_from_shell(true);
    pkg->set_profileable(true);
    pkg->set_version_code(42);
  }
  {
    auto* pkg = pkg_list->add_packages();
    pkg->set_name("com.test.app2");
    pkg->set_uid(1001);
    pkg->set_debuggable(false);
    pkg->set_profileable_from_shell(false);
    pkg->set_profileable(false);
    pkg->set_version_code(43);
  }
  {
    auto* pkg = pkg_list->add_packages();
    pkg->set_name("com.test.app3");
    pkg->set_uid(1002);
  }

  Tokenize();
  context_.sorter->ExtractEventsForced();

  // Packet-level errors reflected in stats storage.
  EXPECT_FALSE(
      context_.stats_tracker->GetStats(stats::packages_list_has_read_errors));
  EXPECT_TRUE(
      context_.stats_tracker->GetStats(stats::packages_list_has_parse_errors));

  // Expect two metadata rows, each with an int_value of a separate arg set id.
  // The relevant arg sets have the info about the packages. To simplify test
  // structure, make an assumption that metadata storage is filled in in the
  // FIFO order of seen packages.
  const auto& package_list = context_.storage->package_list_table();
  ASSERT_EQ(package_list.row_count(), 3u);

  EXPECT_STREQ(storage_->GetString(package_list[0].package_name()).c_str(),
               "com.test.app");
  EXPECT_EQ(package_list[0].uid(), 1000u);
  EXPECT_EQ(package_list[0].debuggable(), false);
  EXPECT_EQ(package_list[0].profileable_from_shell(), true);
  EXPECT_EQ(package_list[0].profileable(), true);
  EXPECT_EQ(package_list[0].version_code(), 42);

  EXPECT_STREQ(storage_->GetString(package_list[1].package_name()).c_str(),
               "com.test.app2");
  EXPECT_EQ(package_list[1].uid(), 1001u);
  EXPECT_EQ(package_list[1].debuggable(), false);
  EXPECT_EQ(package_list[1].profileable_from_shell(), false);
  EXPECT_EQ(package_list[1].profileable(), false);
  EXPECT_EQ(package_list[1].version_code(), 43);

  EXPECT_STREQ(storage_->GetString(package_list[2].package_name()).c_str(),
               "com.test.app3");
  EXPECT_FALSE(package_list[2].version_code().has_value());
}

TEST_F(ProtoTraceParserTest, AndroidPackagesListDuplicate) {
  auto* packet = trace_->add_packet();
  auto* pkg_list = packet->set_packages_list();

  pkg_list->set_read_error(false);
  pkg_list->set_parse_error(true);
  {
    auto* pkg = pkg_list->add_packages();
    pkg->set_name("com.test.app");
    pkg->set_uid(1000);
    pkg->set_debuggable(false);
    pkg->set_profileable_from_shell(true);
    pkg->set_profileable(true);
    pkg->set_version_code(42);
  }
  {
    auto* pkg = pkg_list->add_packages();
    pkg->set_name("com.test.app");
    pkg->set_uid(1000);
    pkg->set_debuggable(false);
    pkg->set_profileable_from_shell(true);
    pkg->set_profileable(true);
    pkg->set_version_code(42);
  }

  Tokenize();
  context_.sorter->ExtractEventsForced();

  // Packet-level errors reflected in stats storage.
  EXPECT_FALSE(
      context_.stats_tracker->GetStats(stats::packages_list_has_read_errors));
  EXPECT_TRUE(
      context_.stats_tracker->GetStats(stats::packages_list_has_parse_errors));

  // Expect two metadata rows, each with an int_value of a separate arg set id.
  // The relevant arg sets have the info about the packages. To simplify test
  // structure, make an assumption that metadata storage is filled in in the
  // FIFO order of seen packages.
  const auto& package_list = context_.storage->package_list_table();
  ASSERT_EQ(package_list.row_count(), 1u);

  EXPECT_STREQ(storage_->GetString(package_list[0].package_name()).c_str(),
               "com.test.app");
  EXPECT_EQ(package_list[0].uid(), 1000u);
  EXPECT_EQ(package_list[0].debuggable(), false);
  EXPECT_EQ(package_list[0].profileable_from_shell(), true);
  EXPECT_EQ(package_list[0].profileable(), true);
  EXPECT_EQ(package_list[0].version_code(), 42);
}

TEST_F(ProtoTraceParserTest, ConfigUuid) {
  auto* config = trace_->add_packet()->set_trace_config();
  config->set_trace_uuid_lsb(1);
  config->set_trace_uuid_msb(2);

  ASSERT_TRUE(Tokenize().ok());
  context_.sorter->ExtractEventsForced();

  SqlValue value =
      context_.metadata_tracker->GetMetadata(metadata::trace_uuid).value();
  EXPECT_STREQ(value.string_value, "00000000-0000-0002-0000-000000000001");
  ASSERT_TRUE(context_.uuid_state->uuid_found_in_trace);
}

TEST_F(ProtoTraceParserTest, PacketUuid) {
  auto* uuid = trace_->add_packet()->set_trace_uuid();
  uuid->set_lsb(1);
  uuid->set_msb(2);

  ASSERT_TRUE(Tokenize().ok());
  context_.sorter->ExtractEventsForced();

  SqlValue value =
      context_.metadata_tracker->GetMetadata(metadata::trace_uuid).value();
  EXPECT_STREQ(value.string_value, "00000000-0000-0002-0000-000000000001");
  ASSERT_TRUE(context_.uuid_state->uuid_found_in_trace);
}

// If both the TraceConfig and TracePacket.trace_uuid are present, the latter
// is considered the source of truth.
TEST_F(ProtoTraceParserTest, PacketAndConfigUuid) {
  auto* uuid = trace_->add_packet()->set_trace_uuid();
  uuid->set_lsb(1);
  uuid->set_msb(2);

  auto* config = trace_->add_packet()->set_trace_config();
  config->set_trace_uuid_lsb(42);
  config->set_trace_uuid_msb(42);

  ASSERT_TRUE(Tokenize().ok());
  context_.sorter->ExtractEventsForced();

  SqlValue value =
      context_.metadata_tracker->GetMetadata(metadata::trace_uuid).value();
  EXPECT_STREQ(value.string_value, "00000000-0000-0002-0000-000000000001");
  ASSERT_TRUE(context_.uuid_state->uuid_found_in_trace);
}

TEST_F(ProtoTraceParserTest, ConfigPbtxt) {
  auto* config = trace_->add_packet()->set_trace_config();
  config->add_buffers()->set_size_kb(42);

  ASSERT_TRUE(Tokenize().ok());
  context_.sorter->ExtractEventsForced();

  SqlValue value =
      context_.metadata_tracker->GetMetadata(metadata::trace_config_pbtxt)
          .value();
  EXPECT_THAT(value.string_value, HasSubstr("size_kb: 42"));
}

TEST_F(ProtoTraceParserTest, RecoveredTraceInfoWithReason) {
  auto* packet = trace_->add_packet();
  auto* info = packet->set_recovered_trace_info();
  info->set_reason(
      protos::pbzero::RecoveredTraceInfo::REASON_UNEXPECTED_REBOOT);

  ASSERT_TRUE(Tokenize().ok());
  context_.sorter->ExtractEventsForced();

  SqlValue reason =
      context_.metadata_tracker->GetMetadata(metadata::trace_recovery_reason)
          .value();
  EXPECT_STREQ(reason.string_value, "REASON_UNEXPECTED_REBOOT");
}

TEST_F(ProtoTraceParserTest,
       RecoveredTraceInfoOmittedReasonDefaultsToUnspecified) {
  trace_->add_packet()->set_recovered_trace_info();

  ASSERT_TRUE(Tokenize().ok());
  context_.sorter->ExtractEventsForced();

  SqlValue reason =
      context_.metadata_tracker->GetMetadata(metadata::trace_recovery_reason)
          .value();
  EXPECT_STREQ(reason.string_value, "REASON_UNSPECIFIED");
}

TEST_F(ProtoTraceParserTest, RecoveredTraceInfoNotPresentIsNull) {
  {
    auto* packet = trace_->add_packet();
    packet->set_timestamp(1000);
  }
  ASSERT_TRUE(Tokenize().ok());
  context_.sorter->ExtractEventsForced();

  auto reason =
      context_.metadata_tracker->GetMetadata(metadata::trace_recovery_reason);
  EXPECT_FALSE(reason.has_value());
}

TEST_F(ProtoTraceParserTest, PerfEventWithMultipleCounter) {
  {
    auto* packet = trace_->add_packet();
    packet->set_trusted_packet_sequence_id(1);
    packet->set_incremental_state_cleared(true);
    packet->set_timestamp(3000);
    auto* perf_sample_default =
        packet->set_trace_packet_defaults()->set_perf_sample_defaults();

    // leader description:
    auto* timebase = perf_sample_default->set_timebase();
    timebase->set_name("leader");
    timebase->set_counter(
        protos::pbzero::PerfEvents::Counter::SW_CONTEXT_SWITCHES);
    timebase->set_frequency(1000);

    // followers description:
    auto* follower = perf_sample_default->add_followers();
    follower->set_counter(protos::pbzero::PerfEvents::Counter::HW_CPU_CYCLES);
    follower->set_name("cycle-follower");

    follower = perf_sample_default->add_followers();
    follower->set_counter(protos::pbzero::PerfEvents::Counter::HW_CACHE_MISSES);
    follower->set_name("cache-follower");
  }
  {
    auto* packet = trace_->add_packet();
    packet->set_trusted_packet_sequence_id(1);
    packet->set_timestamp(3000);
    auto* perf_sample = packet->set_perf_sample();
    perf_sample->set_cpu(0);
    perf_sample->set_pid(1);
    perf_sample->set_tid(42);
    perf_sample->set_cpu_mode(
        ::perfetto::protos::pbzero::Profiling_CpuMode::MODE_USER);
    perf_sample->set_timebase_count(512);
    perf_sample->add_follower_counts(1024);
    perf_sample->add_follower_counts(2048);
  }

  EXPECT_CALL(*event_, PushCounter(3000, testing::DoubleEq(512), TrackId{0u}));
  EXPECT_CALL(*event_, PushCounter(3000, testing::DoubleEq(1024), TrackId{1u}));
  EXPECT_CALL(*event_, PushCounter(3000, testing::DoubleEq(2048), TrackId{2u}));

  Tokenize();
  context_.sorter->ExtractEventsForced();

  const auto& tracks = storage_->track_table();
  EXPECT_EQ(tracks.row_count(), 3u);

  EXPECT_EQ(tracks[0].name(), storage_->InternString("leader"));
  EXPECT_EQ(tracks[1].name(), storage_->InternString("cycle-follower"));
  EXPECT_EQ(tracks[2].name(), storage_->InternString("cache-follower"));

  Variadic cpu = Variadic::Null();
  ArgExtractor args_cursor(context_.storage->arg_table());
  auto get_cpu = [&, this](uint32_t i) {
    auto dim_set_id = tracks[i].dimension_arg_set_id();
    ASSERT_TRUE(dim_set_id.has_value());
    uint32_t row = args_cursor.Get(*dim_set_id, "cpu");
    ASSERT_NE(row, std::numeric_limits<uint32_t>::max());
    cpu = GetArgValue(*context_.storage, args_cursor.cursor());
  };
  get_cpu(0);
  EXPECT_EQ(cpu.int_value, 0u);
  get_cpu(1);
  EXPECT_EQ(cpu.int_value, 0u);
  get_cpu(2);
  EXPECT_EQ(cpu.int_value, 0u);
}

TEST_F(ProtoTraceParserTest, TraceAttributes) {
  auto container = trace_->add_packet()->set_trace_attributes();
  auto* attribute = container->add_attribute();
  attribute->set_key("string_key");
  attribute->set_string_value("string_value");
  attribute = container->add_attribute();
  attribute->set_key("int_key");
  attribute->set_long_value(42);
  attribute = container->add_attribute();
  ASSERT_TRUE(Tokenize().ok());
  context_.sorter->ExtractEventsForced();
  const auto& metadata_table = context_.storage->metadata_table();
  EXPECT_EQ(metadata_table.row_count(), 2u);
  EXPECT_STREQ(context_.storage->GetString(metadata_table[0].name()).c_str(),
               "trace_attribute.string_key");
  EXPECT_STREQ(context_.storage->GetString(metadata_table[1].name()).c_str(),
               "trace_attribute.int_key");
}

TEST_F(ProtoTraceParserTest, TraceAttributesLastValueWins) {
  auto* container = trace_->add_packet()->set_trace_attributes();
  auto* attribute = container->add_attribute();
  attribute->set_key("key");
  attribute->set_string_value("old_value");
  container = trace_->add_packet()->set_trace_attributes();
  attribute = container->add_attribute();
  attribute->set_key("key");
  attribute->set_long_value(42);
  ASSERT_TRUE(Tokenize().ok());
  context_.sorter->ExtractEventsForced();
  const auto& metadata_table = context_.storage->metadata_table();
  EXPECT_EQ(metadata_table.row_count(), 1u);
  EXPECT_STREQ(context_.storage->GetString(metadata_table[0].name()).c_str(),
               "trace_attribute.key");
  EXPECT_FALSE(metadata_table[0].str_value().has_value());
  EXPECT_EQ(metadata_table[0].int_value(), 42);
}

TEST_F(ProtoTraceParserTest, EmptyCpuInfo) {
  trace_->add_packet()->set_cpu_info();
  ASSERT_TRUE(Tokenize().ok());
  context_.sorter->ExtractEventsForced();
  EXPECT_EQ(context_.stats_tracker->GetStats(stats::cpu_info_empty), 1);
}

TEST_F(ProtoTraceParserTest, NonEmptyCpuInfo) {
  auto* cpu_info = trace_->add_packet()->set_cpu_info();
  auto* cpu = cpu_info->add_cpus();
  cpu->set_processor("ARMv8 Processor rev 0 (v8l)");
  cpu->set_capacity(1024);
  cpu->add_frequencies(1800000);

  ASSERT_TRUE(Tokenize().ok());
  context_.sorter->ExtractEventsForced();

  EXPECT_EQ(context_.stats_tracker->GetStats(stats::cpu_info_empty), 0);
  const auto& cpu_table = context_.storage->cpu_table();
  EXPECT_STREQ(context_.storage->GetString(cpu_table[0].processor()).c_str(),
               "ARMv8 Processor rev 0 (v8l)");
  EXPECT_EQ(cpu_table[0].capacity(), 1024u);
}

TEST_F(ProtoTraceParserTest, CpuInfoUniformCapacityFallsBackToFrequencies) {
  auto* cpu_info = trace_->add_packet()->set_cpu_info();
  auto* ecore = cpu_info->add_cpus();
  ecore->set_processor("12th Gen Intel(R) Core(TM) i5-1235U");
  ecore->set_capacity(1024);
  ecore->add_frequencies(3300000);
  auto* pcore = cpu_info->add_cpus();
  pcore->set_processor("12th Gen Intel(R) Core(TM) i5-1235U");
  pcore->set_capacity(1024);
  pcore->add_frequencies(4400000);

  ASSERT_TRUE(Tokenize().ok());
  context_.sorter->ExtractEventsForced();

  const auto& cpu_table = context_.storage->cpu_table();
  ASSERT_GE(cpu_table.row_count(), 2u);
  EXPECT_EQ(cpu_table[0].capacity(), 1024u);
  EXPECT_EQ(cpu_table[1].capacity(), 1024u);
  EXPECT_EQ(cpu_table[0].cluster_id(), 0u);
  EXPECT_EQ(cpu_table[1].cluster_id(), 1u);
}

TEST_F(ProtoTraceParserTest, SystemInfoDeviceTreeCompatible) {
  auto* packet = trace_->add_packet();
  packet->set_trusted_packet_sequence_id(1);
  packet->set_timestamp(1000);
  auto* sys_info = packet->set_system_info();
  sys_info->add_device_tree_compatibles("google,gs101-oriole");
  sys_info->add_device_tree_compatibles("google,gs101");

  ASSERT_TRUE(Tokenize().ok());
  context_.sorter->ExtractEventsForced();

  const auto& metadata_table = context_.storage->metadata_table();
  std::vector<std::string> compatibles;
  StringId key_id = context_.storage->InternString("device_tree_compatible");
  for (uint32_t i = 0; i < metadata_table.row_count(); ++i) {
    if (metadata_table[i].name() == key_id) {
      compatibles.push_back(
          context_.storage->GetString(*metadata_table[i].str_value()).c_str());
    }
  }
  EXPECT_THAT(compatibles,
              testing::ElementsAre("google,gs101-oriole", "google,gs101"));
}

}  // namespace
}  // namespace perfetto::trace_processor
