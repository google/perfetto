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
using protos::FtraceEvent;
using protos::FtraceEventBundle;
using protos::PrintFtraceEvent;
using protos::SchedSwitchFtraceEvent;
using protos::SchedWakeupFtraceEvent;
using protos::CpuFrequencyFtraceEvent;
using protos::CpuFrequencyLimitsFtraceEvent;
using protos::CpuIdleFtraceEvent;
using protos::ClockEnableFtraceEvent;
using protos::ClockDisableFtraceEvent;
using protos::ClockSetRateFtraceEvent;
using protos::SchedCpuHotplugFtraceEvent;
using protos::SchedWakingFtraceEvent;
using protos::IpiEntryFtraceEvent;
using protos::IpiExitFtraceEvent;
using protos::IpiRaiseFtraceEvent;
using protos::I2cReadFtraceEvent;
using protos::I2cWriteFtraceEvent;
using protos::I2cResultFtraceEvent;
using protos::I2cReplyFtraceEvent;
using protos::SmbusReadFtraceEvent;
using protos::SmbusWriteFtraceEvent;
using protos::SmbusResultFtraceEvent;
using protos::SmbusReplyFtraceEvent;
using protos::IrqHandlerEntryFtraceEvent;
using protos::IrqHandlerExitFtraceEvent;
using protos::SyncPtFtraceEvent;
using protos::SyncTimelineFtraceEvent;
using protos::SyncWaitFtraceEvent;
using protos::Ext4DaWriteBeginFtraceEvent;
using protos::Ext4DaWriteEndFtraceEvent;
using protos::Ext4SyncFileEnterFtraceEvent;
using protos::Ext4SyncFileExitFtraceEvent;
using protos::BlockRqIssueFtraceEvent;
using protos::MmVmscanKswapdWakeFtraceEvent;
using protos::MmVmscanKswapdSleepFtraceEvent;
using protos::BinderTransactionFtraceEvent;
using protos::BinderTransactionReceivedFtraceEvent;
using protos::BinderSetPriorityFtraceEvent;
using protos::BinderLockFtraceEvent;
using protos::BinderLockedFtraceEvent;
using protos::BinderUnlockFtraceEvent;
using protos::RegulatorDisableFtraceEvent;
using protos::RegulatorDisableCompleteFtraceEvent;
using protos::RegulatorEnableFtraceEvent;
using protos::RegulatorEnableCompleteFtraceEvent;
using protos::RegulatorEnableDelayFtraceEvent;
using protos::RegulatorSetVoltageFtraceEvent;
using protos::RegulatorSetVoltageCompleteFtraceEvent;
using protos::CgroupAttachTaskFtraceEvent;
using protos::CgroupMkdirFtraceEvent;
using protos::CgroupRemountFtraceEvent;
using protos::CgroupRmdirFtraceEvent;
using protos::CgroupTransferTasksFtraceEvent;
using protos::CgroupDestroyRootFtraceEvent;
using protos::CgroupReleaseFtraceEvent;
using protos::CgroupRenameFtraceEvent;
using protos::CgroupSetupRootFtraceEvent;
using protos::MdpCmdKickoffFtraceEvent;
using protos::MdpCommitFtraceEvent;
using protos::MdpPerfSetOtFtraceEvent;
using protos::MdpSsppChangeFtraceEvent;
using protos::TracingMarkWriteFtraceEvent;
using protos::MdpCmdPingpongDoneFtraceEvent;
using protos::MdpCompareBwFtraceEvent;
using protos::MdpPerfSetPanicLutsFtraceEvent;
using protos::MdpSsppSetFtraceEvent;
using protos::MdpCmdReadptrDoneFtraceEvent;
using protos::MdpMisrCrcFtraceEvent;
using protos::MdpPerfSetQosLutsFtraceEvent;
using protos::MdpTraceCounterFtraceEvent;
using protos::MdpCmdReleaseBwFtraceEvent;
using protos::MdpMixerUpdateFtraceEvent;
using protos::MdpPerfSetWmLevelsFtraceEvent;
using protos::MdpVideoUnderrunDoneFtraceEvent;
using protos::MdpCmdWaitPingpongFtraceEvent;
using protos::MdpPerfPrefillCalcFtraceEvent;
using protos::MdpPerfUpdateBusFtraceEvent;
using protos::RotatorBwAoAsContextFtraceEvent;
using protos::MmFilemapAddToPageCacheFtraceEvent;
using protos::MmFilemapDeleteFromPageCacheFtraceEvent;
using protos::SchedBlockedReasonFtraceEvent;
using protos::LowmemoryKillFtraceEvent;
using protos::SoftirqEntryFtraceEvent;
using protos::SoftirqExitFtraceEvent;
using protos::SoftirqRaiseFtraceEvent;
using protos::MmVmscanDirectReclaimBeginFtraceEvent;
using protos::MmVmscanDirectReclaimEndFtraceEvent;
using protos::WorkqueueExecuteEndFtraceEvent;
using protos::WorkqueueExecuteStartFtraceEvent;
using protos::WorkqueueActivateWorkFtraceEvent;
using protos::WorkqueueQueueWorkFtraceEvent;
using protos::MmCompactionBeginFtraceEvent;
using protos::MmCompactionDeferCompactionFtraceEvent;
using protos::MmCompactionDeferredFtraceEvent;
using protos::MmCompactionDeferResetFtraceEvent;
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
using protos::SuspendResumeFtraceEvent;
using protos::SchedWakeupNewFtraceEvent;
using protos::Trace;
using protos::TracePacket;

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

