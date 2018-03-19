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

#include <inttypes.h>

#include <stdio.h>
#include <sys/sysmacros.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <istream>
#include <map>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/text_format.h>
#include <google/protobuf/util/field_comparator.h>
#include <google/protobuf/util/message_differencer.h>

#include "perfetto/base/logging.h"
#include "perfetto/trace/trace.pb.h"
#include "perfetto/trace/trace_packet.pb.h"

namespace perfetto {
namespace {

const char kTraceHeader[] = R"({
  "traceEvents": [],
)";

const char kTraceFooter[] = R"(\n",
  "controllerTraceDataKey": "systraceController"
})";

const char kFtraceHeader[] =
    ""
    "  \"systemTraceEvents\": \""
    "# tracer: nop\\n"
    "#\\n"
    "# entries-in-buffer/entries-written: 30624/30624   #P:4\\n"
    "#\\n"
    "#                                      _-----=> irqs-off\\n"
    "#                                     / _----=> need-resched\\n"
    "#                                    | / _---=> hardirq/softirq\\n"
    "#                                    || / _--=> preempt-depth\\n"
    "#                                    ||| /     delay\\n"
    "#           TASK-PID    TGID   CPU#  ||||    TIMESTAMP  FUNCTION\\n"
    "#              | |        |      |   ||||       |         |\\n";

using google::protobuf::Descriptor;
using google::protobuf::DynamicMessageFactory;
using google::protobuf::FileDescriptor;
using google::protobuf::Message;
using google::protobuf::TextFormat;
using google::protobuf::compiler::DiskSourceTree;
using google::protobuf::compiler::Importer;
using google::protobuf::compiler::MultiFileErrorCollector;
using google::protobuf::io::OstreamOutputStream;

using protos::BinderLockFtraceEvent;
using protos::BinderLockedFtraceEvent;
using protos::BinderSetPriorityFtraceEvent;
using protos::BinderTransactionFtraceEvent;
using protos::BinderTransactionReceivedFtraceEvent;
using protos::BinderUnlockFtraceEvent;
using protos::BlockBioBackmergeFtraceEvent;
using protos::BlockBioBounceFtraceEvent;
using protos::BlockBioCompleteFtraceEvent;
using protos::BlockBioFrontmergeFtraceEvent;
using protos::BlockBioQueueFtraceEvent;
using protos::BlockBioRemapFtraceEvent;
using protos::BlockDirtyBufferFtraceEvent;
using protos::BlockGetrqFtraceEvent;
using protos::BlockPlugFtraceEvent;
using protos::BlockRqAbortFtraceEvent;
using protos::BlockRqCompleteFtraceEvent;
using protos::BlockRqInsertFtraceEvent;
using protos::BlockRqIssueFtraceEvent;
using protos::BlockRqRemapFtraceEvent;
using protos::BlockRqRequeueFtraceEvent;
using protos::BlockSleeprqFtraceEvent;
using protos::BlockSplitFtraceEvent;
using protos::BlockTouchBufferFtraceEvent;
using protos::BlockUnplugFtraceEvent;
using protos::CgroupAttachTaskFtraceEvent;
using protos::CgroupDestroyRootFtraceEvent;
using protos::CgroupMkdirFtraceEvent;
using protos::CgroupReleaseFtraceEvent;
using protos::CgroupRemountFtraceEvent;
using protos::CgroupRenameFtraceEvent;
using protos::CgroupRmdirFtraceEvent;
using protos::CgroupSetupRootFtraceEvent;
using protos::CgroupTransferTasksFtraceEvent;
using protos::MmCompactionBeginFtraceEvent;
using protos::MmCompactionDeferCompactionFtraceEvent;
using protos::MmCompactionDeferResetFtraceEvent;
using protos::MmCompactionDeferredFtraceEvent;
using protos::MmCompactionEndFtraceEvent;
using protos::MmCompactionFinishedFtraceEvent;
using protos::MmCompactionIsolateFreepagesFtraceEvent;
using protos::MmCompactionIsolateMigratepagesFtraceEvent;
using protos::MmCompactionKcompactdSleepFtraceEvent;
using protos::MmCompactionKcompactdWakeFtraceEvent;
using protos::MmCompactionMigratepagesFtraceEvent;
using protos::MmCompactionSuitableFtraceEvent;
using protos::MmCompactionTryToCompactPagesFtraceEvent;
using protos::MmCompactionWakeupKcompactdFtraceEvent;
using protos::CpufreqInteractiveAlreadyFtraceEvent;
using protos::CpufreqInteractiveBoostFtraceEvent;
using protos::CpufreqInteractiveNotyetFtraceEvent;
using protos::CpufreqInteractiveSetspeedFtraceEvent;
using protos::CpufreqInteractiveTargetFtraceEvent;
using protos::CpufreqInteractiveUnboostFtraceEvent;
using protos::Ext4AllocDaBlocksFtraceEvent;
using protos::Ext4AllocateBlocksFtraceEvent;
using protos::Ext4AllocateInodeFtraceEvent;
using protos::Ext4BeginOrderedTruncateFtraceEvent;
using protos::Ext4CollapseRangeFtraceEvent;
using protos::Ext4DaReleaseSpaceFtraceEvent;
using protos::Ext4DaReserveSpaceFtraceEvent;
using protos::Ext4DaUpdateReserveSpaceFtraceEvent;
using protos::Ext4DaWriteBeginFtraceEvent;
using protos::Ext4DaWriteEndFtraceEvent;
using protos::Ext4DaWritePagesFtraceEvent;
using protos::Ext4DaWritePagesExtentFtraceEvent;
using protos::Ext4DirectIOEnterFtraceEvent;
using protos::Ext4DirectIOExitFtraceEvent;
using protos::Ext4DiscardBlocksFtraceEvent;
using protos::Ext4DiscardPreallocationsFtraceEvent;
using protos::Ext4DropInodeFtraceEvent;
using protos::Ext4EsCacheExtentFtraceEvent;
using protos::Ext4EsFindDelayedExtentRangeEnterFtraceEvent;
using protos::Ext4EsFindDelayedExtentRangeExitFtraceEvent;
using protos::Ext4EsInsertExtentFtraceEvent;
using protos::Ext4EsLookupExtentEnterFtraceEvent;
using protos::Ext4EsLookupExtentExitFtraceEvent;
using protos::Ext4EsRemoveExtentFtraceEvent;
using protos::Ext4EsShrinkFtraceEvent;
using protos::Ext4EsShrinkCountFtraceEvent;
using protos::Ext4EsShrinkScanEnterFtraceEvent;
using protos::Ext4EsShrinkScanExitFtraceEvent;
using protos::Ext4EvictInodeFtraceEvent;
using protos::Ext4ExtConvertToInitializedEnterFtraceEvent;
using protos::Ext4ExtConvertToInitializedFastpathFtraceEvent;
using protos::Ext4ExtHandleUnwrittenExtentsFtraceEvent;
using protos::Ext4ExtInCacheFtraceEvent;
using protos::Ext4ExtLoadExtentFtraceEvent;
using protos::Ext4ExtMapBlocksEnterFtraceEvent;
using protos::Ext4ExtMapBlocksExitFtraceEvent;
using protos::Ext4ExtPutInCacheFtraceEvent;
using protos::Ext4ExtRemoveSpaceFtraceEvent;
using protos::Ext4ExtRemoveSpaceDoneFtraceEvent;
using protos::Ext4ExtRmIdxFtraceEvent;
using protos::Ext4ExtRmLeafFtraceEvent;
using protos::Ext4ExtShowExtentFtraceEvent;
using protos::Ext4FallocateEnterFtraceEvent;
using protos::Ext4FallocateExitFtraceEvent;
using protos::Ext4FindDelallocRangeFtraceEvent;
using protos::Ext4ForgetFtraceEvent;
using protos::Ext4FreeBlocksFtraceEvent;
using protos::Ext4FreeInodeFtraceEvent;
using protos::Ext4GetImpliedClusterAllocExitFtraceEvent;
using protos::Ext4GetReservedClusterAllocFtraceEvent;
using protos::Ext4IndMapBlocksEnterFtraceEvent;
using protos::Ext4IndMapBlocksExitFtraceEvent;
using protos::Ext4InsertRangeFtraceEvent;
using protos::Ext4InvalidatepageFtraceEvent;
using protos::Ext4JournalStartFtraceEvent;
using protos::Ext4JournalStartReservedFtraceEvent;
using protos::Ext4JournalledInvalidatepageFtraceEvent;
using protos::Ext4JournalledWriteEndFtraceEvent;
using protos::Ext4LoadInodeFtraceEvent;
using protos::Ext4LoadInodeBitmapFtraceEvent;
using protos::Ext4MarkInodeDirtyFtraceEvent;
using protos::Ext4MbBitmapLoadFtraceEvent;
using protos::Ext4MbBuddyBitmapLoadFtraceEvent;
using protos::Ext4MbDiscardPreallocationsFtraceEvent;
using protos::Ext4MbNewGroupPaFtraceEvent;
using protos::Ext4MbNewInodePaFtraceEvent;
using protos::Ext4MbReleaseGroupPaFtraceEvent;
using protos::Ext4MbReleaseInodePaFtraceEvent;
using protos::Ext4MballocAllocFtraceEvent;
using protos::Ext4MballocDiscardFtraceEvent;
using protos::Ext4MballocFreeFtraceEvent;
using protos::Ext4MballocPreallocFtraceEvent;
using protos::Ext4OtherInodeUpdateTimeFtraceEvent;
using protos::Ext4PunchHoleFtraceEvent;
using protos::Ext4ReadBlockBitmapLoadFtraceEvent;
using protos::Ext4ReadpageFtraceEvent;
using protos::Ext4ReleasepageFtraceEvent;
using protos::Ext4RemoveBlocksFtraceEvent;
using protos::Ext4RequestBlocksFtraceEvent;
using protos::Ext4RequestInodeFtraceEvent;
using protos::Ext4SyncFileEnterFtraceEvent;
using protos::Ext4SyncFileExitFtraceEvent;
using protos::Ext4SyncFsFtraceEvent;
using protos::Ext4TrimAllFreeFtraceEvent;
using protos::Ext4TrimExtentFtraceEvent;
using protos::Ext4TruncateEnterFtraceEvent;
using protos::Ext4TruncateExitFtraceEvent;
using protos::Ext4UnlinkEnterFtraceEvent;
using protos::Ext4UnlinkExitFtraceEvent;
using protos::Ext4WriteBeginFtraceEvent;
using protos::Ext4WriteEndFtraceEvent;
using protos::Ext4WritepageFtraceEvent;
using protos::Ext4WritepagesFtraceEvent;
using protos::Ext4WritepagesResultFtraceEvent;
using protos::Ext4ZeroRangeFtraceEvent;
using protos::MmFilemapAddToPageCacheFtraceEvent;
using protos::MmFilemapDeleteFromPageCacheFtraceEvent;
using protos::PrintFtraceEvent;
using protos::I2cReadFtraceEvent;
using protos::I2cReplyFtraceEvent;
using protos::I2cResultFtraceEvent;
using protos::I2cWriteFtraceEvent;
using protos::SmbusReadFtraceEvent;
using protos::SmbusReplyFtraceEvent;
using protos::SmbusResultFtraceEvent;
using protos::SmbusWriteFtraceEvent;
using protos::IpiEntryFtraceEvent;
using protos::IpiExitFtraceEvent;
using protos::IpiRaiseFtraceEvent;
using protos::IrqHandlerEntryFtraceEvent;
using protos::IrqHandlerExitFtraceEvent;
using protos::SoftirqEntryFtraceEvent;
using protos::SoftirqExitFtraceEvent;
using protos::SoftirqRaiseFtraceEvent;
using protos::LowmemoryKillFtraceEvent;
using protos::MdpCmdKickoffFtraceEvent;
using protos::MdpCmdPingpongDoneFtraceEvent;
using protos::MdpCmdReadptrDoneFtraceEvent;
using protos::MdpCmdReleaseBwFtraceEvent;
using protos::MdpCmdWaitPingpongFtraceEvent;
using protos::MdpCommitFtraceEvent;
using protos::MdpCompareBwFtraceEvent;
using protos::MdpMisrCrcFtraceEvent;
using protos::MdpMixerUpdateFtraceEvent;
using protos::MdpPerfPrefillCalcFtraceEvent;
using protos::MdpPerfSetOtFtraceEvent;
using protos::MdpPerfSetPanicLutsFtraceEvent;
using protos::MdpPerfSetQosLutsFtraceEvent;
using protos::MdpPerfSetWmLevelsFtraceEvent;
using protos::MdpPerfUpdateBusFtraceEvent;
using protos::MdpSsppChangeFtraceEvent;
using protos::MdpSsppSetFtraceEvent;
using protos::MdpTraceCounterFtraceEvent;
using protos::MdpVideoUnderrunDoneFtraceEvent;
using protos::RotatorBwAoAsContextFtraceEvent;
using protos::TracingMarkWriteFtraceEvent;
using protos::ClockDisableFtraceEvent;
using protos::ClockEnableFtraceEvent;
using protos::ClockSetRateFtraceEvent;
using protos::CpuFrequencyFtraceEvent;
using protos::CpuFrequencyLimitsFtraceEvent;
using protos::CpuIdleFtraceEvent;
using protos::SuspendResumeFtraceEvent;
using protos::RegulatorDisableFtraceEvent;
using protos::RegulatorDisableCompleteFtraceEvent;
using protos::RegulatorEnableFtraceEvent;
using protos::RegulatorEnableCompleteFtraceEvent;
using protos::RegulatorEnableDelayFtraceEvent;
using protos::RegulatorSetVoltageFtraceEvent;
using protos::RegulatorSetVoltageCompleteFtraceEvent;
using protos::SchedBlockedReasonFtraceEvent;
using protos::SchedCpuHotplugFtraceEvent;
using protos::SchedProcessExecFtraceEvent;
using protos::SchedProcessExitFtraceEvent;
using protos::SchedProcessForkFtraceEvent;
using protos::SchedProcessFreeFtraceEvent;
using protos::SchedProcessHangFtraceEvent;
using protos::SchedProcessWaitFtraceEvent;
using protos::SchedSwitchFtraceEvent;
using protos::SchedWakeupFtraceEvent;
using protos::SchedWakeupNewFtraceEvent;
using protos::SchedWakingFtraceEvent;
using protos::SyncPtFtraceEvent;
using protos::SyncTimelineFtraceEvent;
using protos::SyncWaitFtraceEvent;
using protos::MmVmscanDirectReclaimBeginFtraceEvent;
using protos::MmVmscanDirectReclaimEndFtraceEvent;
using protos::MmVmscanKswapdSleepFtraceEvent;
using protos::MmVmscanKswapdWakeFtraceEvent;
using protos::WorkqueueActivateWorkFtraceEvent;
using protos::WorkqueueExecuteEndFtraceEvent;
using protos::WorkqueueExecuteStartFtraceEvent;
using protos::WorkqueueQueueWorkFtraceEvent;
using protos::TaskNewtaskFtraceEvent;
using protos::TaskRenameFtraceEvent;

