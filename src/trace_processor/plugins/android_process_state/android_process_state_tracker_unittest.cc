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

#include "src/trace_processor/plugins/android_process_state/android_process_state_tracker.h"

#include <limits>
#include <memory>
#include <string>

#include "perfetto/protozero/scattered_heap_buffer.h"
#include "protos/perfetto/common/builtin_clock.pbzero.h"
#include "protos/perfetto/config/android/android_process_state_config.pbzero.h"
#include "protos/perfetto/config/data_source_config.pbzero.h"
#include "protos/perfetto/config/trace_config.pbzero.h"
#include "protos/third_party/android/frameworks/base/proto/tracing/frameworks_base_trace_packet.pbzero.h"
#include "src/trace_processor/importers/android/android_process_tracker.h"
#include "src/trace_processor/importers/common/clock_tracker.h"
#include "src/trace_processor/importers/common/global_stats_tracker.h"
#include "src/trace_processor/importers/common/machine_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/plugins/android_process_state/android_process_state_module.h"
#include "src/trace_processor/plugins/android_process_state/tables_py.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/clock_synchronizer.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::android_process_state {
class AndroidProcessStateTrackerTest : public ::testing::Test {
 public:
  AndroidProcessStateTrackerTest() {
    context_.storage.reset(new TraceStorage());
    context_.global_stats_tracker =
        std::make_unique<GlobalStatsTracker>(context_.storage.get());
    context_.trace_state =
        TraceProcessorContextPtr<TraceProcessorContext::TraceState>::MakeRoot(
            TraceProcessorContext::TraceState{TraceId(0)});
    context_.machine_tracker.reset(new MachineTracker(&context_, 0));
    context_.stats_tracker = std::make_unique<StatsTracker>(&context_);
    context_.process_tracker.reset(new ProcessTracker(&context_));
    context_.android_process_tracker.reset(
        new AndroidProcessTracker(&context_));
    context_.descriptor_pool_.reset(new DescriptorPool());
    context_.trace_time_state = std::make_unique<TraceTimeState>(
        ClockId::Machine(protos::pbzero::BUILTIN_CLOCK_BOOTTIME));
    primary_sync_ = std::make_unique<ClockSynchronizer>(
        context_.trace_time_state.get(),
        std::make_unique<ClockSynchronizerListenerImpl>(&context_));
    context_.clock_tracker = std::make_unique<ClockTracker>(
        &context_, primary_sync_.get(), /*is_primary=*/true);
    process_state_table_ = std::make_unique<tables::AndroidProcessStateTable>(
        context_.storage->mutable_string_pool());
    freezer_state_table_ = std::make_unique<tables::AndroidFreezerStateTable>(
        context_.storage->mutable_string_pool());
    tracker_ = std::make_unique<AndroidProcessStateTracker>(
        &context_, process_state_table_.get(), freezer_state_table_.get());
  }

 protected:
  TraceProcessorContext context_;
  std::unique_ptr<ClockSynchronizer> primary_sync_;
  std::unique_ptr<tables::AndroidProcessStateTable> process_state_table_;
  std::unique_ptr<tables::AndroidFreezerStateTable> freezer_state_table_;
  std::unique_ptr<AndroidProcessStateTracker> tracker_;
};