const char* GetFlag(int32_t state) {
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

const char* MmCompactionRetArray[] = {
    "deferred", "skipped",          "continue",          "partial",
    "complete", "no_suitable_page", "not_suitable_zone", "contended"};

const char* MmCompactionSuitableArray[] = {"DMA", "Normal", "Movable"};

const char* SoftirqArray[] = {"HI",      "TIMER",        "NET_TX",  "NET_RX",
                              "BLOCK",   "BLOCK_IOPOLL", "TASKLET", "SCHED",
                              "HRTIMER", "RCU"};

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
          sched_switch.prev_prio(), GetFlag(sched_switch.prev_state()),
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
          (unsigned int)(event.dev() >> 20),
          (unsigned int)(event.dev() & ((1U << 20) - 1)),
          (unsigned long)event.ino(), (unsigned long)event.parent(),
          event.datasync());
  return std::string(line);
}

std::string FormatExt4SyncFileExit(const Ext4SyncFileExitFtraceEvent& event) {
  char line[2048];
  sprintf(line, "ext4_sync_file_exit: dev %d,%d ino %lu ret %d\\n",
          (unsigned int)(event.dev() >> 20),
          (unsigned int)(event.dev() & ((1U << 20) - 1)),
          (unsigned long)event.ino(), event.ret());
  return std::string(line);
}

std::string FormatExt4DaWriteBegin(const Ext4DaWriteBeginFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_da_write_begin: dev %d,%d ino %lu pos %lld len %u flags %u\\n",
          (unsigned int)(event.dev() >> 20),
          (unsigned int)(event.dev() & ((1U << 20) - 1)),
          (unsigned long)event.ino(), event.pos(), event.len(), event.flags());
  return std::string(line);
}

std::string FormatExt4DaWriteEnd(const Ext4DaWriteEndFtraceEvent& event) {
  char line[2048];
  sprintf(line,
          "ext4_da_write_end: dev %d,%d ino %lu pos %lld len %u copied %u\\n",
          (unsigned int)(event.dev() >> 20),
          (unsigned int)(event.dev() & ((1U << 20) - 1)),
          (unsigned long)event.ino(), event.pos(), event.len(), event.copied());
  return std::string(line);
}