using protos::FtraceEvent;
using protos::FtraceEventBundle;
using protos::InodeFileMap;
using protos::PrintFtraceEvent;
using protos::ProcessTree;
using protos::Trace;
using protos::TracePacket;
using Entry = protos::InodeFileMap::Entry;
using Process = protos::ProcessTree::Process;

// TODO(hjd): Add tests.

class MFE : public MultiFileErrorCollector {
  virtual void AddError(const std::string& filename,
                        int line,
                        int column,
                        const std::string& message) {
    PERFETTO_ELOG("Error %s %d:%d: %s", filename.c_str(), line, column,
                  message.c_str());
  }

  virtual void AddWarning(const std::string& filename,
                          int line,
                          int column,
                          const std::string& message) {
    PERFETTO_ELOG("Error %s %d:%d: %s", filename.c_str(), line, column,
                  message.c_str());
  }
};

const char* GetSchedSwitchFlag(int32_t state) {
  state &= 511;
  if (state & 1)
    return "S";
  if (state & 2)
    return "D";
  if (state & 4)
    return "T";
  if (state & 8)
    return "t";
  if (state & 16)
    return "Z";
  if (state & 32)
    return "X";
  if (state & 64)
    return "x";
  if (state & 128)
    return "W";
  return "R";
}

const char* GetExt4HintFlag(int32_t state) {
  if (state & 0x0001)
    return "HINT_MERGE";
  if (state & 0x0002)
    return "HINT_RESV";
  if (state & 0x0004)
    return "HINT_MDATA";
  if (state & 0x0008)
    return "HINT_FIRST";
  if (state & 0x0010)
    return "HINT_BEST";
  if (state & 0x0020)
    return "HINT_DATA";
  if (state & 0x0040)
    return "HINT_NOPREALLOC";
  if (state & 0x0080)
    return "HINT_GRP_ALLOCE";
  if (state & 0x0100)
    return "HINT_GOAL_ONLY";
  if (state & 0x0200)
    return "HINT_DATA";
  if (state & 0x0400)
    return "HINT_NOPREALLOC";
  if (state & 0x0800)
    return "HINT_GRP_ALLOCE";
  if (state & 0x2000)
    return "HINT_GOAL_ONLY";
  return "";
}

const char* GetExt4FreeBlocksFlag(int32_t state) {
  if (state & 0x0001)
    return "METADATA";
  if (state & 0x0002)
    return "FORGET";
  if (state & 0x0004)
    return "VALIDATED";
  if (state & 0x0008)
    return "NO_QUOTA";
  if (state & 0x0010)
    return "1ST_CLUSTER";
  if (state & 0x0020)
    return "LAST_CLUSTER";
  return "";
}

const char* GetExt4ModeFlag(int32_t state) {
  if (state & 0x01)
    return "KEEP_SIZE";
  if (state & 0x02)
    return "PUNCH_HOLE";
  if (state & 0x04)
    return "NO_HIDE_STALE";
  if (state & 0x08)
    return "COLLAPSE_RANGE";
  if (state & 0x10)
    return "ZERO_RANGE";
  return "";
}

const char* GetExt4ExtFlag(int32_t state) {
  if (state & 0x0001)
    return "CREATE";
  if (state & 0x0002)
    return "UNWRIT";
  if (state & 0x0004)
    return "DEALLOC";
  if (state & 0x0008)
    return "PRE_IO";
  if (state & 0x0010)
    return "CONVERT";
  if (state & 0x0020)
    return "METADATA_NOFAIL";
  if (state & 0x0040)
    return "NO_NORMALIZE";
  if (state & 0x0080)
    return "KEEP_SIZE";
  if (state & 0x0100)
    return "NO_LOCK";
  return "";
}

const char* MmCompactionRetArray[] = {
    "deferred", "skipped",          "continue",          "partial",
    "complete", "no_suitable_page", "not_suitable_zone", "contended"};

const char* MmCompactionSuitableArray[] = {"DMA", "Normal", "Movable"};

const char* SoftirqArray[] = {"HI",      "TIMER",        "NET_TX",  "NET_RX",
                              "BLOCK",   "BLOCK_IOPOLL", "TASKLET", "SCHED",
                              "HRTIMER", "RCU"};

const char* inodeFileTypeArray[] = {"UNKNOWN", "FILE", "DIRECTORY"};

uint64_t TimestampToSeconds(uint64_t timestamp) {
  return timestamp / 1000000000ul;
}

uint64_t TimestampToMicroseconds(uint64_t timestamp) {
  return (timestamp / 1000) % 1000000ul;
}

std::string FormatPrefix(uint64_t timestamp, uint64_t cpu) {
  char line[2048];
  uint64_t seconds = TimestampToSeconds(timestamp);
  uint64_t useconds = TimestampToMicroseconds(timestamp);
  sprintf(line,
          "<idle>-0     (-----) [%03" PRIu64 "] d..3 %" PRIu64 ".%.6" PRIu64
          ": ",
          cpu, seconds, useconds);
  return std::string(line);
}

std::string FormatSchedSwitch(const SchedSwitchFtraceEvent& sched_switch) {
  char line[2048];
  sprintf(line,
          "sched_switch: prev_comm=%s "
          "prev_pid=%d prev_prio=%d prev_state=%s ==> next_comm=%s next_pid=%d "
          "next_prio=%d\\n",
          sched_switch.prev_comm().c_str(), sched_switch.prev_pid(),
          sched_switch.prev_prio(),
          GetSchedSwitchFlag(sched_switch.prev_state()),
          sched_switch.next_comm().c_str(), sched_switch.next_pid(),
          sched_switch.next_prio());
  return std::string(line);
}

std::string FormatSchedWakeup(const SchedWakeupFtraceEvent& sched_wakeup) {
  char line[2048];
  sprintf(line,
          "sched_wakeup: comm=%s "
          "pid=%d prio=%d success=%d target_cpu=%03d\\n",
          sched_wakeup.comm().c_str(), sched_wakeup.pid(), sched_wakeup.prio(),
          sched_wakeup.success(), sched_wakeup.target_cpu());
  return std::string(line);
}

std::string FormatSchedBlockedReason(
    const SchedBlockedReasonFtraceEvent& event) {
  char line[2048];
  sprintf(line, "sched_blocked_reason: pid=%d iowait=%d caller=%llxS\\n",
          event.pid(), event.io_wait(), event.caller());
  return std::string(line);
}

std::string FormatPrint(const PrintFtraceEvent& print) {
  char line[2048];
  std::string msg = print.buf();
  // Remove any newlines in the message. It's not entirely clear what the right
  // behaviour is here. Maybe we should escape them instead?
  msg.erase(std::remove(msg.begin(), msg.end(), '\n'), msg.end());
  sprintf(line, "tracing_mark_write: %s\\n", msg.c_str());
  return std::string(line);
}

std::string FormatCpuFrequency(const CpuFrequencyFtraceEvent& event) {
  char line[2048];
  sprintf(line, "cpu_frequency: state=%" PRIu32 " cpu_id=%" PRIu32 "\\n",
          event.state(), event.cpu_id());
  return std::string(line);
}

std::string FormatCpuFrequencyLimits(
    const CpuFrequencyLimitsFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "cpu_frequency_limits: min_freq=%" PRIu32 "max_freq=%" PRIu32
          " cpu_id=%" PRIu32 "\\n",
          event.min_freq(), event.max_freq(), event.cpu_id());
  return std::string(line);
}

std::string FormatCpuIdle(const CpuIdleFtraceEvent& event) {
  char line[2048];
  sprintf(line, "cpu_idle: state=%" PRIu32 " cpu_id=%" PRIu32 "\\n",
          event.state(), event.cpu_id());
  return std::string(line);
}

std::string FormatClockSetRate(const ClockSetRateFtraceEvent& event) {
  char line[2048];
  sprintf(line, "clock_set_rate: %s state=%llu cpu_id=%llu\\n",
          event.name().empty() ? "todo" : event.name().c_str(), event.state(),
          event.cpu_id());
  return std::string(line);
}

std::string FormatClockEnable(const ClockEnableFtraceEvent& event) {
  char line[2048];
  sprintf(line, "clock_enable: %s state=%llu cpu_id=%llu\\n",
          event.name().empty() ? "todo" : event.name().c_str(), event.state(),
          event.cpu_id());
  return std::string(line);
}

std::string FormatClockDisable(const ClockDisableFtraceEvent& event) {
  char line[2048];
  sprintf(line, "clock_disable: %s state=%llu cpu_id=%llu\\n",
          event.name().empty() ? "todo" : event.name().c_str(), event.state(),
          event.cpu_id());
  return std::string(line);
}

std::string FormatTracingMarkWrite(const TracingMarkWriteFtraceEvent& event) {
  char line[2048];
  sprintf(line, "tracing_mark_write: %s|%d|%s\\n",
          event.trace_begin() ? "B" : "E", event.pid(),
          event.trace_name().c_str());
  return std::string(line);
}

std::string FormatBinderLocked(const BinderLockedFtraceEvent& event) {
  char line[2048];
  sprintf(line, "binder_locked: tag=%s\\n", event.tag().c_str());
  return std::string(line);
}

std::string FormatBinderUnlock(const BinderUnlockFtraceEvent& event) {
  char line[2048];
  sprintf(line, "binder_unlock: tag=%s\\n", event.tag().c_str());
  return std::string(line);
}

std::string FormatBinderLock(const BinderLockFtraceEvent& event) {
  char line[2048];
  sprintf(line, "binder_lock: tag=%s\\n", event.tag().c_str());
  return std::string(line);
}

std::string FormatBinderTransaction(const BinderTransactionFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "binder_transaction: transaction=%d dest_node=%d dest_proc=%d "
          "dest_thread=%d reply=%d flags=0x%x code=0x%x\\n",
          event.debug_id(), event.target_node(), event.to_proc(),
          event.to_thread(), event.reply(), event.flags(), event.code());
  return std::string(line);
}

std::string FormatBinderTransactionReceived(
    const BinderTransactionReceivedFtraceEvent& event) {
  char line[2048];
  sprintf(line, "binder_transaction_received: transaction=%d\\n",
          event.debug_id());
  return std::string(line);
}

std::string FormatExt4SyncFileEnter(const Ext4SyncFileEnterFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_sync_file_enter: dev %d,%d ino %lu parent %lu datasync %d \\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          (unsigned long)event.parent(), event.datasync());
  return std::string(line);
}

std::string FormatExt4SyncFileExit(const Ext4SyncFileExitFtraceEvent& event) {
  char line[2048];
  sprintf(line, "ext4_sync_file_exit: dev %d,%d ino %lu ret %d\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          event.ret());
  return std::string(line);
}

std::string FormatExt4DaWriteBegin(const Ext4DaWriteBeginFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_da_write_begin: dev %d,%d ino %lu pos %lld len %u flags %u\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          event.pos(), event.len(), event.flags());
  return std::string(line);
}

std::string FormatExt4DaWriteEnd(const Ext4DaWriteEndFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_da_write_end: dev %d,%d ino %lu pos %lld len %u copied %u\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          event.pos(), event.len(), event.copied());
  return std::string(line);
}

std::string FormatBlockRqIssue(const BlockRqIssueFtraceEvent& event) {
  char line[2048];
  sprintf(line, "block_rq_issue: %d,%d %s %u (%s) %llu + %u [%s]\\n",
          major(event.dev()), minor(event.dev()), event.rwbs().c_str(),
          event.bytes(), event.cmd().c_str(),
          (unsigned long long)event.sector(), event.nr_sector(),
          event.comm().c_str());
  return std::string(line);
}

std::string FormatI2cRead(const I2cReadFtraceEvent& event) {
  char line[2048];
  sprintf(line, "i2c_read: i2c-%d #%u a=%03x f=%04x l=%u\\n",
          event.adapter_nr(), event.msg_nr(), event.addr(), event.flags(),
          event.len());
  return std::string(line);
}

std::string FormatI2cResult(const I2cResultFtraceEvent& event) {
  char line[2048];
  sprintf(line, "i2c_result: i2c-%d n=%u ret=%d\\n", event.adapter_nr(),
          event.nr_msgs(), event.ret());
  return std::string(line);
}

std::string FormatIrqHandlerEntry(const IrqHandlerEntryFtraceEvent& event) {
  char line[2048];
  sprintf(line, "irq_handler_entry: irq=%d name=%s\\n", event.irq(),
          event.name().c_str());
  return std::string(line);
}

std::string FormatIrqHandlerExit(const IrqHandlerExitFtraceEvent& event) {
  char line[2048];
  sprintf(line, "irq_handler_exit: irq=%d ret=%s\\n", event.irq(),
          event.ret() ? "handled" : "unhandled");
  return std::string(line);
}

std::string FormatMmVmscanKswapdWake(
    const MmVmscanKswapdWakeFtraceEvent& event) {
  char line[2048];
  sprintf(line, "mm_vmscan_kswapd_wake: nid=%d order=%d\\n", event.nid(),
          event.order());
  return std::string(line);
}

std::string FormatMmVmscanKswapdSleep(
    const MmVmscanKswapdSleepFtraceEvent& event) {
  char line[2048];
  sprintf(line, "mm_vmscan_kswapd_sleep: nid=%d\\n", event.nid());
  return std::string(line);
}

