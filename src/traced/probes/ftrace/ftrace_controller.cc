/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include "src/traced/probes/ftrace/ftrace_controller.h"

#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/metatrace.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/tracing/core/trace_writer.h"
#include "src/kallsyms/kernel_symbol_map.h"
#include "src/kallsyms/lazy_kernel_symbolizer.h"
#include "src/traced/probes/ftrace/atrace_hal_wrapper.h"
#include "src/traced/probes/ftrace/cpu_reader.h"
#include "src/traced/probes/ftrace/cpu_stats_parser.h"
#include "src/traced/probes/ftrace/event_info.h"
#include "src/traced/probes/ftrace/ftrace_config_muxer.h"
#include "src/traced/probes/ftrace/ftrace_data_source.h"
#include "src/traced/probes/ftrace/ftrace_metadata.h"
#include "src/traced/probes/ftrace/ftrace_procfs.h"
#include "src/traced/probes/ftrace/ftrace_stats.h"
#include "src/traced/probes/ftrace/proto_translation_table.h"
#include "src/traced/probes/ftrace/vendor_tracepoints.h"

namespace perfetto {
namespace {

constexpr int kDefaultDrainPeriodMs = 100;
constexpr int kMinDrainPeriodMs = 1;
constexpr int kMaxDrainPeriodMs = 1000 * 60;

// Read at most this many pages of data per cpu per read task. If we hit this
// limit on at least one cpu, we stop and repost the read task, letting other
// tasks get some cpu time before continuing reading.
constexpr size_t kMaxPagesPerCpuPerReadTick = 256;  // 1 MB per cpu

uint32_t ClampDrainPeriodMs(uint32_t drain_period_ms) {
  if (drain_period_ms == 0) {
    return kDefaultDrainPeriodMs;
  }
  if (drain_period_ms < kMinDrainPeriodMs ||
      kMaxDrainPeriodMs < drain_period_ms) {
    PERFETTO_LOG("drain_period_ms was %u should be between %u and %u",
                 drain_period_ms, kMinDrainPeriodMs, kMaxDrainPeriodMs);
    return kDefaultDrainPeriodMs;
  }
  return drain_period_ms;
}

bool WriteToFile(const char* path, const char* str) {
  auto fd = base::OpenFile(path, O_WRONLY);
  if (!fd)
    return false;
  const size_t str_len = strlen(str);
  return base::WriteAll(*fd, str, str_len) == static_cast<ssize_t>(str_len);
}

bool ClearFile(const char* path) {
  auto fd = base::OpenFile(path, O_WRONLY | O_TRUNC);
  return !!fd;
}

std::optional<int64_t> ReadFtraceNowTs(const base::ScopedFile& cpu_stats_fd) {
  PERFETTO_CHECK(cpu_stats_fd);

  char buf[512];
  ssize_t res = PERFETTO_EINTR(pread(*cpu_stats_fd, buf, sizeof(buf) - 1, 0));
  if (res <= 0)
    return std::nullopt;
  buf[res] = '\0';

  FtraceCpuStats stats{};
  DumpCpuStats(buf, &stats);
  return static_cast<int64_t>(stats.now_ts * 1000 * 1000 * 1000);
}

std::map<std::string, std::vector<GroupAndName>> GetAtraceVendorEvents(
    FtraceProcfs* tracefs) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
  if (base::FileExists(vendor_tracepoints::kCategoriesFile)) {
    std::map<std::string, std::vector<GroupAndName>> vendor_evts;
    base::Status status =
        vendor_tracepoints::DiscoverAccessibleVendorTracepointsWithFile(
            vendor_tracepoints::kCategoriesFile, &vendor_evts, tracefs);
    if (!status.ok()) {
      PERFETTO_ELOG("Cannot load vendor categories: %s", status.c_message());
    }
    return vendor_evts;
  } else {
    AtraceHalWrapper hal;
    return vendor_tracepoints::DiscoverVendorTracepointsWithHal(&hal, tracefs);
  }
#else
  base::ignore_result(tracefs);
  return {};
#endif
}

}  // namespace