std::string FormatBlockRqIssue(const BlockRqIssueFtraceEvent& event) {
  char line[2048];
  sprintf(line, "block_rq_issue: %d,%d %s %u (%s) %llu + %u [%s]\\n",
          (unsigned int)(event.dev() >> 20),
          (unsigned int)(event.dev() & ((1U << 20) - 1)), event.rwbs().c_str(),
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
  sprintf(line, "i2c_write: i2c-%d #%u a=%03x f=%04x l=%u [%*xhD]\\n",
          event.adapter_nr(), event.msg_nr(), event.addr(), event.flags(),
          event.len(), event.len(), event.buf());
  return std::string(line);
}

std::string FormatI2cReply(const I2cReplyFtraceEvent& event) {
  char line[2048];
  sprintf(line, "i2c_reply: i2c-%d #%u a=%03x f=%04x l=%u [%*xhD]\\n",
          event.adapter_nr(), event.msg_nr(), event.addr(), event.flags(),
          event.len(), event.len(), event.buf());
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
      if (event.has_sched_switch()) {
        const auto& inner = event.sched_switch();
        line = FormatSchedSwitch(inner);
      } else if (event.has_sched_wakeup()) {
        const auto& inner = event.sched_wakeup();
        line = FormatSchedWakeup(inner);
      } else if (event.has_sched_blocked_reason()) {
        const auto& inner = event.sched_blocked_reason();
        line = FormatSchedBlockedReason(inner);
      } else if (event.has_tracing_mark_write()) {
        const auto& inner = event.tracing_mark_write();
        line = FormatTracingMarkWrite(inner);
      } else if (event.has_binder_locked()) {
        const auto& inner = event.binder_locked();
        line = FormatBinderLocked(inner);
      } else if (event.has_binder_unlock()) {
        const auto& inner = event.binder_unlock();
        line = FormatBinderUnlock(inner);
      } else if (event.has_binder_lock()) {
        const auto& inner = event.binder_lock();
        line = FormatBinderLock(inner);
      } else if (event.has_binder_transaction()) {
        const auto& inner = event.binder_transaction();
        line = FormatBinderTransaction(inner);
      } else if (event.has_binder_transaction_received()) {
        const auto& inner = event.binder_transaction_received();
        line = FormatBinderTransactionReceived(inner);
      } else if (event.has_ext4_sync_file_enter()) {
        const auto& inner = event.ext4_sync_file_enter();
        line = FormatExt4SyncFileEnter(inner);
      } else if (event.has_ext4_sync_file_exit()) {
        const auto& inner = event.ext4_sync_file_exit();
        line = FormatExt4SyncFileExit(inner);
      } else if (event.has_ext4_da_write_begin()) {
        const auto& inner = event.ext4_da_write_begin();
        line = FormatExt4DaWriteBegin(inner);
      } else if (event.has_ext4_da_write_end()) {
        const auto& inner = event.ext4_da_write_end();
        line = FormatExt4DaWriteEnd(inner);
      } else if (event.has_block_rq_issue()) {
        const auto& inner = event.block_rq_issue();
        line = FormatBlockRqIssue(inner);
      } else if (event.has_i2c_read()) {
        const auto& inner = event.i2c_read();
        line = FormatI2cRead(inner);
      } else if (event.has_i2c_result()) {
        const auto& inner = event.i2c_result();
        line = FormatI2cResult(inner);
      } else if (event.has_irq_handler_entry()) {
        const auto& inner = event.irq_handler_entry();
        line = FormatIrqHandlerEntry(inner);
      } else if (event.has_irq_handler_exit()) {
        const auto& inner = event.irq_handler_exit();
        line = FormatIrqHandlerExit(inner);
      } else if (event.has_mm_vmscan_kswapd_wake()) {
        const auto& inner = event.mm_vmscan_kswapd_wake();
        line = FormatMmVmscanKswapdWake(inner);
      } else if (event.has_mm_vmscan_kswapd_sleep()) {
        const auto& inner = event.mm_vmscan_kswapd_sleep();
        line = FormatMmVmscanKswapdSleep(inner);
      } else if (event.has_regulator_enable()) {
        const auto& inner = event.regulator_enable();
        line = FormatRegulatorEnable(inner);
      } else if (event.has_regulator_enable_delay()) {
        const auto& inner = event.regulator_enable_delay();
        line = FormatRegulatorEnableDelay(inner);
      } else if (event.has_regulator_enable_complete()) {
        const auto& inner = event.regulator_enable_complete();
        line = FormatRegulatorEnableComplete(inner);
      } else if (event.has_regulator_disable()) {
        const auto& inner = event.regulator_disable();
        line = FormatRegulatorDisable(inner);
      } else if (event.has_regulator_disable_complete()) {
        const auto& inner = event.regulator_disable_complete();
        line = FormatRegulatorDisableComplete(inner);
      } else if (event.has_regulator_set_voltage()) {
        const auto& inner = event.regulator_set_voltage();
        line = FormatRegulatorSetVoltage(inner);
      } else if (event.has_regulator_set_voltage_complete()) {
        const auto& inner = event.regulator_set_voltage_complete();
        line = FormatRegulatorSetVoltageComplete(inner);
      } else if (event.has_sched_cpu_hotplug()) {
        const auto& inner = event.sched_cpu_hotplug();
        line = FormatSchedCpuHotplug(inner);
      } else if (event.has_sync_timeline()) {
        const auto& inner = event.sync_timeline();
        line = FormatSyncTimeline(inner);
      } else if (event.has_sync_wait()) {
        const auto& inner = event.sync_wait();
        line = FormatSyncWait(inner);
      } else if (event.has_sync_pt()) {
        const auto& inner = event.sync_pt();
        line = FormatSyncPt(inner);
      } else if (event.has_print()) {
        const auto& inner = event.print();
        line = FormatPrint(inner);
      } else if (event.has_cpu_frequency()) {
        const auto& inner = event.cpu_frequency();
        line = FormatCpuFrequency(inner);
      } else if (event.has_cpu_frequency_limits()) {
        const auto& inner = event.cpu_frequency_limits();
        line = FormatCpuFrequencyLimits(inner);
      } else if (event.has_cpu_idle()) {
        const auto& inner = event.cpu_idle();
        line = FormatCpuIdle(inner);
      } else if (event.has_clock_set_rate()) {
        const auto& inner = event.clock_set_rate();
        line = FormatClockSetRate(inner);
      } else if (event.has_clock_enable()) {
        const auto& inner = event.clock_enable();
        line = FormatClockEnable(inner);
      } else if (event.has_clock_disable()) {
        const auto& inner = event.clock_disable();
        line = FormatClockDisable(inner);
      } else if (event.has_i2c_write()) {
        const auto& inner = event.i2c_write();
        line = FormatI2cWrite(inner);
      } else if (event.has_i2c_reply()) {
        const auto& inner = event.i2c_reply();
        line = FormatI2cReply(inner);
      } else if (event.has_softirq_raise()) {
        const auto& inner = event.softirq_raise();
        line = FormatSoftirqRaise(inner);
      } else if (event.has_softirq_entry()) {
        const auto& inner = event.softirq_entry();
        line = FormatSoftirqEntry(inner);
      } else if (event.has_softirq_exit()) {
        const auto& inner = event.softirq_exit();
        line = FormatSoftirqExit(inner);
      } else if (event.has_mm_vmscan_direct_reclaim_begin()) {
        const auto& inner = event.mm_vmscan_direct_reclaim_begin();
        line = FormatMmVmscanDirectReclaimBegin(inner);
      } else if (event.has_mm_vmscan_direct_reclaim_end()) {
        const auto& inner = event.mm_vmscan_direct_reclaim_end();
        line = FormatMmVmscanDirectReclaimEnd(inner);
      } else if (event.has_lowmemory_kill()) {
        const auto& inner = event.lowmemory_kill();
        line = FormatLowmemoryKill(inner);
      } else if (event.has_workqueue_execute_start()) {
        const auto& inner = event.workqueue_execute_start();
        line = FormatWorkqueueExecuteStart(inner);
      } else if (event.has_workqueue_execute_end()) {
        const auto& inner = event.workqueue_execute_end();
        line = FormatWorkqueueExecuteEnd(inner);
      } else if (event.has_workqueue_queue_work()) {
        const auto& inner = event.workqueue_queue_work();
        line = FormatWorkqueueQueueWork(inner);
      } else if (event.has_workqueue_activate_work()) {
        const auto& inner = event.workqueue_activate_work();
        line = FormatWorkqueueActivateWork(inner);
      } else if (event.has_mm_compaction_begin()) {
        const auto& inner = event.mm_compaction_begin();
        line = FormatMmCompactionBegin(inner);
      } else if (event.has_mm_compaction_deferred()) {
        const auto& inner = event.mm_compaction_deferred();
        line = FormatMmCompactionDeferred(inner);
      } else if (event.has_mm_compaction_defer_reset()) {
        const auto& inner = event.mm_compaction_defer_reset();
        line = FormatMmCompactionDeferReset(inner);
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
      } else if (event.has_suspend_resume()) {
        const auto& inner = event.suspend_resume();
        line = FormatSuspendResume(inner);
      } else if (event.has_sched_wakeup_new()) {
        const auto& inner = event.sched_wakeup_new();
        line = FormatSchedWakeupNew(inner);
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