std::string FormatRegulatorEnable(const RegulatorEnableFtraceEvent& event) {
  char line[2048];
  sprintf(line, "regulator_enable: name=%s\\n", event.name().c_str());
  return std::string(line);
}

std::string FormatRegulatorEnableDelay(
    const RegulatorEnableDelayFtraceEvent& event) {
  char line[2048];
  sprintf(line, "regulator_enable_delay: name=%s\\n", event.name().c_str());
  return std::string(line);
}

std::string FormatRegulatorEnableComplete(
    const RegulatorEnableCompleteFtraceEvent& event) {
  char line[2048];
  sprintf(line, "regulator_enable_complete: name=%s\\n", event.name().c_str());
  return std::string(line);
}

std::string FormatRegulatorDisable(const RegulatorDisableFtraceEvent& event) {
  char line[2048];
  sprintf(line, "regulator_disable: name=%s\\n", event.name().c_str());
  return std::string(line);
}

std::string FormatRegulatorDisableComplete(
    const RegulatorDisableCompleteFtraceEvent& event) {
  char line[2048];
  sprintf(line, "regulator_disable_complete: name=%s\\n", event.name().c_str());
  return std::string(line);
}

std::string FormatRegulatorSetVoltage(
    const RegulatorSetVoltageFtraceEvent& event) {
  char line[2048];
  sprintf(line, "regulator_set_voltage: name=%s (%d-%d)\\n",
          event.name().c_str(), event.min(), event.max());
  return std::string(line);
}

std::string FormatRegulatorSetVoltageComplete(
    const RegulatorSetVoltageCompleteFtraceEvent& event) {
  char line[2048];
  sprintf(line, "regulator_set_voltage_complete: name=%s, val=%u\\n",
          event.name().c_str(), event.val());
  return std::string(line);
}

std::string FormatSchedCpuHotplug(const SchedCpuHotplugFtraceEvent& event) {
  char line[2048];
  sprintf(line, "sched_cpu_hotplug: cpu %d %s error=%d\\n",
          event.affected_cpu(), event.status() ? "online" : "offline",
          event.error());
  return std::string(line);
}

std::string FormatSyncTimeline(const SyncTimelineFtraceEvent& event) {
  char line[2048];
  sprintf(line, "sync_timeline: name=%s value=%s\\n", event.name().c_str(),
          event.value().c_str());
  return std::string(line);
}

std::string FormatSyncWait(const SyncWaitFtraceEvent& event) {
  char line[2048];
  sprintf(line, "sync_wait: %s name=%s state=%d\\n",
          event.begin() ? "begin" : "end", event.name().c_str(),
          event.status());
  return std::string(line);
}

std::string FormatSyncPt(const SyncPtFtraceEvent& event) {
  char line[2048];
  sprintf(line, "sync_pt: name=%s value=%s\\n", event.timeline().c_str(),
          event.value().c_str());
  return std::string(line);
}

int TraceToText(std::istream* input, std::ostream* output) {
  DiskSourceTree dst;
  dst.MapPath("perfetto", "protos/perfetto");
  MFE mfe;
  Importer importer(&dst, &mfe);
  const FileDescriptor* parsed_file =
      importer.Import("perfetto/trace/trace.proto");

  DynamicMessageFactory dmf;
  const Descriptor* trace_descriptor = parsed_file->message_type(0);
  const Message* msg_root = dmf.GetPrototype(trace_descriptor);
  Message* msg = msg_root->New();

  if (!msg->ParseFromIstream(input)) {
    PERFETTO_ELOG("Could not parse input.");
    return 1;
  }
  OstreamOutputStream zero_copy_output(output);
  TextFormat::Print(*msg, &zero_copy_output);
  return 0;
}

std::string FormatSoftirqRaise(const SoftirqRaiseFtraceEvent& event) {
  char line[2048];
  sprintf(line, "softirq_raise: vec=%u [action=%s]\\n", event.vec(),
          SoftirqArray[event.vec()]);
  return std::string(line);
}

std::string FormatSoftirqEntry(const SoftirqEntryFtraceEvent& event) {
  char line[2048];
  sprintf(line, "softirq_entry: vec=%u [action=%s]\\n", event.vec(),
          SoftirqArray[event.vec()]);
  return std::string(line);
}

std::string FormatSoftirqExit(const SoftirqExitFtraceEvent& event) {
  char line[2048];
  sprintf(line, "softirq_exit: vec=%u [action=%s]\\n", event.vec(),
          SoftirqArray[event.vec()]);
  return std::string(line);
}

std::string FormatI2cWrite(const I2cWriteFtraceEvent& event) {
  char line[2048];
  // TODO(hjd): Check event.buf().
  sprintf(line, "i2c_write: i2c-%d #%u a=%03x f=%04x l=%u\\n",
          event.adapter_nr(), event.msg_nr(), event.addr(), event.flags(),
          event.len());
  return std::string(line);
}

std::string FormatI2cReply(const I2cReplyFtraceEvent& event) {
  char line[2048];
  // TODO(hjd): Check event.buf().
  sprintf(line, "i2c_reply: i2c-%d #%u a=%03x f=%04x l=%u\\n",
          event.adapter_nr(), event.msg_nr(), event.addr(), event.flags(),
          event.len());
  return std::string(line);
}

// TODO(hjd): Check gfp_flags
std::string FormatMmVmscanDirectReclaimBegin(
    const MmVmscanDirectReclaimBeginFtraceEvent& event) {
  char line[2048];
  sprintf(line, "mm_vmscan_direct_reclaim_begin: order=%d may_writepage=%d\\n",
          event.order(), event.may_writepage());
  return std::string(line);
}

std::string FormatMmVmscanDirectReclaimEnd(
    const MmVmscanDirectReclaimEndFtraceEvent& event) {
  char line[2048];
  sprintf(line, "mm_vmscan_direct_reclaim_end: nr_reclaimed=%llu\\n",
          event.nr_reclaimed());
  return std::string(line);
}

std::string FormatLowmemoryKill(const LowmemoryKillFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "lowmemory_kill: %s (%d), page cache %lldkB (limit %lldkB), free "
          "%lldKb\\n",
          event.comm().c_str(), event.pid(), event.pagecache_size(),
          event.pagecache_limit(), event.free());
  return std::string(line);
}

std::string FormatWorkqueueExecuteStart(
    const WorkqueueExecuteStartFtraceEvent& event) {
  char line[2048];
  sprintf(line, "workqueue_execute_start: work struct %llx: function %llxf\\n",
          event.work(), event.function());
  return std::string(line);
}

std::string FormatWorkqueueExecuteEnd(
    const WorkqueueExecuteEndFtraceEvent& event) {
  char line[2048];
  sprintf(line, "workqueue_execute_end: work struct %llx\\n", event.work());
  return std::string(line);
}

std::string FormatWorkqueueQueueWork(
    const WorkqueueQueueWorkFtraceEvent& event) {
  char line[2048];
  sprintf(
      line,
      "workqueue_queue_work: work struct=%llx function=%llxf workqueue=%llx "
      "req_cpu=%u cpu=%u\\n",
      event.work(), event.function(), event.workqueue(), event.req_cpu(),
      event.cpu());
  return std::string(line);
}

std::string FormatWorkqueueActivateWork(
    const WorkqueueActivateWorkFtraceEvent& event) {
  char line[2048];
  sprintf(line, "workqueue_activate_work: work struct %llx\\n", event.work());
  return std::string(line);
}

std::string FormatMmCompactionBegin(const MmCompactionBeginFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "mm_compaction_begin: zone_start=0x%llx migrate_pfn=0x%llx "
          "free_pfn=0x%llx zone_end=0x%llx, mode=%s\\n",
          event.zone_start(), event.migrate_pfn(), event.free_pfn(),
          event.zone_end(), event.sync() ? "sync" : "async");
  return std::string(line);
}

std::string FormatMmCompactionDeferCompaction(
    const MmCompactionDeferCompactionFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "mm_compaction_defer_compaction: node=%d zone=%-8s order=%d "
          "order_failed=%d consider=%u limit=%lu\\n",
          event.nid(), MmCompactionSuitableArray[event.idx()], event.order(),
          event.order_failed(), event.considered(), 1UL << event.defer_shift());
  return std::string(line);
}

std::string FormatMmCompactionDeferred(
    const MmCompactionDeferredFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "mm_compaction_deferred: node=%d zone=%-8s order=%d order_failed=%d "
          "consider=%u limit=%lu\\n",
          event.nid(), MmCompactionSuitableArray[event.idx()], event.order(),
          event.order_failed(), event.considered(), 1UL << event.defer_shift());
  return std::string(line);
}

std::string FormatMmCompactionDeferReset(
    const MmCompactionDeferResetFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "mm_compaction_defer_reset: node=%d zone=%-8s order=%d "
          "order_failed=%d consider=%u limit=%lu\\n",
          event.nid(), MmCompactionSuitableArray[event.idx()], event.order(),
          event.order_failed(), event.considered(), 1UL << event.defer_shift());
  return std::string(line);
}

std::string FormatMmCompactionEnd(const MmCompactionEndFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "mm_compaction_end: zone_start=0x%llx migrate_pfn=0x%llx "
          "free_pfn=0x%llx zone_end=0x%llx, mode=%s status=%s\\n",
          event.zone_start(), event.migrate_pfn(), event.free_pfn(),
          event.zone_end(), event.sync() ? "sync" : "aysnc",
          MmCompactionRetArray[event.status()]);
  return std::string(line);
}

std::string FormatMmCompactionFinished(
    const MmCompactionFinishedFtraceEvent& event) {
  char line[2048];
  sprintf(line, "mm_compaction_finished: node=%d zone=%-8s order=%d ret=%s\\n",
          event.nid(), MmCompactionSuitableArray[event.idx()], event.order(),
          MmCompactionRetArray[event.ret()]);
  return std::string(line);
}

std::string FormatMmCompactionIsolateFreepages(
    const MmCompactionIsolateFreepagesFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "mm_compaction_isolate_freepages: range=(0x%llx ~ 0x%llx) "
          "nr_scanned=%llu nr_taken=%llu\\n",
          event.start_pfn(), event.end_pfn(), event.nr_scanned(),
          event.nr_taken());
  return std::string(line);
}

std::string FormatMmCompactionIsolateMigratepages(
    const MmCompactionIsolateMigratepagesFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "mm_compaction_isolate_migratepages: range=(0x%llx ~ 0x%llx) "
          "nr_scanned=%llu nr_taken=%llu\\n",
          event.start_pfn(), event.end_pfn(), event.nr_scanned(),
          event.nr_taken());
  return std::string(line);
}

std::string FormatMmCompactionKcompactdSleep(
    const MmCompactionKcompactdSleepFtraceEvent& event) {
  char line[2048];
  sprintf(line, "mm_compaction_kcompactd_sleep: nid=%d\\n", event.nid());
  return std::string(line);
}

std::string FormatMmCompactionKcompactdWake(
    const MmCompactionKcompactdWakeFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "mm_compaction_kcompactd_wake: nid=%d order=%d classzone_idx=%-8s\\n",
          event.nid(), event.order(),
          MmCompactionSuitableArray[event.classzone_idx()]);
  return std::string(line);
}

std::string FormatMmCompactionMigratepages(
    const MmCompactionMigratepagesFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "mm_compaction_migratepages: nr_migrated=%llu nr_failed=%llu\\n",
          event.nr_migrated(), event.nr_failed());
  return std::string(line);
}

std::string FormatMmCompactionSuitable(
    const MmCompactionSuitableFtraceEvent& event) {
  char line[2048];
  sprintf(line, "mm_compaction_suitable: node=%d zone=%-8s order=%d ret=%s\\n",
          event.nid(), MmCompactionSuitableArray[event.idx()], event.order(),
          MmCompactionRetArray[event.ret()]);
  return std::string(line);
}

std::string FormatMmCompactionTryToCompactPages(
    const MmCompactionTryToCompactPagesFtraceEvent& event) {
  char line[2048];
  sprintf(
      line,
      "mm_compaction_try_to_compact_pages: order=%d gfp_mask=0x%x mode=%d\\n",
      event.order(), event.gfp_mask(),
      event.mode());  // convert to int?
  return std::string(line);
}

std::string FormatMmCompactionWakeupKcompactd(
    const MmCompactionWakeupKcompactdFtraceEvent& event) {
  char line[2048];
  sprintf(
      line,
      "mm_compaction_wakeup_kcompactd: nid=%d order=%d classzone_idx=%-8s\\n",
      event.nid(), event.order(),
      MmCompactionSuitableArray[event.classzone_idx()]);
  return std::string(line);
}

std::string FormatSuspendResume(const SuspendResumeFtraceEvent& event) {
  char line[2048];
  sprintf(line, "suspend_resume: %s[%u] %s\\n", event.action().c_str(),
          event.val(), event.start() ? "begin" : "end");
  return std::string(line);
}

std::string FormatSchedWakeupNew(const SchedWakeupNewFtraceEvent& event) {
  char line[2048];
  sprintf(line, "sched_wakeup_new: comm=%s pid=%d prio=%d target_cpu=%03d\\n",
          event.comm().c_str(), event.pid(), event.prio(), event.target_cpu());
  return std::string(line);
}

std::string FormatSchedProcessExec(const SchedProcessExecFtraceEvent& event) {
  char line[2048];
  sprintf(line, "sched_process_exec: filename=%s pid=%d old_pid=%d\\n",
          event.filename().c_str(), event.pid(), event.old_pid());
  return std::string(line);
}
std::string FormatSchedProcessExit(const SchedProcessExitFtraceEvent& event) {
  char line[2048];
  sprintf(line, "sched_process_exit: comm=%s pid=%d tgid=%d prio=%d\\n",
          event.comm().c_str(), event.pid(), event.tgid(), event.prio());
  return std::string(line);
}
std::string FormatSchedProcessFork(const SchedProcessForkFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "sched_process_fork: parent_comm=%s parent_pid=%d child_comm=%s "
          "child_pid=%d\\n",
          event.parent_comm().c_str(), event.parent_pid(),
          event.child_comm().c_str(), event.child_pid());
  return std::string(line);
}
std::string FormatSchedProcessFree(const SchedProcessFreeFtraceEvent& event) {
  char line[2048];
  sprintf(line, "sched_process_free: comm=%s pid=%d prio=%d\\n",
          event.comm().c_str(), event.pid(), event.prio());
  return std::string(line);
}
std::string FormatSchedProcessHang(const SchedProcessHangFtraceEvent& event) {
  char line[2048];
  sprintf(line, "sched_process_hang: comm=%s pid=%d\\n", event.comm().c_str(),
          event.pid());
  return std::string(line);
}