// Method of last resort to reset ftrace state.
// We don't know what state the rest of the system and process is so as far
// as possible avoid allocations.
bool HardResetFtraceState() {
  for (const char* const* item = FtraceProcfs::kTracingPaths; *item; ++item) {
    std::string prefix(*item);
    PERFETTO_CHECK(base::EndsWith(prefix, "/"));
    bool res = true;
    res &= WriteToFile((prefix + "tracing_on").c_str(), "0");
    res &= WriteToFile((prefix + "buffer_size_kb").c_str(), "4");
    // Not checking success because these files might not be accessible on
    // older or release builds of Android:
    WriteToFile((prefix + "events/enable").c_str(), "0");
    WriteToFile((prefix + "events/raw_syscalls/filter").c_str(), "0");
    WriteToFile((prefix + "current_tracer").c_str(), "nop");
    res &= ClearFile((prefix + "trace").c_str());
    if (res)
      return true;
  }
  return false;
}

// static
std::unique_ptr<FtraceController> FtraceController::Create(
    base::TaskRunner* runner,
    Observer* observer) {
  std::unique_ptr<FtraceProcfs> ftrace_procfs =
      FtraceProcfs::CreateGuessingMountPoint("");
  if (!ftrace_procfs)
    return nullptr;

  std::unique_ptr<ProtoTranslationTable> table = ProtoTranslationTable::Create(
      ftrace_procfs.get(), GetStaticEventInfo(), GetStaticCommonFieldsInfo());
  if (!table)
    return nullptr;

  std::map<std::string, std::vector<GroupAndName>> vendor_evts =
      GetAtraceVendorEvents(ftrace_procfs.get());

  SyscallTable syscalls = SyscallTable::FromCurrentArch();

  auto muxer = std::make_unique<FtraceConfigMuxer>(
      ftrace_procfs.get(), table.get(), std::move(syscalls), vendor_evts);
  return std::unique_ptr<FtraceController>(
      new FtraceController(std::move(ftrace_procfs), std::move(table),
                           std::move(muxer), runner, observer));
}

FtraceController::FtraceController(std::unique_ptr<FtraceProcfs> ftrace_procfs,
                                   std::unique_ptr<ProtoTranslationTable> table,
                                   std::unique_ptr<FtraceConfigMuxer> muxer,
                                   base::TaskRunner* task_runner,
                                   Observer* observer)
    : task_runner_(task_runner),
      observer_(observer),
      primary_(std::move(ftrace_procfs), std::move(table), std::move(muxer)),
      weak_factory_(this) {}

FtraceController::~FtraceController() {
  while (!data_sources_.empty()) {
    RemoveDataSource(*data_sources_.begin());
  }
  PERFETTO_DCHECK(data_sources_.empty());
  PERFETTO_DCHECK(primary_.started_data_sources.empty());
  PERFETTO_DCHECK(primary_.per_cpu.empty());
  PERFETTO_DCHECK(secondary_instances_.empty());
}

uint64_t FtraceController::NowMs() const {
  return static_cast<uint64_t>(base::GetWallTimeMs().count());
}

void FtraceController::StartIfNeeded(FtraceInstanceState* instance) {
  using FtraceClock = protos::pbzero::FtraceClock;
  if (instance->started_data_sources.size() > 1)
    return;

  // Lazily allocate the memory used for reading & parsing ftrace. In the case
  // of multiple ftrace instances, this might already be valid.
  parsing_mem_.AllocateIfNeeded();

  PERFETTO_DCHECK(instance->per_cpu.empty());
  size_t num_cpus = instance->ftrace_procfs->NumberOfCpus();
  const auto ftrace_clock = instance->ftrace_config_muxer->ftrace_clock();
  instance->per_cpu.clear();
  instance->per_cpu.reserve(num_cpus);
  size_t period_page_quota =
      instance->ftrace_config_muxer->GetPerCpuBufferSizePages();
  for (size_t cpu = 0; cpu < num_cpus; cpu++) {
    auto reader = std::make_unique<CpuReader>(
        cpu, instance->ftrace_procfs->OpenPipeForCpu(cpu),
        instance->table.get(), &symbolizer_, ftrace_clock,
        &ftrace_clock_snapshot_);
    instance->per_cpu.emplace_back(std::move(reader), period_page_quota);
  }

  // Special case for primary instance: if not using the boot clock, take
  // manual clock snapshots so that the trace parser can do a best effort
  // conversion back to boot. This is primarily for old kernels that predate
  // boot support, and therefore default to "global" clock.
  if (instance == &primary_ &&
      ftrace_clock != FtraceClock::FTRACE_CLOCK_UNSPECIFIED) {
    cpu_zero_stats_fd_ = primary_.ftrace_procfs->OpenCpuStats(0 /* cpu */);
    MaybeSnapshotFtraceClock();
  }

  // Start a new repeating read task (even if there is already one posted due
  // to a different ftrace instance). Any old tasks will stop due to generation
  // checks.
  auto generation = ++generation_;
  auto drain_period_ms = GetDrainPeriodMs();
  auto weak_this = weak_factory_.GetWeakPtr();
  task_runner_->PostDelayedTask(
      [weak_this, generation] {
        if (weak_this)
          weak_this->ReadTick(generation);
      },
      drain_period_ms - (NowMs() % drain_period_ms));
}