namespace {

using tables::AndroidFreezerStateTable;
using tables::AndroidProcessStateTable;
namespace fb = com::android::internal::pbzero;

TEST_F(AndroidProcessStateTrackerTest, CoexistenceDefaultsToKernelAuthority) {
  // Both ftrace and dump_process_metadata configured: defaults to
  // kKernelAuthority.
  tracker_->OnConfigDetected(/*ftrace_configured=*/true,
                             /*dump_process_metadata=*/true);
  EXPECT_FALSE(context_.android_process_tracker->FrameworkIsProcessAuthority());
}

TEST_F(AndroidProcessStateTrackerTest, UserspaceAuthorityMode) {
  // Only dump_process_metadata configured, no ftrace.
  tracker_->OnConfigDetected(/*ftrace_configured=*/false,
                             /*dump_process_metadata=*/true);
  EXPECT_TRUE(context_.android_process_tracker->FrameworkIsProcessAuthority());
}

TEST_F(AndroidProcessStateTrackerTest, KernelAuthorityModeDisabledFlag) {
  // dump_process_metadata explicitly false.
  tracker_->OnConfigDetected(/*ftrace_configured=*/false,
                             /*dump_process_metadata=*/false);
  EXPECT_FALSE(context_.android_process_tracker->FrameworkIsProcessAuthority());
}

TEST_F(AndroidProcessStateTrackerTest, DefaultModeIsKernelAuthority) {
  EXPECT_FALSE(context_.android_process_tracker->FrameworkIsProcessAuthority());
}

TEST_F(AndroidProcessStateTrackerTest, SystemServerZeroSuppressionHandling) {
  // Configured with dump_process_metadata = true.
  // system_server has startSeq=0 and startElapsedTime=0 (suppressed on wire),
  // but has process_name.
  tracker_->OnConfigDetected(/*ftrace_configured=*/false,
                             /*dump_process_metadata=*/true);

  protozero::HeapBuffered<fb::AndroidProcessStateSnapshot> snapshot;
  auto* rec = snapshot->add_record();
  rec->set_pid(1000);
  rec->set_process_name("system_server");
  std::string bytes = snapshot.SerializeAsString();
  tracker_->ParseProcessStateDump(
      1000, protozero::ConstBytes{
                reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()});

  auto opt_upid = context_.process_tracker->GetProcessOrNull(1000);
  ASSERT_TRUE(opt_upid.has_value());
  auto process = context_.storage->process_table()[*opt_upid];
  EXPECT_EQ(context_.storage->GetString(*process.name()), "system_server");
  EXPECT_FALSE(
      context_.android_process_tracker->GetStartSeqId(*opt_upid).has_value());
  EXPECT_FALSE(process.start_ts().has_value());
}

TEST_F(AndroidProcessStateTrackerTest, TokenizeTraceConfigViaModule) {
  ProtoImporterModuleContext module_context;
  AndroidProcessStateModule module(&module_context, tracker_.get());

  protozero::HeapBuffered<protos::pbzero::TraceConfig> config;
  {
    auto* ds = config->add_data_sources();
    auto* ds_cfg = ds->set_config();
    ds_cfg->set_name("linux.ftrace");
  }
  {
    auto* ds = config->add_data_sources();
    auto* ds_cfg = ds->set_config();
    ds_cfg->set_name("android.process_state");
    auto* aps_cfg = ds_cfg->set_android_process_state_config();
    aps_cfg->set_dump_process_metadata(true);
  }

  std::string config_bytes = config.SerializeAsString();
  protos::pbzero::TraceConfig_Decoder trace_config_decoder(config_bytes);
  module.TokenizeTraceConfig(trace_config_decoder);

  EXPECT_FALSE(context_.android_process_tracker->FrameworkIsProcessAuthority());
}

TEST_F(AndroidProcessStateTrackerTest, ConfigLatching) {
  // First config enables metadata without ftrace -> userspace authority.
  tracker_->OnConfigDetected(/*ftrace_configured=*/false,
                             /*dump_process_metadata=*/true);
  EXPECT_TRUE(context_.android_process_tracker->FrameworkIsProcessAuthority());

  // Secondary config without metadata or with false does not overwrite.
  tracker_->OnConfigDetected(/*ftrace_configured=*/false,
                             /*dump_process_metadata=*/false);
  EXPECT_TRUE(context_.android_process_tracker->FrameworkIsProcessAuthority());

  // Subsequent config enabling ftrace switches mode to kernel authority.
  tracker_->OnConfigDetected(/*ftrace_configured=*/true,
                             /*dump_process_metadata=*/std::nullopt);
  EXPECT_FALSE(context_.android_process_tracker->FrameworkIsProcessAuthority());
}

TEST_F(AndroidProcessStateTrackerTest, ReconcileUserspaceAuthority) {
  tracker_->OnConfigDetected(/*ftrace_configured=*/false,
                             /*dump_process_metadata=*/true);

  protozero::HeapBuffered<fb::AndroidProcessStateSnapshot> snapshot;
  auto* rec = snapshot->add_record();
  rec->set_pid(2001);
  rec->set_uid(10050);
  rec->set_process_name("com.test.app");
  rec->set_start_seq_id(100);
  rec->set_start_time_ms(5000);
  rec->set_proc_state(1002);
  rec->set_oom_score(100);

  std::string bytes = snapshot.SerializeAsString();
  tracker_->ParseProcessStateDump(
      1000, protozero::ConstBytes{
                reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()});

  tracker_->Finalize();

  auto opt_upid = context_.process_tracker->GetProcessOrNull(2001);
  ASSERT_TRUE(opt_upid.has_value());
  auto process = context_.storage->process_table()[*opt_upid];
  EXPECT_EQ(process.pid(), 2001u);
  EXPECT_EQ(context_.storage->GetString(*process.name()), "com.test.app");
  EXPECT_EQ(process.android_user_id(), 0u);
  EXPECT_EQ(context_.android_process_tracker->GetStartSeqId(*opt_upid), 100);
  EXPECT_EQ(process.start_ts(), 5000000000LL);

  EXPECT_EQ(process_state_table_->row_count(), 1u);
  auto state_row = (*process_state_table_)[0];
  EXPECT_EQ(state_row.upid(), *opt_upid);
  EXPECT_EQ(state_row.is_initial(), 1u);
  EXPECT_EQ(state_row.oom_score(), 100);
  // The seq id is copied out of ProcessTracker so it outlives parsing.
  EXPECT_EQ(state_row.start_seq_id(), 100);
}

TEST_F(AndroidProcessStateTrackerTest, ReconcileUserspacePidRecycling) {
  tracker_->OnConfigDetected(/*ftrace_configured=*/false,
                             /*dump_process_metadata=*/true);

  // Initial process for PID 2001 with start_seq_id 10.
  UniquePid old_upid = context_.android_process_tracker->GetOrStartProcess(
      1000, 2001, 10, context_.storage->InternString("com.test.oldapp"),
      ThreadNamePriority::kTrackDescriptor);

  // New dump record for PID 2001 with different start_seq_id 20 arriving at
  // ts=2000, with start_time_ms=8000.
  protozero::HeapBuffered<fb::AndroidProcessStateSnapshot> snapshot;
  auto* rec = snapshot->add_record();
  rec->set_pid(2001);
  rec->set_uid(10060);
  rec->set_process_name("com.test.newapp");
  rec->set_start_seq_id(20);
  rec->set_start_time_ms(8000);
  rec->set_proc_state(1003);

  std::string bytes = snapshot.SerializeAsString();
  tracker_->ParseProcessStateDump(
      2000, protozero::ConstBytes{
                reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()});

  tracker_->Finalize();

  auto opt_upid = context_.process_tracker->GetProcessOrNull(2001);
  ASSERT_TRUE(opt_upid.has_value());
  EXPECT_NE(*opt_upid, old_upid);

  // Seeing a new incarnation tells us the old one is gone, but not when it
  // died: end_ts stays unset until a death event reports it.
  auto old_process = context_.storage->process_table()[old_upid];
  EXPECT_FALSE(old_process.end_ts().has_value());

  auto new_process = context_.storage->process_table()[*opt_upid];
  EXPECT_EQ(context_.storage->GetString(*new_process.name()),
            "com.test.newapp");
  EXPECT_EQ(context_.android_process_tracker->GetStartSeqId(*opt_upid), 20);
  EXPECT_EQ(new_process.start_ts(), 8000000000LL);
}

TEST_F(AndroidProcessStateTrackerTest, RecyclingObservedMidTraceThenSnapshot) {
  tracker_->OnConfigDetected(/*ftrace_configured=*/false,
                             /*dump_process_metadata=*/true);

  // Mid-trace: App A is started via framework at ts=1000 with seq_id=1.
  StringId app_a_name = context_.storage->InternString("com.test.appa");
  UniquePid upid_a = context_.android_process_tracker->GetOrStartProcess(
      1000, 2001, 1, app_a_name, ThreadNamePriority::kTrackDescriptor);
  EXPECT_EQ(context_.storage->GetString(
                *context_.storage->process_table()[upid_a].name()),
            "com.test.appa");

  // Mid-trace at ts=5000: PID 2001 is recycled to App B with seq_id=2.
  StringId app_b_name = context_.storage->InternString("com.test.appb");
  UniquePid upid_b = context_.android_process_tracker->GetOrStartProcess(
      5000, 2001, 2, app_b_name, ThreadNamePriority::kTrackDescriptor);
  EXPECT_NE(upid_a, upid_b);

  // App A lost the pid but is not marked as ended: no death event was seen.
  EXPECT_FALSE(context_.storage->process_table()[upid_a].end_ts().has_value());
  EXPECT_EQ(*context_.storage->process_table()[upid_b].start_ts(), 5000);

  // Snapshot arrives at trace stop (ts=10000) for App B (seq_id=2,
  // start_time_ms=5).
  protozero::HeapBuffered<fb::AndroidProcessStateSnapshot> snapshot;
  auto* rec = snapshot->add_record();
  rec->set_pid(2001);
  rec->set_process_name("com.test.appb");
  rec->set_start_seq_id(2);
  rec->set_start_time_ms(5);
  rec->set_proc_state(1002);
  std::string bytes = snapshot.SerializeAsString();
  tracker_->ParseProcessStateDump(
      10000, protozero::ConstBytes{
                 reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()});

  tracker_->Finalize();

  // Verify no 3rd UPID was created; snapshot updated upid_b.
  EXPECT_EQ(context_.storage->process_table().row_count(), 3u);
  auto current_upid = context_.process_tracker->GetProcessOrNull(2001);
  ASSERT_TRUE(current_upid.has_value());
  EXPECT_EQ(*current_upid, upid_b);
}

TEST_F(AndroidProcessStateTrackerTest,
       PidReuseWithoutResolvableStartTimeLeavesEndTsUnset) {
  tracker_->OnConfigDetected(/*ftrace_configured=*/false,
                             /*dump_process_metadata=*/true);

  // Initial process with start_seq_id 10.
  UniquePid old_upid = context_.android_process_tracker->GetOrStartProcess(
      1000, 2001, 10, context_.storage->InternString("com.test.oldapp"),
      ThreadNamePriority::kTrackDescriptor);

  // Snapshot has different start_seq_id 20, but NO start_time_ms.
  protozero::HeapBuffered<fb::AndroidProcessStateSnapshot> snapshot;
  auto* rec = snapshot->add_record();
  rec->set_pid(2001);
  rec->set_start_seq_id(20);
  rec->set_process_name("com.test.newapp");

  std::string bytes = snapshot.SerializeAsString();
  tracker_->ParseProcessStateDump(
      5000, protozero::ConstBytes{
                reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()});

  tracker_->Finalize();

  auto opt_upid = context_.process_tracker->GetProcessOrNull(2001);
  ASSERT_TRUE(opt_upid.has_value());
  EXPECT_NE(*opt_upid, old_upid);

  // Old process end_ts must NOT be fabricated from the snapshot ts (5000).
  auto old_process = context_.storage->process_table()[old_upid];
  EXPECT_FALSE(old_process.end_ts().has_value());

  // New process start_ts is also unset.
  auto new_process = context_.storage->process_table()[*opt_upid];
  EXPECT_FALSE(new_process.start_ts().has_value());

  // Stat must be incremented.
  EXPECT_EQ(context_.global_stats_tracker->GetStats(
                context_.machine_id(), context_.trace_id(),
                stats::android_process_state_reuse_unknown_ts),
            1);
}

TEST_F(AndroidProcessStateTrackerTest, InvalidStartTimeMsOverflow) {
  tracker_->OnConfigDetected(/*ftrace_configured=*/false,
                             /*dump_process_metadata=*/true);

  protozero::HeapBuffered<fb::AndroidProcessStateSnapshot> snapshot;
  auto* rec = snapshot->add_record();
  rec->set_pid(2001);
  rec->set_process_name("com.test.overflow");
  rec->set_start_seq_id(1);
  // Causes overflow when multiplied by 1,000,000.
  rec->set_start_time_ms(std::numeric_limits<int64_t>::max() / 100);

  std::string bytes = snapshot.SerializeAsString();
  tracker_->ParseProcessStateDump(
      1000, protozero::ConstBytes{
                reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()});

  EXPECT_EQ(context_.global_stats_tracker->GetStats(
                context_.machine_id(), context_.trace_id(),
                stats::android_process_state_invalid_start_time),
            1);
}

TEST_F(AndroidProcessStateTrackerTest, ReconcileKernelAuthority) {
  // Both ftrace and dump_process_metadata enabled -> kKernelAuthority.
  tracker_->OnConfigDetected(/*ftrace_configured=*/true,
                             /*dump_process_metadata=*/true);

  protozero::HeapBuffered<fb::AndroidProcessStateSnapshot> snapshot;
  auto* rec = snapshot->add_record();
  rec->set_pid(3001);
  rec->set_uid(10070);
  rec->set_process_name("com.test.kernel");
  rec->set_start_seq_id(77);
  rec->set_proc_state(1002);
  rec->set_oom_score(200);

  std::string bytes = snapshot.SerializeAsString();
  tracker_->ParseProcessStateDump(
      1000, protozero::ConstBytes{
                reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()});

  tracker_->Finalize();

  auto opt_upid = context_.process_tracker->GetProcessOrNull(3001);
  ASSERT_TRUE(opt_upid.has_value());
  auto process = context_.storage->process_table()[*opt_upid];
  EXPECT_EQ(process.pid(), 3001u);
  // Under Kernel Authority, snapshot records do not enrich core process table
  // metadata.
  EXPECT_FALSE(process.name().has_value());
  EXPECT_FALSE(
      context_.android_process_tracker->GetStartSeqId(*opt_upid).has_value());

  EXPECT_EQ(process_state_table_->row_count(), 1u);
  auto state_row = (*process_state_table_)[0];
  EXPECT_EQ(state_row.upid(), *opt_upid);
  EXPECT_EQ(state_row.is_initial(), 1u);
  EXPECT_EQ(state_row.oom_score(), 200);
}

}  // namespace
}  // namespace perfetto::trace_processor::android_process_state