std::string FormatSchedProcessWait(const SchedProcessWaitFtraceEvent& event) {
  char line[2048];
  sprintf(line, "sched_process_wait: comm=%s pid=%d\\n", event.comm().c_str(),
          event.pid());
  return std::string(line);
}

std::string FormatTaskNewtask(const TaskNewtaskFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "task_newtask: comm=%s pid=%d clone_flags=%llu oom_score_adj=%d\\n",
          event.comm().c_str(), event.pid(), event.clone_flags(),
          event.oom_score_adj());
  return std::string(line);
}

std::string FormatTaskRename(const TaskRenameFtraceEvent& event) {
  char line[2048];
  sprintf(line, "task_rename: pid=%d oldcomm=%s newcomm=%s oom_score_adj=%d\\n",
          event.pid(), event.newcomm().c_str(), event.oldcomm().c_str(),
          event.oom_score_adj());
  return std::string(line);
}

std::string FormatBlockBioBackmerge(const BlockBioBackmergeFtraceEvent& event) {
  char line[2048];
  sprintf(line, "block_bio_backmerge: %d,%d %s %llu + %u [%s]\\n",
          major(event.dev()), minor(event.dev()), event.rwbs().c_str(),
          (unsigned long long)event.sector(), event.nr_sector(),
          event.comm().c_str());
  return std::string(line);
}

std::string FormatBlockBioBounce(const BlockBioBounceFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "block_bio_bounce:"
          "%d,%d %s %llu + %u [%s]\\n",
          major(event.dev()), minor(event.dev()), event.rwbs().c_str(),
          (unsigned long long)event.sector(), event.nr_sector(),
          event.comm().c_str());
  return std::string(line);
}

std::string FormatBlockBioComplete(const BlockBioCompleteFtraceEvent& event) {
  char line[2048];
  sprintf(line, "block_bio_complete: %d,%d %s %llu + %u [%d]\\n",
          major(event.dev()), minor(event.dev()), event.rwbs().c_str(),
          (unsigned long long)event.sector(), event.nr_sector(), event.error());
  return std::string(line);
}

std::string FormatBlockBioFrontmerge(
    const BlockBioFrontmergeFtraceEvent& event) {
  char line[2048];
  sprintf(line, "block_bio_frontmerge: %d,%d %s %llu + %u [%s]\\n",
          major(event.dev()), minor(event.dev()), event.rwbs().c_str(),
          (unsigned long long)event.sector(), event.nr_sector(),
          event.comm().c_str());
  return std::string(line);
}

std::string FormatBlockBioQueue(const BlockBioQueueFtraceEvent& event) {
  char line[2048];
  sprintf(line, "block_bio_queue: %d,%d %s %llu + %u [%s]\\n",
          major(event.dev()), minor(event.dev()), event.rwbs().c_str(),
          (unsigned long long)event.sector(), event.nr_sector(),
          event.comm().c_str());
  return std::string(line);
}

std::string FormatBlockBioRemap(const BlockBioRemapFtraceEvent& event) {
  char line[2048];
  sprintf(line, "block_bio_remap:  %d,%d %s %llu + %u <- (%d,%d) %llu\\n",
          major(event.dev()), minor(event.dev()), event.rwbs().c_str(),
          (unsigned long long)event.sector(), event.nr_sector(),
          major(event.dev()), minor(event.dev()),
          (unsigned long long)event.old_sector());
  return std::string(line);
}

std::string FormatBlockDirtyBuffer(const BlockDirtyBufferFtraceEvent& event) {
  char line[2048];
  sprintf(line, "block_dirty_buffer: %d,%d sector=%llu size=%zu\\n",
          major(event.dev()), minor(event.dev()),
          (unsigned long long)event.sector(), (unsigned long)event.size());
  return std::string(line);
}

std::string FormatBlockGetrq(const BlockGetrqFtraceEvent& event) {
  char line[2048];
  sprintf(line, "block_getrq: %d,%d %s %llu + %u [%s]\\n", major(event.dev()),
          minor(event.dev()), event.rwbs().c_str(),
          (unsigned long long)event.sector(), event.nr_sector(),
          event.comm().c_str());
  return std::string(line);
}

std::string FormatBlockPlug(const BlockPlugFtraceEvent& event) {
  char line[2048];
  sprintf(line, "block_plug: comm=[%s]\\n", event.comm().c_str());
  return std::string(line);
}

std::string FormatBlockRqAbort(const BlockRqAbortFtraceEvent& event) {
  char line[2048];
  sprintf(line, "block_rq_abort: %d,%d %s (%s) %llu + %u [%d]\\n",
          major(event.dev()), minor(event.dev()), event.rwbs().c_str(),
          event.cmd().c_str(), (unsigned long long)event.sector(),
          event.nr_sector(), event.errors());
  return std::string(line);
}

std::string FormatBlockRqComplete(const BlockRqCompleteFtraceEvent& event) {
  char line[2048];
  sprintf(line, "block_rq_complete: %d,%d %s (%s) %llu + %u [%d]\\n",
          major(event.dev()), minor(event.dev()), event.rwbs().c_str(),
          event.cmd().c_str(), (unsigned long long)event.sector(),
          event.nr_sector(), event.errors());
  return std::string(line);
}

std::string FormatBlockRqInsert(const BlockRqInsertFtraceEvent& event) {
  char line[2048];
  sprintf(line, "block_rq_insert: %d,%d %s %u (%s) %llu + %u [%s]\\n",
          major(event.dev()), minor(event.dev()), event.rwbs().c_str(),
          event.bytes(), event.cmd().c_str(),
          (unsigned long long)event.sector(), event.nr_sector(),
          event.comm().c_str());
  return std::string(line);
}

std::string FormatBlockRqRemap(const BlockRqRemapFtraceEvent& event) {
  char line[2048];
  sprintf(line, "block_rq_remap: %d,%d %s %llu + %u <- (%d,%d) %llu %u\\n",
          major(event.dev()), minor(event.dev()), event.rwbs().c_str(),
          (unsigned long long)event.sector(), event.nr_sector(),
          major(event.dev()), minor(event.dev()),
          (unsigned long long)event.old_sector(), event.nr_bios());
  return std::string(line);
}

std::string FormatBlockRqRequeue(const BlockRqRequeueFtraceEvent& event) {
  char line[2048];
  sprintf(line, "block_rq_requeue: %d,%d %s (%s) %llu + %u [%d\\n",
          major(event.dev()), minor(event.dev()), event.rwbs().c_str(),
          event.cmd().c_str(), (unsigned long long)event.sector(),
          event.nr_sector(), event.errors());
  return std::string(line);
}

std::string FormatBlockSleeprq(const BlockSleeprqFtraceEvent& event) {
  char line[2048];
  sprintf(line, "block_sleeprq: %d,%d %s %llu + %u [%s]\\n", major(event.dev()),
          minor(event.dev()), event.rwbs().c_str(),
          (unsigned long long)event.sector(), event.nr_sector(),
          event.comm().c_str());
  return std::string(line);
}

std::string FormatBlockSplit(const BlockSplitFtraceEvent& event) {
  char line[2048];
  sprintf(line, "block_split: %d,%d %s %llu / %llu [%s]\\n", major(event.dev()),
          minor(event.dev()), event.rwbs().c_str(),
          (unsigned long long)event.sector(),
          (unsigned long long)event.new_sector(), event.comm().c_str());
  return std::string(line);
}

std::string FormatBlockTouchBuffer(const BlockTouchBufferFtraceEvent& event) {
  char line[2048];
  sprintf(line, "block_touch_buffer: %d,%d sector=%llu size=%zu\\n",
          major(event.dev()), minor(event.dev()),
          (unsigned long long)event.sector(), (unsigned long)event.size());
  return std::string(line);
}

std::string FormatBlockUnplug(const BlockUnplugFtraceEvent& event) {
  char line[2048];
  sprintf(line, "block_unplug: [%s] %d\\n", event.comm().c_str(),
          event.nr_rq());
  return std::string(line);
}

std::string FormatExt4AllocDaBlocks(const Ext4AllocDaBlocksFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_alloc_da_blocks: dev %d,%d ino %lu data_blocks %u meta_blocks "
          "%u \\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          event.data_blocks(), event.meta_blocks());
  return std::string(line);
}

std::string FormatExt4AllocateBlocks(
    const Ext4AllocateBlocksFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_allocate_blocks: dev %d,%d ino %lu flags %s len %u block %llu "
          "lblk %u goal %llu lleft %u lright %u pleft %llu pright %llu\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          GetExt4HintFlag(event.flags()), event.len(), event.block(),
          event.logical(), event.goal(), event.lleft(), event.lright(),
          event.pleft(), event.pright());
  return std::string(line);
}

std::string FormatExt4AllocateInode(const Ext4AllocateInodeFtraceEvent& event) {
  char line[2048];
  sprintf(line, "ext4_allocate_inode: dev %d,%d ino %lu dir %lu mode 0%o\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          (unsigned long)event.dir(), event.mode());
  return std::string(line);
}

std::string FormatExt4BeginOrderedTruncate(
    const Ext4BeginOrderedTruncateFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_begin_ordered_truncate: dev %d,%d ino %lu new_size %lld\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          event.new_size());
  return std::string(line);
}

std::string FormatExt4CollapseRange(const Ext4CollapseRangeFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_collapse_range: dev %d,%d ino %lu offset %lld len %lld\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          event.offset(), event.len());
  return std::string(line);
}

std::string FormatExt4DaReleaseSpace(
    const Ext4DaReleaseSpaceFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_da_release_space: dev %d,%d ino %lu mode 0%o i_blocks %llu "
          "freed_blocks %d reserved_data_blocks %d reserved_meta_blocks %d "
          "allocated_meta_blocks %d\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          event.mode(), event.i_blocks(), event.freed_blocks(),
          event.reserved_data_blocks(), event.reserved_meta_blocks(),
          event.allocated_meta_blocks());
  return std::string(line);
}

std::string FormatExt4DaReserveSpace(
    const Ext4DaReserveSpaceFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_da_reserve_space:dev %d,%d ino %lu mode 0%o i_blocks %llu "
          "reserved_data_blocks %d reserved_meta_blocks %d \\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          event.mode(), event.i_blocks(), event.reserved_data_blocks(),
          event.reserved_meta_blocks());
  return std::string(line);
}

std::string FormatExt4DaUpdateReserveSpace(
    const Ext4DaUpdateReserveSpaceFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_da_update_reserve_space: dev %d,%d ino %lu mode 0%o i_blocks "
          "%llu used_blocks %d reserved_data_blocks %d reserved_meta_blocks %d "
          "allocated_meta_blocks %d quota_claim %d\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          event.mode(), event.i_blocks(), event.used_blocks(),
          event.reserved_data_blocks(), event.reserved_meta_blocks(),
          event.allocated_meta_blocks(), event.quota_claim());
  return std::string(line);
}

std::string FormatExt4DaWritePages(const Ext4DaWritePagesFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_da_write_pages: dev %d,%d ino %lu first_page %lu nr_to_write "
          "%ld sync_mode %d\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          (unsigned long)event.first_page(), (long)event.nr_to_write(),
          event.sync_mode());
  return std::string(line);
}

// TODO(hjd): Check flags
std::string FormatExt4DaWritePagesExtent(
    const Ext4DaWritePagesExtentFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_da_write_pages_extent: dev %d,%d ino %lu lblk %llu len %u \\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          event.lblk(), event.len());
  return std::string(line);
}

std::string FormatExt4DiscardBlocks(const Ext4DiscardBlocksFtraceEvent& event) {
  char line[2048];
  sprintf(line, "ext4_discard_blocks: dev %d,%d blk %llu count %llu\\n",
          major(event.dev()), minor(event.dev()), event.blk(), event.count());
  return std::string(line);
}

std::string FormatExt4DiscardPreallocations(
    const Ext4DiscardPreallocationsFtraceEvent& event) {
  char line[2048];
  sprintf(line, "ext4_discard_preallocations: dev %d,%d ino %lu\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino());
  return std::string(line);
}

std::string FormatExt4DropInode(const Ext4DropInodeFtraceEvent& event) {
  char line[2048];
  sprintf(line, "ext4_drop_inode: dev %d,%d ino %lu drop %d\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          event.drop());
  return std::string(line);
}

// TODO(hjd): Check Es status flags
std::string FormatExt4EsCacheExtent(const Ext4EsCacheExtentFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_es_cache_extent: dev %d,%d ino %lu es [%u/%u) mapped %llu \\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          event.lblk(), event.len(), event.pblk());
  return std::string(line);
}

std::string FormatExt4EsFindDelayedExtentRangeEnter(
    const Ext4EsFindDelayedExtentRangeEnterFtraceEvent& event) {
  char line[2048];
  sprintf(
      line,
      "ext4_es_find_delayed_extent_range_enter: dev %d,%d ino %lu lblk %u\\n",
      major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
      event.lblk());
  return std::string(line);
}

// TODO(hjd): Check Es status flags
std::string FormatExt4EsFindDelayedExtentRangeExit(
    const Ext4EsFindDelayedExtentRangeExitFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_es_find_delayed_extent_range_exit: dev %d,%d ino %lu es "
          "[%u/%u) mapped %llu\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          event.lblk(), event.len(), event.pblk());
  return std::string(line);
}

// TODO(hjd): Check Es status flags
std::string FormatExt4EsInsertExtent(
    const Ext4EsInsertExtentFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_es_insert_extent: dev %d,%d ino %lu es [%u/%u) mapped %llu \\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          event.lblk(), event.len(), event.pblk());
  return std::string(line);
}