// We handle the ftrace buffers in a repeating task (ReadTick). On a given tick,
// we iterate over all per-cpu buffers, parse their contents, and then write out
// the serialized packets. This is handled by |CpuReader| instances, which
// attempt to read from their respective per-cpu buffer fd until they catch up
// to the head of the buffer, or hit a transient error.
//
// The readers work in batches of |kParsingBufferSizePages| pages for cache
// locality, and to limit memory usage.
//
// However, the reading happens on the primary thread, shared with the rest of
// the service (including ipc). If there is a lot of ftrace data to read, we
// want to yield to the event loop, re-enqueueing a continuation task at the end
// of the immediate queue (letting other enqueued tasks to run before
// continuing). Therefore we introduce |kMaxPagesPerCpuPerReadTick|.
//
// There is also a possibility that the ftrace bandwidth is particularly high.
// We do not want to continue trying to catch up to the event stream (via
// continuation tasks) without bound, as we want to limit our cpu% usage.  We
// assume that given a config saying "per-cpu kernel ftrace buffer is N pages,
// and drain every T milliseconds", we should not read more than N pages per
// drain period. Therefore we introduce |per_cpu.period_page_quota|. If the
// consumer wants to handle a high bandwidth of ftrace events, they should set
// the config values appropriately.
void FtraceController::ReadTick(int generation) {
  metatrace::ScopedEvent evt(metatrace::TAG_FTRACE,
                             metatrace::FTRACE_READ_TICK);
  if (generation != generation_ || GetStartedDataSourcesCount() == 0) {
    return;
  }

  // Read all cpu buffers with remaining per-period quota.
  bool all_cpus_done = ReadTickForInstance(&primary_);
  for (auto& kv : secondary_instances_) {
    all_cpus_done &= ReadTickForInstance(kv.second.get());
  }

  observer_->OnFtraceDataWrittenIntoDataSourceBuffers();

  // More work to do in this period.
  auto weak_this = weak_factory_.GetWeakPtr();
  if (!all_cpus_done) {
    PERFETTO_DLOG("Reposting immediate ReadTick as there's more work.");
    task_runner_->PostTask([weak_this, generation] {
      if (weak_this)
        weak_this->ReadTick(generation);
    });
  } else {
    // Done until next drain period.
    size_t period_page_quota =
        primary_.ftrace_config_muxer->GetPerCpuBufferSizePages();
    for (auto& per_cpu : primary_.per_cpu)
      per_cpu.period_page_quota = period_page_quota;

    for (auto& it : secondary_instances_) {
      FtraceInstanceState* instance = it.second.get();
      size_t quota = instance->ftrace_config_muxer->GetPerCpuBufferSizePages();
      for (auto& per_cpu : instance->per_cpu) {
        per_cpu.period_page_quota = quota;
      }
    }

    // Snapshot the clock so the data in the next period will be clock synced as
    // well.
    MaybeSnapshotFtraceClock();

    auto drain_period_ms = GetDrainPeriodMs();
    task_runner_->PostDelayedTask(
        [weak_this, generation] {
          if (weak_this)
            weak_this->ReadTick(generation);
        },
        drain_period_ms - (NowMs() % drain_period_ms));
  }
}

bool FtraceController::ReadTickForInstance(FtraceInstanceState* instance) {
  if (instance->started_data_sources.empty())
    return true;

#if PERFETTO_DCHECK_IS_ON()
  // The OnFtraceDataWrittenIntoDataSourceBuffers() below is supposed to clear
  // all metadata, including the |kernel_addrs| map for symbolization.
  for (FtraceDataSource* ds : instance->started_data_sources) {
    FtraceMetadata* ftrace_metadata = ds->mutable_metadata();
    PERFETTO_DCHECK(ftrace_metadata->kernel_addrs.empty());
    PERFETTO_DCHECK(ftrace_metadata->last_kernel_addr_index_written == 0);
  }
#endif

  bool all_cpus_done = true;
  for (size_t i = 0; i < instance->per_cpu.size(); i++) {
    size_t orig_quota = instance->per_cpu[i].period_page_quota;
    if (orig_quota == 0)
      continue;

    size_t max_pages = std::min(orig_quota, kMaxPagesPerCpuPerReadTick);
    CpuReader& cpu_reader = *instance->per_cpu[i].reader;
    size_t pages_read = cpu_reader.ReadCycle(&parsing_mem_, max_pages,
                                             instance->started_data_sources);

    size_t new_quota = (pages_read >= orig_quota) ? 0 : orig_quota - pages_read;
    instance->per_cpu[i].period_page_quota = new_quota;

    // Reader got stopped by the cap on the number of pages (to not do too much
    // work on the shared thread at once), but can read more in this drain
    // period. Repost the ReadTick (on the immediate queue) to iterate over all
    // cpus again. In other words, we will keep reposting work for all cpus as
    // long as at least one of them hits the read page cap each tick. If all
    // readers catch up to the event stream (pages_read < max_pages), or exceed
    // their quota, we will stop for the given period.
    PERFETTO_DCHECK(pages_read <= max_pages);
    if (pages_read == max_pages && new_quota > 0) {
      all_cpus_done = false;
    }
  }
  return all_cpus_done;
}

uint32_t FtraceController::GetDrainPeriodMs() {
  if (data_sources_.empty())
    return kDefaultDrainPeriodMs;
  uint32_t min_drain_period_ms = kMaxDrainPeriodMs + 1;
  for (const FtraceDataSource* data_source : data_sources_) {
    if (data_source->config().drain_period_ms() < min_drain_period_ms)
      min_drain_period_ms = data_source->config().drain_period_ms();
  }
  return ClampDrainPeriodMs(min_drain_period_ms);
}

void FtraceController::Flush(FlushRequestID flush_id) {
  metatrace::ScopedEvent evt(metatrace::TAG_FTRACE,
                             metatrace::FTRACE_CPU_FLUSH);

  FlushForInstance(&primary_);
  for (auto& it : secondary_instances_) {
    FlushForInstance(it.second.get());
  }

  observer_->OnFtraceDataWrittenIntoDataSourceBuffers();

  for (FtraceDataSource* data_source : primary_.started_data_sources) {
    data_source->OnFtraceFlushComplete(flush_id);
  }
  for (auto& kv : secondary_instances_) {
    for (FtraceDataSource* data_source : kv.second->started_data_sources) {
      data_source->OnFtraceFlushComplete(flush_id);
    }
  }
}

void FtraceController::FlushForInstance(FtraceInstanceState* instance) {
  if (instance->started_data_sources.empty())
    return;

  // Read all cpus in one go, limiting the per-cpu read amount to make sure we
  // don't get stuck chasing the writer if there's a very high bandwidth of
  // events.
  size_t per_cpubuf_size_pages =
      instance->ftrace_config_muxer->GetPerCpuBufferSizePages();
  for (size_t i = 0; i < instance->per_cpu.size(); i++) {
    instance->per_cpu[i].reader->ReadCycle(&parsing_mem_, per_cpubuf_size_pages,
                                           instance->started_data_sources);
  }
}

// We are not implicitly flushing on Stop. The tracing service is supposed to
// ask for an explicit flush before stopping, unless it needs to perform a
// non-graceful stop.
void FtraceController::StopIfNeeded(FtraceInstanceState* instance) {
  if (!instance->started_data_sources.empty())
    return;

  instance->per_cpu.clear();
  if (instance == &primary_) {
    cpu_zero_stats_fd_.reset();
  }
  // Muxer cannot change the current_tracer until we close the trace pipe fds
  // (i.e. per_cpu). Hence an explicit request here.
  instance->ftrace_config_muxer->ResetCurrentTracer();

  DestroyIfUnusedSeconaryInstance(instance);

  // Clean up global state if done with all data sources.
  if (!data_sources_.empty())
    return;

  if (!retain_ksyms_on_stop_) {
    symbolizer_.Destroy();
  }
  retain_ksyms_on_stop_ = false;

  // Note: might have never been allocated if data sources were rejected.
  parsing_mem_.Release();
}