std::string FormatExt4EsLookupExtentEnter(
    const Ext4EsLookupExtentEnterFtraceEvent& event) {
  char line[2048];
  sprintf(line, "ext4_es_lookup_extent_enter: dev %d,%d ino %lu lblk %u\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          event.lblk());
  return std::string(line);
}

// TODO(hjd): Check Es status flags
std::string FormatExt4EsLookupExtentExit(
    const Ext4EsLookupExtentExitFtraceEvent& event) {
  char line[2048];
  sprintf(
      line,
      "ext4_es_lookup_extent_exit: dev %d,%d ino %lu found %d [%u/%u) %llu\\n",
      major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
      event.found(), event.lblk(), event.len(),
      event.found() ? event.pblk() : 0);
  return std::string(line);
}

std::string FormatExt4EsRemoveExtent(
    const Ext4EsRemoveExtentFtraceEvent& event) {
  char line[2048];
  sprintf(line, "ext4_es_remove_extent: dev %d,%d ino %lu es [%lld/%lld)\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          event.lblk(), event.len());
  return std::string(line);
}

std::string FormatExt4EsShrink(const Ext4EsShrinkFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_es_shrink: dev %d,%d nr_shrunk %d, scan_time %llu nr_skipped "
          "%d retried %d\\n",
          major(event.dev()), minor(event.dev()), event.nr_shrunk(),
          event.scan_time(), event.nr_skipped(), event.retried());
  return std::string(line);
}

std::string FormatExt4EsShrinkCount(const Ext4EsShrinkCountFtraceEvent& event) {
  char line[2048];
  sprintf(line, "ext4_es_shrink_count: dev %d,%d nr_to_scan %d cache_cnt %d\\n",
          major(event.dev()), minor(event.dev()), event.nr_to_scan(),
          event.cache_cnt());
  return std::string(line);
}

std::string FormatExt4EsShrinkScanEnter(
    const Ext4EsShrinkScanEnterFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_es_shrink_scan_enter: dev %d,%d nr_to_scan %d cache_cnt %d\\n",
          major(event.dev()), minor(event.dev()), event.nr_to_scan(),
          event.cache_cnt());
  return std::string(line);
}

std::string FormatExt4EsShrinkScanExit(
    const Ext4EsShrinkScanExitFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_es_shrink_scan_exit: dev %d,%d nr_shrunk %d cache_cnt %d\\n",
          major(event.dev()), minor(event.dev()), event.nr_shrunk(),
          event.cache_cnt());
  return std::string(line);
}

std::string FormatExt4EvictInode(const Ext4EvictInodeFtraceEvent& event) {
  char line[2048];
  sprintf(line, "ext4_evict_inode: dev %d,%d ino %lu nlink %d\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          event.nlink());
  return std::string(line);
}

std::string FormatExt4ExtConvertToInitializedEnter(
    const Ext4ExtConvertToInitializedEnterFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_ext_convert_to_initialized_enter: dev %d,%d ino %lu m_lblk %u "
          "m_len %u u_lblk %u u_len %u u_pblk %llu\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          event.m_lblk(), event.m_len(), event.u_lblk(), event.u_len(),
          event.u_pblk());
  return std::string(line);
}

std::string FormatExt4ExtConvertToInitializedFastpath(
    const Ext4ExtConvertToInitializedFastpathFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_ext_convert_to_initialized_fastpath: dev %d,%d ino %lu m_lblk "
          "%u m_len %u u_lblk %u u_len %u u_pblk %llu i_lblk %u i_len %u "
          "i_pblk %llu\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          event.m_lblk(), event.m_len(), event.u_lblk(), event.u_len(),
          event.u_pblk(), event.i_lblk(), event.i_len(), event.i_pblk());
  return std::string(line);
}

std::string FormatExt4ExtHandleUnwrittenExtents(
    const Ext4ExtHandleUnwrittenExtentsFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_ext_handle_unwritten_extents: dev %d,%d ino %lu m_lblk %u "
          "m_pblk %llu m_len %u flags %s allocated %d newblock %llu\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          (unsigned)event.lblk(), (unsigned long long)event.pblk(), event.len(),
          GetExt4ExtFlag(event.flags()), (unsigned int)event.allocated(),
          (unsigned long long)event.newblk());
  return std::string(line);
}

std::string FormatExt4ExtInCache(const Ext4ExtInCacheFtraceEvent& event) {
  char line[2048];
  sprintf(line, "ext4_ext_in_cache: dev %d,%d ino %lu lblk %u ret %d\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          (unsigned)event.lblk(), event.ret());
  return std::string(line);
}

std::string FormatExt4ExtLoadExtent(const Ext4ExtLoadExtentFtraceEvent& event) {
  char line[2048];
  sprintf(line, "ext4_ext_load_extent: dev %d,%d ino %lu lblk %u pblk %llu\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          event.lblk(), event.pblk());
  return std::string(line);
}

std::string FormatExt4ExtMapBlocksEnter(
    const Ext4ExtMapBlocksEnterFtraceEvent& event) {
  char line[2048];
  sprintf(
      line,
      "ext4_ext_map_blocks_enter: dev %d,%d ino %lu lblk %u len %u flags %s\\n",
      major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
      event.lblk(), event.len(), GetExt4ExtFlag(event.flags()));
  return std::string(line);
}

std::string FormatExt4ExtMapBlocksExit(
    const Ext4ExtMapBlocksExitFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_ext_map_blocks_exit: dev %d,%d ino %lu lblk %u pblk %llu len "
          "%u flags %x ret %d\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          event.lblk(), event.pblk(), event.len(), event.flags(), event.ret());
  return std::string(line);
}

std::string FormatExt4ExtPutInCache(const Ext4ExtPutInCacheFtraceEvent& event) {
  char line[2048];
  sprintf(
      line,
      "ext4_ext_put_in_cache: dev %d,%d ino %lu lblk %u len %u start %llu\\n",
      major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
      (unsigned)event.lblk(), event.len(), (unsigned long long)event.start());
  return std::string(line);
}

std::string FormatExt4ExtRemoveSpace(
    const Ext4ExtRemoveSpaceFtraceEvent& event) {
  char line[2048];
  sprintf(
      line,
      "ext4_ext_remove_space: dev %d,%d ino %lu since %u end %u depth %d\\n",
      major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
      (unsigned)event.start(), (unsigned)event.end(), event.depth());
  return std::string(line);
}

std::string FormatExt4ExtRemoveSpaceDone(
    const Ext4ExtRemoveSpaceDoneFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_ext_remove_space_done: dev %d,%d ino %lu since %u end %u depth "
          "%d partial %lld remaining_entries %u\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          (unsigned)event.start(), (unsigned)event.end(), event.depth(),
          (long long)event.partial(), (unsigned short)event.eh_entries());
  return std::string(line);
}

std::string FormatExt4ExtRmIdx(const Ext4ExtRmIdxFtraceEvent& event) {
  char line[2048];
  sprintf(line, "ext4_ext_rm_idx: dev %d,%d ino %lu index_pblk %llu\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          (unsigned long long)event.pblk());
  return std::string(line);
}

std::string FormatExt4ExtRmLeaf(const Ext4ExtRmLeafFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_ext_rm_leaf: dev %d,%d ino %lu start_lblk %u last_extent "
          "[%u(%llu), %u]partial_cluster %lld\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          (unsigned)event.start(), (unsigned)event.ee_lblk(),
          (unsigned long long)event.ee_pblk(), (unsigned short)event.ee_len(),
          (long long)event.partial());
  return std::string(line);
}

std::string FormatExt4ExtShowExtent(const Ext4ExtShowExtentFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_ext_show_extent: dev %d,%d ino %lu lblk %u pblk %llu len %u\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          (unsigned)event.lblk(), (unsigned long long)event.pblk(),
          (unsigned short)event.len());
  return std::string(line);
}

std::string FormatExt4FallocateEnter(
    const Ext4FallocateEnterFtraceEvent& event) {
  char line[2048];
  sprintf(
      line,
      "ext4_fallocate_enter: dev %d,%d ino %lu offset %lld len %lld mode %s\\n",
      major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
      event.offset(), event.len(), GetExt4ModeFlag(event.mode()));
  return std::string(line);
}

std::string FormatExt4FallocateExit(const Ext4FallocateExitFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_fallocate_exit: dev %d,%d ino %lu pos %lld blocks %u ret %d\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          event.pos(), event.blocks(), event.ret());
  return std::string(line);
}

std::string FormatExt4FindDelallocRange(
    const Ext4FindDelallocRangeFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_find_delalloc_range: dev %d,%d ino %lu from %u to %u reverse "
          "%d found %d (blk = %u)\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          (unsigned)event.from(), (unsigned)event.to(), event.reverse(),
          event.found(), (unsigned)event.found_blk());
  return std::string(line);
}

std::string FormatExt4Forget(const Ext4ForgetFtraceEvent& event) {
  char line[2048];
  sprintf(
      line,
      "ext4_forget: dev %d,%d ino %lu mode 0%o is_metadata %d block %llu\\n",
      major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
      event.mode(), event.is_metadata(), event.block());
  return std::string(line);
}

std::string FormatExt4FreeBlocks(const Ext4FreeBlocksFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_free_blocks: dev %d,%d ino %lu mode 0%o block %llu count %lu "
          "flags %s\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          event.mode(), event.block(), (unsigned long)event.count(),
          GetExt4FreeBlocksFlag(event.flags()));
  return std::string(line);
}

std::string FormatExt4FreeInode(const Ext4FreeInodeFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_free_inode: dev %d,%d ino %lu mode 0%o uid %u gid %u blocks "
          "%llu\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          event.mode(), event.uid(), event.gid(), event.blocks());
  return std::string(line);
}

std::string FormatExt4GetImpliedClusterAllocExit(
    const Ext4GetImpliedClusterAllocExitFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_get_implied_cluster_alloc_exit: dev %d,%d m_lblk %u m_pblk "
          "%llu m_len %u m_flags %u ret %d\\n",
          major(event.dev()), minor(event.dev()), event.lblk(),
          (unsigned long long)event.pblk(), event.len(), event.flags(),
          event.ret());
  return std::string(line);
}

std::string FormatExt4GetReservedClusterAlloc(
    const Ext4GetReservedClusterAllocFtraceEvent& event) {
  char line[2048];
  sprintf(
      line,
      "ext4_get_reserved_cluster_alloc: dev %d,%d ino %lu lblk %u len %u\\n",
      major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
      (unsigned)event.lblk(), event.len());
  return std::string(line);
}

std::string FormatExt4IndMapBlocksEnter(
    const Ext4IndMapBlocksEnterFtraceEvent& event) {
  char line[2048];
  sprintf(
      line,
      "ext4_ind_map_blocks_enter: dev %d,%d ino %lu lblk %u len %u flags %u\\n",
      major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
      event.lblk(), event.len(), event.flags());
  return std::string(line);
}

std::string FormatExt4IndMapBlocksExit(
    const Ext4IndMapBlocksExitFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_ind_map_blocks_exit: dev %d,%d ino %lu lblk %u pblk %llu len "
          "%u flags %x ret %d\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          event.lblk(), event.pblk(), event.len(), event.flags(), event.ret());
  return std::string(line);
}

std::string FormatExt4InsertRange(const Ext4InsertRangeFtraceEvent& event) {
  char line[2048];
  sprintf(line, "ext4_insert_range: dev %d,%d ino %lu offset %lld len %lld\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          event.offset(), event.len());
  return std::string(line);
}

std::string FormatExt4Invalidatepage(
    const Ext4InvalidatepageFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_invalidatepage: dev %d,%d ino %lu page_index %lu offset %u "
          "length %u\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          (unsigned long)event.index(), event.offset(), event.length());
  return std::string(line);
}

std::string FormatExt4JournalStart(const Ext4JournalStartFtraceEvent& event) {
  char line[2048];
  sprintf(
      line,
      "ext4_journal_start: dev %d,%d blocks, %d rsv_blocks, %d caller %pS\\n",
      major(event.dev()), minor(event.dev()), event.blocks(),
      event.rsv_blocks(), (void*)event.ip());
  return std::string(line);
}

std::string FormatExt4JournalStartReserved(
    const Ext4JournalStartReservedFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_journal_start_reserved: dev %d,%d blocks, %d caller %pS\\n",
          major(event.dev()), minor(event.dev()), event.blocks(),
          (void*)event.ip());
  return std::string(line);
}

std::string FormatExt4JournalledInvalidatepage(
    const Ext4JournalledInvalidatepageFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_journalled_invalidatepage: dev %d,%d ino %lu page_index %lu "
          "offset %u length %u\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          (unsigned long)event.index(), event.offset(), event.length());
  return std::string(line);
}

std::string FormatExt4JournalledWriteEnd(
    const Ext4JournalledWriteEndFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_journalled_write_end: dev %d,%d ino %lu pos %lld len %u copied "
          "%u\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          event.pos(), event.len(), event.copied());
  return std::string(line);
}

std::string FormatExt4LoadInode(const Ext4LoadInodeFtraceEvent& event) {
  char line[2048];
  sprintf(line, "ext4_load_inode: dev %d,%d ino %ld\\n", major(event.dev()),
          minor(event.dev()), (unsigned long)event.ino());
  return std::string(line);
}

std::string FormatExt4LoadInodeBitmap(
    const Ext4LoadInodeBitmapFtraceEvent& event) {
  char line[2048];
  sprintf(line, "ext4_load_inode_bitmap: dev %d,%d group %u\\n",
          major(event.dev()), minor(event.dev()), event.group());
  return std::string(line);
}

std::string FormatExt4MarkInodeDirty(
    const Ext4MarkInodeDirtyFtraceEvent& event) {
  char line[2048];
  sprintf(line, "ext4_mark_inode_dirty: dev %d,%d ino %lu caller %pS\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          (void*)event.ip());
  return std::string(line);
}

std::string FormatExt4MbBitmapLoad(const Ext4MbBitmapLoadFtraceEvent& event) {
  char line[2048];
  sprintf(line, "ext4_mb_bitmap_load: dev %d,%d group %u\\n",
          major(event.dev()), minor(event.dev()), event.group());
  return std::string(line);
}

std::string FormatExt4MbBuddyBitmapLoad(
    const Ext4MbBuddyBitmapLoadFtraceEvent& event) {
  char line[2048];
  sprintf(line, "ext4_mb_buddy_bitmap_load: dev %d,%d group %u\\n",
          major(event.dev()), minor(event.dev()), event.group());
  return std::string(line);
}

std::string FormatExt4MbDiscardPreallocations(
    const Ext4MbDiscardPreallocationsFtraceEvent& event) {
  char line[2048];
  sprintf(line, "ext4_mb_discard_preallocations: dev %d,%d needed %d\\n",
          major(event.dev()), minor(event.dev()), event.needed());
  return std::string(line);
}

std::string FormatExt4MbNewGroupPa(const Ext4MbNewGroupPaFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_mb_new_group_pa: dev %d,%d ino %lu pstart %llu len %u lstart "
          "%llu\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          event.pa_pstart(), event.pa_len(), event.pa_lstart());
  return std::string(line);
}

std::string FormatExt4MbNewInodePa(const Ext4MbNewInodePaFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_mb_new_inode_pa: dev %d,%d ino %lu pstart %llu len %u lstart "
          "%llu\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          event.pa_pstart(), event.pa_len(), event.pa_lstart());
  return std::string(line);
}

std::string FormatExt4MbReleaseGroupPa(
    const Ext4MbReleaseGroupPaFtraceEvent& event) {
  char line[2048];
  sprintf(line, "ext4_mb_release_group_pa: dev %d,%d pstart %llu len %u\\n",
          major(event.dev()), minor(event.dev()), event.pa_pstart(),
          event.pa_len());
  return std::string(line);
}

std::string FormatExt4MbReleaseInodePa(
    const Ext4MbReleaseInodePaFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_mb_release_inode_pa: dev %d,%d ino %lu block %llu count %u\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          event.block(), event.count());
  return std::string(line);
}

std::string FormatExt4MballocAlloc(const Ext4MballocAllocFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_mballoc_alloc: dev %d,%d inode %lu orig %u/%d/%u@%u goal "
          "%u/%d/%u@%u result %u/%d/%u@%u blks %u grps %u cr %u flags %s tail "
          "%u broken %u\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          event.orig_group(), event.orig_start(), event.orig_len(),
          event.orig_logical(), event.goal_group(), event.goal_start(),
          event.goal_len(), event.goal_logical(), event.result_group(),
          event.result_start(), event.result_len(), event.result_logical(),
          event.found(), event.groups(), event.cr(),
          GetExt4HintFlag(event.flags()), event.tail(),
          event.buddy() ? 1 << event.buddy() : 0);
  return std::string(line);
}

std::string FormatExt4MballocDiscard(
    const Ext4MballocDiscardFtraceEvent& event) {
  char line[2048];
  sprintf(line, "ext4_mballoc_discard: dev %d,%d inode %lu extent %u/%d/%d \\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          event.result_group(), event.result_start(), event.result_len());
  return std::string(line);
}

std::string FormatExt4MballocFree(const Ext4MballocFreeFtraceEvent& event) {
  char line[2048];
  sprintf(line, "ext4_mballoc_free: dev %d,%d inode %lu extent %u/%d/%d \\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          event.result_group(), event.result_start(), event.result_len());
  return std::string(line);
}

std::string FormatExt4MballocPrealloc(
    const Ext4MballocPreallocFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_mballoc_prealloc: dev %d,%d inode %lu orig %u/%d/%u@%u result "
          "%u/%d/%u@%u\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          event.orig_group(), event.orig_start(), event.orig_len(),
          event.orig_logical(), event.result_group(), event.result_start(),
          event.result_len(), event.result_logical());
  return std::string(line);
}

std::string FormatExt4OtherInodeUpdateTime(
    const Ext4OtherInodeUpdateTimeFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_other_inode_update_time: dev %d,%d orig_ino %lu ino %lu mode "
          "0%o uid %u gid %u\\n",
          major(event.dev()), minor(event.dev()),
          (unsigned long)event.orig_ino(), (unsigned long)event.ino(),
          event.mode(), event.uid(), event.gid());
  return std::string(line);
}

std::string FormatExt4PunchHole(const Ext4PunchHoleFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_punch_hole: dev %d,%d ino %lu offset %lld len %lld mode %s\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          event.offset(), event.len(), GetExt4ModeFlag(event.mode()));
  return std::string(line);
}

std::string FormatExt4ReadBlockBitmapLoad(
    const Ext4ReadBlockBitmapLoadFtraceEvent& event) {
  char line[2048];
  sprintf(line, "ext4_read_block_bitmap_load: dev %d,%d group %u\\n",
          major(event.dev()), minor(event.dev()), event.group());
  return std::string(line);
}

std::string FormatExt4Readpage(const Ext4ReadpageFtraceEvent& event) {
  char line[2048];
  sprintf(line, "ext4_readpage: dev %d,%d ino %lu page_index %lu\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          (unsigned long)event.index());
  return std::string(line);
}

std::string FormatExt4Releasepage(const Ext4ReleasepageFtraceEvent& event) {
  char line[2048];
  sprintf(line, "ext4_releasepage: dev %d,%d ino %lu page_index %lu\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          (unsigned long)event.index());
  return std::string(line);
}

std::string FormatExt4RemoveBlocks(const Ext4RemoveBlocksFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_remove_blocks: dev %d,%d ino %lu extent [%u(%llu), %u]from %u "
          "to %u partial_cluster %lld\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          (unsigned)event.ee_lblk(), (unsigned long long)event.ee_pblk(),
          (unsigned short)event.ee_len(), (unsigned)event.from(),
          (unsigned)event.to(), (long long)event.partial());
  return std::string(line);
}

std::string FormatExt4RequestBlocks(const Ext4RequestBlocksFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_request_blocks: dev %d,%d ino %lu flags %s len %u lblk %u goal "
          "%llu lleft %u lright %u pleft %llu pright %llu \\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          GetExt4HintFlag(event.flags()), event.len(), event.logical(),
          event.goal(), event.lleft(), event.lright(), event.pleft(),
          event.pright());
  return std::string(line);
}

std::string FormatExt4RequestInode(const Ext4RequestInodeFtraceEvent& event) {
  char line[2048];
  sprintf(line, "ext4_request_inode: dev %d,%d dir %lu mode 0%o\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.dir(),
          event.mode());
  return std::string(line);
}

std::string FormatExt4SyncFs(const Ext4SyncFsFtraceEvent& event) {
  char line[2048];
  sprintf(line, "ext4_sync_fs: dev %d,%d wait %d\\n", major(event.dev()),
          minor(event.dev()), event.wait());
  return std::string(line);
}

std::string FormatExt4TrimAllFree(const Ext4TrimAllFreeFtraceEvent& event) {
  char line[2048];
  sprintf(line, "ext4_trim_all_free: dev %d,%d group %u, start %d, len %d\\n",
          event.dev_major(), event.dev_minor(), event.group(), event.start(),
          event.len());
  return std::string(line);
}

std::string FormatExt4TrimExtent(const Ext4TrimExtentFtraceEvent& event) {
  char line[2048];
  sprintf(line, "ext4_trim_extent: dev %d,%d group %u, start %d, len %d\\n",
          event.dev_major(), event.dev_minor(), event.group(), event.start(),
          event.len());
  return std::string(line);
}