bool FtraceController::AddDataSource(FtraceDataSource* data_source) {
  if (!ValidConfig(data_source->config()))
    return false;

  FtraceInstanceState* instance =
      GetOrCreateInstance(data_source->config().instance_name());
  if (!instance)
    return false;

  // note: from this point onwards, need to not leak a possibly created
  // instance if returning early.

  FtraceConfigId config_id = next_cfg_id_++;
  if (!instance->ftrace_config_muxer->SetupConfig(
          config_id, data_source->config(),
          data_source->mutable_setup_errors())) {
    DestroyIfUnusedSeconaryInstance(instance);
    return false;
  }

  const FtraceDataSourceConfig* ds_config =
      instance->ftrace_config_muxer->GetDataSourceConfig(config_id);
  auto it_and_inserted = data_sources_.insert(data_source);
  PERFETTO_DCHECK(it_and_inserted.second);
  data_source->Initialize(config_id, ds_config);
  return true;
}

bool FtraceController::StartDataSource(FtraceDataSource* data_source) {
  PERFETTO_DCHECK(data_sources_.count(data_source) > 0);

  FtraceConfigId config_id = data_source->config_id();
  PERFETTO_CHECK(config_id);

  FtraceInstanceState* instance =
      GetOrCreateInstance(data_source->config().instance_name());
  PERFETTO_CHECK(instance);

  if (!instance->ftrace_config_muxer->ActivateConfig(config_id))
    return false;
  instance->started_data_sources.insert(data_source);
  StartIfNeeded(instance);

  // Parse kernel symbols if required by the config. This can be an expensive
  // operation (cpu-bound for 500ms+), so delay the StartDataSource
  // acknowledgement until after we're done. This lets a consumer wait for the
  // expensive work to be done by waiting on the "all data sources started"
  // fence. This helps isolate the effects of the cpu-bound work on
  // frequency scaling of cpus when recording benchmarks (b/236143653).
  // Note that we're already recording data into the kernel ftrace
  // buffers while doing the symbol parsing.
  if (data_source->config().symbolize_ksyms()) {
    symbolizer_.GetOrCreateKernelSymbolMap();
    // If at least one config sets the KSYMS_RETAIN flag, keep the ksysm map
    // around in StopIfNeeded().
    const auto KRET = FtraceConfig::KSYMS_RETAIN;
    retain_ksyms_on_stop_ |= data_source->config().ksyms_mem_policy() == KRET;
  }

  return true;
}

void FtraceController::RemoveDataSource(FtraceDataSource* data_source) {
  size_t removed = data_sources_.erase(data_source);
  if (!removed)
    return;  // can happen if AddDataSource failed

  FtraceInstanceState* instance =
      GetOrCreateInstance(data_source->config().instance_name());
  PERFETTO_CHECK(instance);

  instance->ftrace_config_muxer->RemoveConfig(data_source->config_id());
  instance->started_data_sources.erase(data_source);
  StopIfNeeded(instance);
}

void FtraceController::DumpFtraceStats(FtraceDataSource* data_source,
                                       FtraceStats* stats_out) {
  FtraceInstanceState* instance =
      GetInstance(data_source->config().instance_name());
  PERFETTO_DCHECK(instance);
  if (!instance)
    return;

  DumpAllCpuStats(instance->ftrace_procfs.get(), stats_out);
  if (symbolizer_.is_valid()) {
    auto* symbol_map = symbolizer_.GetOrCreateKernelSymbolMap();
    stats_out->kernel_symbols_parsed =
        static_cast<uint32_t>(symbol_map->num_syms());
    stats_out->kernel_symbols_mem_kb =
        static_cast<uint32_t>(symbol_map->size_bytes() / 1024);
  }
}

void FtraceController::MaybeSnapshotFtraceClock() {
  if (!cpu_zero_stats_fd_)
    return;

  auto ftrace_clock = primary_.ftrace_config_muxer->ftrace_clock();
  PERFETTO_DCHECK(ftrace_clock != protos::pbzero::FTRACE_CLOCK_UNSPECIFIED);

  // Snapshot the boot clock *before* reading CPU stats so that
  // two clocks are as close togher as possible (i.e. if it was the
  // other way round, we'd skew by the const of string parsing).
  ftrace_clock_snapshot_.boot_clock_ts = base::GetBootTimeNs().count();

  // A value of zero will cause this snapshot to be skipped.
  ftrace_clock_snapshot_.ftrace_clock_ts =
      ReadFtraceNowTs(cpu_zero_stats_fd_).value_or(0);
}

size_t FtraceController::GetStartedDataSourcesCount() const {
  size_t cnt = primary_.started_data_sources.size();
  for (auto& it : secondary_instances_) {
    cnt += it.second->started_data_sources.size();
  }
  return cnt;
}