std::string FormatExt4TruncateEnter(const Ext4TruncateEnterFtraceEvent& event) {
  char line[2048];
  sprintf(line, "ext4_truncate_enter: dev %d,%d ino %lu blocks %llu\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          event.blocks());
  return std::string(line);
}

std::string FormatExt4TruncateExit(const Ext4TruncateExitFtraceEvent& event) {
  char line[2048];
  sprintf(line, "ext4_truncate_exit: dev %d,%d ino %lu blocks %llu\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          event.blocks());
  return std::string(line);
}

std::string FormatExt4UnlinkEnter(const Ext4UnlinkEnterFtraceEvent& event) {
  char line[2048];
  sprintf(line, "ext4_unlink_enter: dev %d,%d ino %lu size %lld parent %lu\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          event.size(), (unsigned long)event.parent());
  return std::string(line);
}

std::string FormatExt4UnlinkExit(const Ext4UnlinkExitFtraceEvent& event) {
  char line[2048];
  sprintf(line, "ext4_unlink_exit: dev %d,%d ino %lu ret %d\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          event.ret());
  return std::string(line);
}

std::string FormatExt4WriteBegin(const Ext4WriteBeginFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_write_begin: dev %d,%d ino %lu pos %lld len %u flags %u\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          event.pos(), event.len(), event.flags());
  return std::string(line);
}

std::string FormatExt4WriteEnd(const Ext4WriteEndFtraceEvent& event) {
  char line[2048];
  sprintf(line, "ext4_write_end: %d,%d ino %lu pos %lld len %u copied %u\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          event.pos(), event.len(), event.copied());
  return std::string(line);
}

std::string FormatExt4Writepage(const Ext4WritepageFtraceEvent& event) {
  char line[2048];
  sprintf(line, "ext4_writepage: dev %d,%d ino %lu page_index %lu\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          (unsigned long)event.index());
  return std::string(line);
}

std::string FormatExt4Writepages(const Ext4WritepagesFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_writepages: dev %d,%d ino %lu nr_to_write %ld pages_skipped "
          "%ld range_start %lld range_end %lld sync_mode %d for_kupdate %d "
          "range_cyclic %d writeback_index %lu\\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          (long)event.nr_to_write(), (long)event.pages_skipped(),
          event.range_start(), event.range_end(), event.sync_mode(),
          event.for_kupdate(), event.range_cyclic(),
          (unsigned long)event.writeback_index());
  return std::string(line);
}

std::string FormatExt4WritepagesResult(
    const Ext4WritepagesResultFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_writepages_result: dev %d,%d ino %lu ret %d pages_written %d "
          "pages_skipped %ld sync_mode %d writeback_index %lu \\n",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          event.ret(), event.pages_written(), (long)event.pages_skipped(),
          event.sync_mode(), (unsigned long)event.writeback_index());
  return std::string(line);
}

std::string FormatExt4ZeroRange(const Ext4ZeroRangeFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_zero_range: dev %d,%d ino %lu offset %lld len %lld mode %s\\n ",
          major(event.dev()), minor(event.dev()), (unsigned long)event.ino(),
          event.offset(), event.len(), GetExt4ModeFlag(event.mode()));
  return std::string(line);
}

// TODO(taylori): Confirm correct format for this.
// Calling this breaks loading into chrome://tracing
std::string FormatProcess(const Process& process) {
  char line[2048];
  sprintf(line, "process: pid=%d ppid=%d cmdline=", process.pid(),
          process.ppid());
  std::string output = std::string(line);
  for (auto field : process.cmdline()) {
    char cmd[2048];
    sprintf(cmd, "%s ", field.c_str());
    output += std::string(cmd);
  }
  output += "\\n";
  for (auto thread : process.threads()) {
    char thread_line[2048];
    sprintf(thread_line, "thread: tid=%d name=%s\\n", thread.tid(),
            thread.name().c_str());
    output += thread_line;
  }
  return output;
}

// Calling this breaks loading into chrome://tracing
std::string FormatInodeFileMap(const Entry& entry) {
  char line[2048];
  sprintf(line, "inode_file_map: ino=%llu type=%s path=", entry.inode_number(),
          inodeFileTypeArray[entry.type()]);
  std::string output = std::string(line);
  for (auto field : entry.paths()) {
    char path[2048];
    sprintf(path, "%s", field.c_str());
    output += std::string(path);
  }
  return output;
}

int TraceToSystrace(std::istream* input, std::ostream* output) {
  std::multimap<uint64_t, std::string> sorted;

  std::string raw;
  std::istreambuf_iterator<char> begin(*input), end;
  raw.assign(begin, end);
  Trace trace;
  if (!trace.ParseFromString(raw)) {
    PERFETTO_ELOG("Could not parse input.");
    return 1;
  }

  for (const TracePacket& packet : trace.packet()) {

    if (!packet.has_ftrace_events())
      continue;

    const FtraceEventBundle& bundle = packet.ftrace_events();
    for (const FtraceEvent& event : bundle.event()) {
      std::string line;
      if (event.has_binder_lock()) {
        const auto& inner = event.binder_lock();
        line = FormatBinderLock(inner);
      } else if (event.has_binder_locked()) {
        const auto& inner = event.binder_locked();
        line = FormatBinderLocked(inner);
      } else if (event.has_binder_transaction()) {
        const auto& inner = event.binder_transaction();
        line = FormatBinderTransaction(inner);
      } else if (event.has_binder_transaction_received()) {
        const auto& inner = event.binder_transaction_received();
        line = FormatBinderTransactionReceived(inner);
      } else if (event.has_binder_unlock()) {
        const auto& inner = event.binder_unlock();
        line = FormatBinderUnlock(inner);
      } else if (event.has_block_bio_backmerge()) {
        const auto& inner = event.block_bio_backmerge();
        line = FormatBlockBioBackmerge(inner);
      } else if (event.has_block_bio_bounce()) {
        const auto& inner = event.block_bio_bounce();
        line = FormatBlockBioBounce(inner);
      } else if (event.has_block_bio_complete()) {
        const auto& inner = event.block_bio_complete();
        line = FormatBlockBioComplete(inner);
      } else if (event.has_block_bio_frontmerge()) {
        const auto& inner = event.block_bio_frontmerge();
        line = FormatBlockBioFrontmerge(inner);
      } else if (event.has_block_bio_queue()) {
        const auto& inner = event.block_bio_queue();
        line = FormatBlockBioQueue(inner);
      } else if (event.has_block_bio_remap()) {
        const auto& inner = event.block_bio_remap();
        line = FormatBlockBioRemap(inner);
      } else if (event.has_block_dirty_buffer()) {
        const auto& inner = event.block_dirty_buffer();
        line = FormatBlockDirtyBuffer(inner);
      } else if (event.has_block_getrq()) {
        const auto& inner = event.block_getrq();
        line = FormatBlockGetrq(inner);
      } else if (event.has_block_plug()) {
        const auto& inner = event.block_plug();
        line = FormatBlockPlug(inner);
      } else if (event.has_block_rq_abort()) {
        const auto& inner = event.block_rq_abort();
        line = FormatBlockRqAbort(inner);
      } else if (event.has_block_rq_complete()) {
        const auto& inner = event.block_rq_complete();
        line = FormatBlockRqComplete(inner);
      } else if (event.has_block_rq_insert()) {
        const auto& inner = event.block_rq_insert();
        line = FormatBlockRqInsert(inner);
      } else if (event.has_block_rq_issue()) {
        const auto& inner = event.block_rq_issue();
        line = FormatBlockRqIssue(inner);
      } else if (event.has_block_rq_remap()) {
        const auto& inner = event.block_rq_remap();
        line = FormatBlockRqRemap(inner);
      } else if (event.has_block_rq_requeue()) {
        const auto& inner = event.block_rq_requeue();
        line = FormatBlockRqRequeue(inner);
      } else if (event.has_block_sleeprq()) {
        const auto& inner = event.block_sleeprq();
        line = FormatBlockSleeprq(inner);
      } else if (event.has_block_split()) {
        const auto& inner = event.block_split();
        line = FormatBlockSplit(inner);
      } else if (event.has_block_touch_buffer()) {
        const auto& inner = event.block_touch_buffer();
        line = FormatBlockTouchBuffer(inner);
      } else if (event.has_block_unplug()) {
        const auto& inner = event.block_unplug();
        line = FormatBlockUnplug(inner);
      } else if (event.has_mm_compaction_begin()) {
        const auto& inner = event.mm_compaction_begin();
        line = FormatMmCompactionBegin(inner);
      } else if (event.has_mm_compaction_defer_compaction()) {
        const auto& inner = event.mm_compaction_defer_compaction();
        line = FormatMmCompactionDeferCompaction(inner);
      } else if (event.has_mm_compaction_defer_reset()) {
        const auto& inner = event.mm_compaction_defer_reset();
        line = FormatMmCompactionDeferReset(inner);
      } else if (event.has_mm_compaction_deferred()) {
        const auto& inner = event.mm_compaction_deferred();
        line = FormatMmCompactionDeferred(inner);
      } else if (event.has_mm_compaction_end()) {
        const auto& inner = event.mm_compaction_end();
        line = FormatMmCompactionEnd(inner);
      } else if (event.has_mm_compaction_finished()) {
        const auto& inner = event.mm_compaction_finished();
        line = FormatMmCompactionFinished(inner);
      } else if (event.has_mm_compaction_isolate_freepages()) {
        const auto& inner = event.mm_compaction_isolate_freepages();
        line = FormatMmCompactionIsolateFreepages(inner);
      } else if (event.has_mm_compaction_isolate_migratepages()) {
        const auto& inner = event.mm_compaction_isolate_migratepages();
        line = FormatMmCompactionIsolateMigratepages(inner);
      } else if (event.has_mm_compaction_kcompactd_sleep()) {
        const auto& inner = event.mm_compaction_kcompactd_sleep();
        line = FormatMmCompactionKcompactdSleep(inner);
      } else if (event.has_mm_compaction_kcompactd_wake()) {
        const auto& inner = event.mm_compaction_kcompactd_wake();
        line = FormatMmCompactionKcompactdWake(inner);
      } else if (event.has_mm_compaction_migratepages()) {
        const auto& inner = event.mm_compaction_migratepages();
        line = FormatMmCompactionMigratepages(inner);
      } else if (event.has_mm_compaction_suitable()) {
        const auto& inner = event.mm_compaction_suitable();
        line = FormatMmCompactionSuitable(inner);
      } else if (event.has_mm_compaction_try_to_compact_pages()) {
        const auto& inner = event.mm_compaction_try_to_compact_pages();
        line = FormatMmCompactionTryToCompactPages(inner);
      } else if (event.has_mm_compaction_wakeup_kcompactd()) {
        const auto& inner = event.mm_compaction_wakeup_kcompactd();
        line = FormatMmCompactionWakeupKcompactd(inner);
      } else if (event.has_ext4_alloc_da_blocks()) {
        const auto& inner = event.ext4_alloc_da_blocks();
        line = FormatExt4AllocDaBlocks(inner);
      } else if (event.has_ext4_allocate_blocks()) {
        const auto& inner = event.ext4_allocate_blocks();
        line = FormatExt4AllocateBlocks(inner);
      } else if (event.has_ext4_allocate_inode()) {
        const auto& inner = event.ext4_allocate_inode();
        line = FormatExt4AllocateInode(inner);
      } else if (event.has_ext4_begin_ordered_truncate()) {
        const auto& inner = event.ext4_begin_ordered_truncate();
        line = FormatExt4BeginOrderedTruncate(inner);
      } else if (event.has_ext4_collapse_range()) {
        const auto& inner = event.ext4_collapse_range();
        line = FormatExt4CollapseRange(inner);
      } else if (event.has_ext4_da_release_space()) {
        const auto& inner = event.ext4_da_release_space();
        line = FormatExt4DaReleaseSpace(inner);
      } else if (event.has_ext4_da_reserve_space()) {
        const auto& inner = event.ext4_da_reserve_space();
        line = FormatExt4DaReserveSpace(inner);
      } else if (event.has_ext4_da_update_reserve_space()) {
        const auto& inner = event.ext4_da_update_reserve_space();
        line = FormatExt4DaUpdateReserveSpace(inner);
      } else if (event.has_ext4_da_write_begin()) {
        const auto& inner = event.ext4_da_write_begin();
        line = FormatExt4DaWriteBegin(inner);
      } else if (event.has_ext4_da_write_end()) {
        const auto& inner = event.ext4_da_write_end();
        line = FormatExt4DaWriteEnd(inner);
      } else if (event.has_ext4_da_write_pages()) {
        const auto& inner = event.ext4_da_write_pages();
        line = FormatExt4DaWritePages(inner);
      } else if (event.has_ext4_da_write_pages_extent()) {
        const auto& inner = event.ext4_da_write_pages_extent();
        line = FormatExt4DaWritePagesExtent(inner);
      } else if (event.has_ext4_discard_blocks()) {
        const auto& inner = event.ext4_discard_blocks();
        line = FormatExt4DiscardBlocks(inner);
      } else if (event.has_ext4_discard_preallocations()) {
        const auto& inner = event.ext4_discard_preallocations();
        line = FormatExt4DiscardPreallocations(inner);
      } else if (event.has_ext4_drop_inode()) {
        const auto& inner = event.ext4_drop_inode();
        line = FormatExt4DropInode(inner);
      } else if (event.has_ext4_es_cache_extent()) {
        const auto& inner = event.ext4_es_cache_extent();
        line = FormatExt4EsCacheExtent(inner);
      } else if (event.has_ext4_es_find_delayed_extent_range_enter()) {
        const auto& inner = event.ext4_es_find_delayed_extent_range_enter();
        line = FormatExt4EsFindDelayedExtentRangeEnter(inner);
      } else if (event.has_ext4_es_find_delayed_extent_range_exit()) {
        const auto& inner = event.ext4_es_find_delayed_extent_range_exit();
        line = FormatExt4EsFindDelayedExtentRangeExit(inner);
      } else if (event.has_ext4_es_insert_extent()) {
        const auto& inner = event.ext4_es_insert_extent();
        line = FormatExt4EsInsertExtent(inner);
      } else if (event.has_ext4_es_lookup_extent_enter()) {
        const auto& inner = event.ext4_es_lookup_extent_enter();
        line = FormatExt4EsLookupExtentEnter(inner);
      } else if (event.has_ext4_es_lookup_extent_exit()) {
        const auto& inner = event.ext4_es_lookup_extent_exit();
        line = FormatExt4EsLookupExtentExit(inner);
      } else if (event.has_ext4_es_remove_extent()) {
        const auto& inner = event.ext4_es_remove_extent();
        line = FormatExt4EsRemoveExtent(inner);
      } else if (event.has_ext4_es_shrink()) {
        const auto& inner = event.ext4_es_shrink();
        line = FormatExt4EsShrink(inner);
      } else if (event.has_ext4_es_shrink_count()) {
        const auto& inner = event.ext4_es_shrink_count();
        line = FormatExt4EsShrinkCount(inner);
      } else if (event.has_ext4_es_shrink_scan_enter()) {
        const auto& inner = event.ext4_es_shrink_scan_enter();
        line = FormatExt4EsShrinkScanEnter(inner);
      } else if (event.has_ext4_es_shrink_scan_exit()) {
        const auto& inner = event.ext4_es_shrink_scan_exit();
        line = FormatExt4EsShrinkScanExit(inner);
      } else if (event.has_ext4_evict_inode()) {
        const auto& inner = event.ext4_evict_inode();
        line = FormatExt4EvictInode(inner);
      } else if (event.has_ext4_ext_convert_to_initialized_enter()) {
        const auto& inner = event.ext4_ext_convert_to_initialized_enter();
        line = FormatExt4ExtConvertToInitializedEnter(inner);
      } else if (event.has_ext4_ext_convert_to_initialized_fastpath()) {
        const auto& inner = event.ext4_ext_convert_to_initialized_fastpath();
        line = FormatExt4ExtConvertToInitializedFastpath(inner);
      } else if (event.has_ext4_ext_handle_unwritten_extents()) {
        const auto& inner = event.ext4_ext_handle_unwritten_extents();
        line = FormatExt4ExtHandleUnwrittenExtents(inner);
      } else if (event.has_ext4_ext_in_cache()) {
        const auto& inner = event.ext4_ext_in_cache();
        line = FormatExt4ExtInCache(inner);
      } else if (event.has_ext4_ext_load_extent()) {
        const auto& inner = event.ext4_ext_load_extent();
        line = FormatExt4ExtLoadExtent(inner);
      } else if (event.has_ext4_ext_map_blocks_enter()) {
        const auto& inner = event.ext4_ext_map_blocks_enter();
        line = FormatExt4ExtMapBlocksEnter(inner);
      } else if (event.has_ext4_ext_map_blocks_exit()) {
        const auto& inner = event.ext4_ext_map_blocks_exit();
        line = FormatExt4ExtMapBlocksExit(inner);
      } else if (event.has_ext4_ext_put_in_cache()) {
        const auto& inner = event.ext4_ext_put_in_cache();
        line = FormatExt4ExtPutInCache(inner);
      } else if (event.has_ext4_ext_remove_space()) {
        const auto& inner = event.ext4_ext_remove_space();
        line = FormatExt4ExtRemoveSpace(inner);
      } else if (event.has_ext4_ext_remove_space_done()) {
        const auto& inner = event.ext4_ext_remove_space_done();
        line = FormatExt4ExtRemoveSpaceDone(inner);
      } else if (event.has_ext4_ext_rm_idx()) {
        const auto& inner = event.ext4_ext_rm_idx();
        line = FormatExt4ExtRmIdx(inner);
      } else if (event.has_ext4_ext_rm_leaf()) {
        const auto& inner = event.ext4_ext_rm_leaf();
        line = FormatExt4ExtRmLeaf(inner);
      } else if (event.has_ext4_ext_show_extent()) {
        const auto& inner = event.ext4_ext_show_extent();
        line = FormatExt4ExtShowExtent(inner);
      } else if (event.has_ext4_fallocate_enter()) {
        const auto& inner = event.ext4_fallocate_enter();
        line = FormatExt4FallocateEnter(inner);
      } else if (event.has_ext4_fallocate_exit()) {
        const auto& inner = event.ext4_fallocate_exit();
        line = FormatExt4FallocateExit(inner);
      } else if (event.has_ext4_find_delalloc_range()) {
        const auto& inner = event.ext4_find_delalloc_range();
        line = FormatExt4FindDelallocRange(inner);
      } else if (event.has_ext4_forget()) {
        const auto& inner = event.ext4_forget();
        line = FormatExt4Forget(inner);
      } else if (event.has_ext4_free_blocks()) {
        const auto& inner = event.ext4_free_blocks();
        line = FormatExt4FreeBlocks(inner);
      } else if (event.has_ext4_free_inode()) {
        const auto& inner = event.ext4_free_inode();
        line = FormatExt4FreeInode(inner);
      } else if (event.has_ext4_get_implied_cluster_alloc_exit()) {
        const auto& inner = event.ext4_get_implied_cluster_alloc_exit();
        line = FormatExt4GetImpliedClusterAllocExit(inner);
      } else if (event.has_ext4_get_reserved_cluster_alloc()) {
        const auto& inner = event.ext4_get_reserved_cluster_alloc();
        line = FormatExt4GetReservedClusterAlloc(inner);
      } else if (event.has_ext4_ind_map_blocks_enter()) {
        const auto& inner = event.ext4_ind_map_blocks_enter();
        line = FormatExt4IndMapBlocksEnter(inner);
      } else if (event.has_ext4_ind_map_blocks_exit()) {
        const auto& inner = event.ext4_ind_map_blocks_exit();
        line = FormatExt4IndMapBlocksExit(inner);
      } else if (event.has_ext4_insert_range()) {
        const auto& inner = event.ext4_insert_range();
        line = FormatExt4InsertRange(inner);
      } else if (event.has_ext4_invalidatepage()) {
        const auto& inner = event.ext4_invalidatepage();
        line = FormatExt4Invalidatepage(inner);
      } else if (event.has_ext4_journal_start()) {
        const auto& inner = event.ext4_journal_start();
        line = FormatExt4JournalStart(inner);
      } else if (event.has_ext4_journal_start_reserved()) {
        const auto& inner = event.ext4_journal_start_reserved();
        line = FormatExt4JournalStartReserved(inner);
      } else if (event.has_ext4_journalled_invalidatepage()) {
        const auto& inner = event.ext4_journalled_invalidatepage();
        line = FormatExt4JournalledInvalidatepage(inner);
      } else if (event.has_ext4_journalled_write_end()) {
        const auto& inner = event.ext4_journalled_write_end();
        line = FormatExt4JournalledWriteEnd(inner);
      } else if (event.has_ext4_load_inode()) {
        const auto& inner = event.ext4_load_inode();
        line = FormatExt4LoadInode(inner);
      } else if (event.has_ext4_load_inode_bitmap()) {
        const auto& inner = event.ext4_load_inode_bitmap();
        line = FormatExt4LoadInodeBitmap(inner);
      } else if (event.has_ext4_mark_inode_dirty()) {
        const auto& inner = event.ext4_mark_inode_dirty();
        line = FormatExt4MarkInodeDirty(inner);
      } else if (event.has_ext4_mb_bitmap_load()) {
        const auto& inner = event.ext4_mb_bitmap_load();
        line = FormatExt4MbBitmapLoad(inner);
      } else if (event.has_ext4_mb_buddy_bitmap_load()) {
        const auto& inner = event.ext4_mb_buddy_bitmap_load();
        line = FormatExt4MbBuddyBitmapLoad(inner);
      } else if (event.has_ext4_mb_discard_preallocations()) {
        const auto& inner = event.ext4_mb_discard_preallocations();
        line = FormatExt4MbDiscardPreallocations(inner);
      } else if (event.has_ext4_mb_new_group_pa()) {
        const auto& inner = event.ext4_mb_new_group_pa();
        line = FormatExt4MbNewGroupPa(inner);
      } else if (event.has_ext4_mb_new_inode_pa()) {
        const auto& inner = event.ext4_mb_new_inode_pa();
        line = FormatExt4MbNewInodePa(inner);
      } else if (event.has_ext4_mb_release_group_pa()) {
        const auto& inner = event.ext4_mb_release_group_pa();
        line = FormatExt4MbReleaseGroupPa(inner);
      } else if (event.has_ext4_mb_release_inode_pa()) {
        const auto& inner = event.ext4_mb_release_inode_pa();
        line = FormatExt4MbReleaseInodePa(inner);
      } else if (event.has_ext4_mballoc_alloc()) {
        const auto& inner = event.ext4_mballoc_alloc();
        line = FormatExt4MballocAlloc(inner);
      } else if (event.has_ext4_mballoc_discard()) {
        const auto& inner = event.ext4_mballoc_discard();
        line = FormatExt4MballocDiscard(inner);
      } else if (event.has_ext4_mballoc_free()) {
        const auto& inner = event.ext4_mballoc_free();
        line = FormatExt4MballocFree(inner);
      } else if (event.has_ext4_mballoc_prealloc()) {
        const auto& inner = event.ext4_mballoc_prealloc();
        line = FormatExt4MballocPrealloc(inner);
      } else if (event.has_ext4_other_inode_update_time()) {
        const auto& inner = event.ext4_other_inode_update_time();
        line = FormatExt4OtherInodeUpdateTime(inner);
      } else if (event.has_ext4_punch_hole()) {
        const auto& inner = event.ext4_punch_hole();
        line = FormatExt4PunchHole(inner);
      } else if (event.has_ext4_read_block_bitmap_load()) {
        const auto& inner = event.ext4_read_block_bitmap_load();
        line = FormatExt4ReadBlockBitmapLoad(inner);
      } else if (event.has_ext4_readpage()) {
        const auto& inner = event.ext4_readpage();
        line = FormatExt4Readpage(inner);
      } else if (event.has_ext4_releasepage()) {
        const auto& inner = event.ext4_releasepage();
        line = FormatExt4Releasepage(inner);
      } else if (event.has_ext4_remove_blocks()) {
        const auto& inner = event.ext4_remove_blocks();
        line = FormatExt4RemoveBlocks(inner);
      } else if (event.has_ext4_request_blocks()) {
        const auto& inner = event.ext4_request_blocks();
        line = FormatExt4RequestBlocks(inner);
      } else if (event.has_ext4_request_inode()) {
        const auto& inner = event.ext4_request_inode();
        line = FormatExt4RequestInode(inner);
      } else if (event.has_ext4_sync_file_enter()) {
        const auto& inner = event.ext4_sync_file_enter();
        line = FormatExt4SyncFileEnter(inner);
      } else if (event.has_ext4_sync_file_exit()) {
        const auto& inner = event.ext4_sync_file_exit();
        line = FormatExt4SyncFileExit(inner);
      } else if (event.has_ext4_sync_fs()) {
        const auto& inner = event.ext4_sync_fs();
        line = FormatExt4SyncFs(inner);
      } else if (event.has_ext4_trim_all_free()) {
        const auto& inner = event.ext4_trim_all_free();
        line = FormatExt4TrimAllFree(inner);
      } else if (event.has_ext4_trim_extent()) {
        const auto& inner = event.ext4_trim_extent();
        line = FormatExt4TrimExtent(inner);
      } else if (event.has_ext4_truncate_enter()) {
        const auto& inner = event.ext4_truncate_enter();
        line = FormatExt4TruncateEnter(inner);
      } else if (event.has_ext4_truncate_exit()) {
        const auto& inner = event.ext4_truncate_exit();
        line = FormatExt4TruncateExit(inner);
      } else if (event.has_ext4_unlink_enter()) {
        const auto& inner = event.ext4_unlink_enter();
        line = FormatExt4UnlinkEnter(inner);
      } else if (event.has_ext4_unlink_exit()) {
        const auto& inner = event.ext4_unlink_exit();
        line = FormatExt4UnlinkExit(inner);
      } else if (event.has_ext4_write_begin()) {
        const auto& inner = event.ext4_write_begin();
        line = FormatExt4WriteBegin(inner);
      } else if (event.has_ext4_write_end()) {
        const auto& inner = event.ext4_write_end();
        line = FormatExt4WriteEnd(inner);
      } else if (event.has_ext4_writepage()) {
        const auto& inner = event.ext4_writepage();
        line = FormatExt4Writepage(inner);
      } else if (event.has_ext4_writepages()) {
        const auto& inner = event.ext4_writepages();
        line = FormatExt4Writepages(inner);
      } else if (event.has_ext4_writepages_result()) {
        const auto& inner = event.ext4_writepages_result();
        line = FormatExt4WritepagesResult(inner);
      } else if (event.has_ext4_zero_range()) {
        const auto& inner = event.ext4_zero_range();
        line = FormatExt4ZeroRange(inner);
      } else if (event.has_print()) {
        const auto& inner = event.print();
        line = FormatPrint(inner);
      } else if (event.has_i2c_read()) {
        const auto& inner = event.i2c_read();
        line = FormatI2cRead(inner);
      } else if (event.has_i2c_reply()) {
        const auto& inner = event.i2c_reply();
        line = FormatI2cReply(inner);
      } else if (event.has_i2c_result()) {
        const auto& inner = event.i2c_result();
        line = FormatI2cResult(inner);
      } else if (event.has_i2c_write()) {
        const auto& inner = event.i2c_write();
        line = FormatI2cWrite(inner);
      } else if (event.has_irq_handler_entry()) {
        const auto& inner = event.irq_handler_entry();
        line = FormatIrqHandlerEntry(inner);
      } else if (event.has_irq_handler_exit()) {
        const auto& inner = event.irq_handler_exit();
        line = FormatIrqHandlerExit(inner);
      } else if (event.has_softirq_entry()) {
        const auto& inner = event.softirq_entry();
        line = FormatSoftirqEntry(inner);
      } else if (event.has_softirq_exit()) {
        const auto& inner = event.softirq_exit();
        line = FormatSoftirqExit(inner);
      } else if (event.has_softirq_raise()) {
        const auto& inner = event.softirq_raise();
        line = FormatSoftirqRaise(inner);
      } else if (event.has_lowmemory_kill()) {
        const auto& inner = event.lowmemory_kill();
        line = FormatLowmemoryKill(inner);
      } else if (event.has_tracing_mark_write()) {
        const auto& inner = event.tracing_mark_write();
        line = FormatTracingMarkWrite(inner);
      } else if (event.has_clock_disable()) {
        const auto& inner = event.clock_disable();
        line = FormatClockDisable(inner);
      } else if (event.has_clock_enable()) {
        const auto& inner = event.clock_enable();
        line = FormatClockEnable(inner);
      } else if (event.has_clock_set_rate()) {
        const auto& inner = event.clock_set_rate();
        line = FormatClockSetRate(inner);
      } else if (event.has_cpu_frequency()) {
        const auto& inner = event.cpu_frequency();
        line = FormatCpuFrequency(inner);
      } else if (event.has_cpu_frequency_limits()) {
        const auto& inner = event.cpu_frequency_limits();
        line = FormatCpuFrequencyLimits(inner);
      } else if (event.has_cpu_idle()) {
        const auto& inner = event.cpu_idle();
        line = FormatCpuIdle(inner);
      } else if (event.has_suspend_resume()) {
        const auto& inner = event.suspend_resume();
        line = FormatSuspendResume(inner);
      } else if (event.has_regulator_disable()) {
        const auto& inner = event.regulator_disable();
        line = FormatRegulatorDisable(inner);
      } else if (event.has_regulator_disable_complete()) {
        const auto& inner = event.regulator_disable_complete();
        line = FormatRegulatorDisableComplete(inner);
      } else if (event.has_regulator_enable()) {
        const auto& inner = event.regulator_enable();
        line = FormatRegulatorEnable(inner);
      } else if (event.has_regulator_enable_complete()) {
        const auto& inner = event.regulator_enable_complete();
        line = FormatRegulatorEnableComplete(inner);
      } else if (event.has_regulator_enable_delay()) {
        const auto& inner = event.regulator_enable_delay();
        line = FormatRegulatorEnableDelay(inner);
      } else if (event.has_regulator_set_voltage()) {
        const auto& inner = event.regulator_set_voltage();
        line = FormatRegulatorSetVoltage(inner);
      } else if (event.has_regulator_set_voltage_complete()) {
        const auto& inner = event.regulator_set_voltage_complete();
        line = FormatRegulatorSetVoltageComplete(inner);
      } else if (event.has_sched_blocked_reason()) {
        const auto& inner = event.sched_blocked_reason();
        line = FormatSchedBlockedReason(inner);
      } else if (event.has_sched_cpu_hotplug()) {
        const auto& inner = event.sched_cpu_hotplug();
        line = FormatSchedCpuHotplug(inner);
      } else if (event.has_sched_switch()) {
        const auto& inner = event.sched_switch();
        line = FormatSchedSwitch(inner);
      } else if (event.has_sched_wakeup()) {
        const auto& inner = event.sched_wakeup();
        line = FormatSchedWakeup(inner);
      } else if (event.has_sched_wakeup_new()) {
        const auto& inner = event.sched_wakeup_new();
        line = FormatSchedWakeupNew(inner);
      } else if (event.has_sync_pt()) {
        const auto& inner = event.sync_pt();
        line = FormatSyncPt(inner);
      } else if (event.has_sync_timeline()) {
        const auto& inner = event.sync_timeline();
        line = FormatSyncTimeline(inner);
      } else if (event.has_sync_wait()) {
        const auto& inner = event.sync_wait();
        line = FormatSyncWait(inner);
      } else if (event.has_mm_vmscan_direct_reclaim_begin()) {
        const auto& inner = event.mm_vmscan_direct_reclaim_begin();
        line = FormatMmVmscanDirectReclaimBegin(inner);
      } else if (event.has_mm_vmscan_direct_reclaim_end()) {
        const auto& inner = event.mm_vmscan_direct_reclaim_end();
        line = FormatMmVmscanDirectReclaimEnd(inner);
      } else if (event.has_mm_vmscan_kswapd_sleep()) {
        const auto& inner = event.mm_vmscan_kswapd_sleep();
        line = FormatMmVmscanKswapdSleep(inner);
      } else if (event.has_mm_vmscan_kswapd_wake()) {
        const auto& inner = event.mm_vmscan_kswapd_wake();
        line = FormatMmVmscanKswapdWake(inner);
      } else if (event.has_workqueue_activate_work()) {
        const auto& inner = event.workqueue_activate_work();
        line = FormatWorkqueueActivateWork(inner);
      } else if (event.has_workqueue_execute_end()) {
        const auto& inner = event.workqueue_execute_end();
        line = FormatWorkqueueExecuteEnd(inner);
      } else if (event.has_workqueue_execute_start()) {
        const auto& inner = event.workqueue_execute_start();
        line = FormatWorkqueueExecuteStart(inner);
      } else if (event.has_workqueue_queue_work()) {
        const auto& inner = event.workqueue_queue_work();
        line = FormatWorkqueueQueueWork(inner);
      } else if (event.has_sched_process_fork()) {
        const auto& inner = event.sched_process_fork();
        line = FormatSchedProcessFork(inner);
      } else if (event.has_sched_process_hang()) {
        const auto& inner = event.sched_process_hang();
        line = FormatSchedProcessHang(inner);
      } else if (event.has_sched_process_free()) {
        const auto& inner = event.sched_process_free();
        line = FormatSchedProcessFree(inner);
      } else if (event.has_sched_process_exec()) {
        const auto& inner = event.sched_process_exec();
        line = FormatSchedProcessExec(inner);
      } else if (event.has_sched_process_exit()) {
        const auto& inner = event.sched_process_exit();
        line = FormatSchedProcessExit(inner);
      } else if (event.has_sched_process_wait()) {
        const auto& inner = event.sched_process_wait();
        line = FormatSchedProcessWait(inner);
      } else if (event.has_task_rename()) {
        const auto& inner = event.task_rename();
        line = FormatTaskRename(inner);
      } else if (event.has_task_newtask()) {
        const auto& inner = event.task_newtask();
        line = FormatTaskNewtask(inner);
      } else {
        continue;
      }
      sorted.emplace(event.timestamp(),
                     FormatPrefix(event.timestamp(), bundle.cpu()) + line);
    }
  }

  *output << kTraceHeader;
  *output << kFtraceHeader;

  for (auto it = sorted.begin(); it != sorted.end(); it++)
    *output << it->second;

  *output << kTraceFooter;

  return 0;
}

}  // namespace
}  // namespace perfetto

namespace {

int Usage(int argc, char** argv) {
  printf("Usage: %s [systrace|text] < trace.proto > trace.txt\n", argv[0]);
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2)
    return Usage(argc, argv);

  std::string format(argv[1]);

  if (format != "systrace" && format != "text")
    return Usage(argc, argv);

  bool systrace = format == "systrace";

  if (systrace) {
    return perfetto::TraceToSystrace(&std::cin, &std::cout);
  } else {
    return perfetto::TraceToText(&std::cin, &std::cout);
  }
}