FtraceController::FtraceInstanceState::FtraceInstanceState(
    std::unique_ptr<FtraceProcfs> ft,
    std::unique_ptr<ProtoTranslationTable> ptt,
    std::unique_ptr<FtraceConfigMuxer> fcm)
    : ftrace_procfs(std::move(ft)),
      table(std::move(ptt)),
      ftrace_config_muxer(std::move(fcm)) {}

FtraceController::FtraceInstanceState* FtraceController::GetOrCreateInstance(
    const std::string& instance_name) {
  FtraceInstanceState* maybe_existing = GetInstance(instance_name);
  if (maybe_existing)
    return maybe_existing;

  PERFETTO_DCHECK(!instance_name.empty());
  std::unique_ptr<FtraceInstanceState> instance =
      CreateSecondaryInstance(instance_name);
  if (!instance)
    return nullptr;

  auto it_and_inserted = secondary_instances_.emplace(
      std::piecewise_construct, std::forward_as_tuple(instance_name),
      std::forward_as_tuple(std::move(instance)));
  PERFETTO_CHECK(it_and_inserted.second);
  return it_and_inserted.first->second.get();
}

FtraceController::FtraceInstanceState* FtraceController::GetInstance(
    const std::string& instance_name) {
  if (instance_name.empty())
    return &primary_;

  auto it = secondary_instances_.find(instance_name);
  return it != secondary_instances_.end() ? it->second.get() : nullptr;
}

void FtraceController::DestroyIfUnusedSeconaryInstance(
    FtraceInstanceState* instance) {
  if (instance == &primary_)
    return;
  for (auto it = secondary_instances_.begin(); it != secondary_instances_.end();
       ++it) {
    if (it->second.get() == instance &&
        instance->ftrace_config_muxer->GetDataSourcesCount() == 0) {
      // no data sources left referencing this secondary instance
      secondary_instances_.erase(it);
      return;
    }
  }
  PERFETTO_FATAL("Bug in ftrace instance lifetimes");
}

// TODO(rsavitski): dedupe with FtraceController::Create.
std::unique_ptr<FtraceController::FtraceInstanceState>
FtraceController::CreateSecondaryInstance(const std::string& instance_name) {
  std::optional<std::string> instance_path = AbsolutePathForInstance(
      primary_.ftrace_procfs->GetRootPath(), instance_name);
  if (!instance_path.has_value()) {
    PERFETTO_ELOG("Invalid ftrace instance name: \"%s\"",
                  instance_name.c_str());
    return nullptr;
  }

  auto ftrace_procfs = FtraceProcfs::Create(*instance_path);
  if (!ftrace_procfs) {
    PERFETTO_ELOG("Failed to create ftrace procfs for \"%s\"",
                  instance_path->c_str());
    return nullptr;
  }

  auto table = ProtoTranslationTable::Create(
      ftrace_procfs.get(), GetStaticEventInfo(), GetStaticCommonFieldsInfo());
  if (!table) {
    PERFETTO_ELOG("Failed to create proto translation table for \"%s\"",
                  instance_path->c_str());
    return nullptr;
  }

  // secondary instances don't support atrace and vendor tracepoint HAL
  std::map<std::string, std::vector<GroupAndName>> vendor_evts;

  auto syscalls = SyscallTable::FromCurrentArch();

  auto muxer = std::make_unique<FtraceConfigMuxer>(
      ftrace_procfs.get(), table.get(), std::move(syscalls), vendor_evts,
      /* secondary_instance= */ true);
  return std::make_unique<FtraceInstanceState>(
      std::move(ftrace_procfs), std::move(table), std::move(muxer));
}

// TODO(rsavitski): we want to eventually add support for the default
// (primary_) tracefs path to be an instance itself, at which point we'll need
// to be careful to distinguish the tracefs mount point from the default
// instance path.
// static
std::optional<std::string> FtraceController::AbsolutePathForInstance(
    const std::string& tracefs_root,
    const std::string& raw_cfg_name) {
  if (base::Contains(raw_cfg_name, '/') ||
      base::StartsWith(raw_cfg_name, "..")) {
    return std::nullopt;
  }

  // ARM64 pKVM hypervisor tracing emulates an instance, but is not under
  // instances/, we special-case that name for now.
  if (raw_cfg_name == "hyp") {
    std::string hyp_path = tracefs_root + "hyp/";
    PERFETTO_LOG(
        "Config specified reserved \"hyp\" instance name, using %s for events.",
        hyp_path.c_str());
    return std::make_optional(hyp_path);
  }

  return tracefs_root + "instances/" + raw_cfg_name + "/";
}

FtraceController::Observer::~Observer() = default;

}  // namespace perfetto
