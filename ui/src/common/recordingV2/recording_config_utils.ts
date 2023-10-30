// Copyright (C) 2022 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.


import {isString} from '../../base/object_utils';
import {base64Encode} from '../../base/string_utils';
import {RecordConfig} from '../../controller/record_config_types';
import {
  AndroidLogConfig,
  AndroidLogId,
  AndroidPowerConfig,
  BufferConfig,
  ChromeConfig,
  DataSourceConfig,
  FtraceConfig,
  HeapprofdConfig,
  JavaContinuousDumpConfig,
  JavaHprofConfig,
  MeminfoCounters,
  NativeContinuousDumpConfig,
  NetworkPacketTraceConfig,
  PerfEventConfig,
  PerfEvents,
  ProcessStatsConfig,
  SysStatsConfig,
  TraceConfig,
  TrackEventConfig,
  VmstatCounters,
} from '../../protos';

import {TargetInfo} from './recording_interfaces_v2';

import PerfClock = PerfEvents.PerfClock;
import Timebase = PerfEvents.Timebase;
import CallstackSampling = PerfEventConfig.CallstackSampling;
import Scope = PerfEventConfig.Scope;

export interface ConfigProtoEncoded {
  configProtoText?: string;
  configProtoBase64?: string;
  hasDataSources: boolean;
}

export class RecordingConfigUtils {
  private lastConfig?: RecordConfig;
  private lastTargetInfo?: TargetInfo;
  private configProtoText?: string;
  private configProtoBase64?: string;
  private hasDataSources: boolean = false;

  fetchLatestRecordCommand(recordConfig: RecordConfig, targetInfo: TargetInfo):
      ConfigProtoEncoded {
    if (recordConfig === this.lastConfig &&
        targetInfo === this.lastTargetInfo) {
      return {
        configProtoText: this.configProtoText,
        configProtoBase64: this.configProtoBase64,
        hasDataSources: this.hasDataSources,
      };
    }
    this.lastConfig = recordConfig;
    this.lastTargetInfo = targetInfo;

    const traceConfig = genTraceConfig(recordConfig, targetInfo);
    const configProto = TraceConfig.encode(traceConfig).finish();
    this.configProtoText = toPbtxt(configProto);
    this.configProtoBase64 = base64Encode(configProto);
    this.hasDataSources = traceConfig.dataSources.length > 0;
    return {
      configProtoText: this.configProtoText,
      configProtoBase64: this.configProtoBase64,
      hasDataSources: this.hasDataSources,
    };
  }
}

function enableSchedBlockedReason(androidApiLevel?: number): boolean {
  return androidApiLevel !== undefined && androidApiLevel >= 31;
}

function enableCompactSched(androidApiLevel?: number): boolean {
  return androidApiLevel !== undefined && androidApiLevel >= 31;
}

export function genTraceConfig(
    uiCfg: RecordConfig, targetInfo: TargetInfo): TraceConfig {
  const isAndroid = targetInfo.targetType === 'ANDROID';
  const androidApiLevel = isAndroid ? targetInfo.androidApiLevel : undefined;
  const protoCfg = new TraceConfig();
  protoCfg.durationMs = uiCfg.durationMs;

  // Auxiliary buffer for slow-rate events.
  // Set to 1/8th of the main buffer size, with reasonable limits.
  let slowBufSizeKb = uiCfg.bufferSizeMb * (1024 / 8);
  slowBufSizeKb = Math.min(slowBufSizeKb, 2 * 1024);
  slowBufSizeKb = Math.max(slowBufSizeKb, 256);

  // Main buffer for ftrace and other high-freq events.
  const fastBufSizeKb = uiCfg.bufferSizeMb * 1024 - slowBufSizeKb;

  protoCfg.buffers.push(new BufferConfig());
  protoCfg.buffers.push(new BufferConfig());
  protoCfg.buffers[0].sizeKb = fastBufSizeKb;
  protoCfg.buffers[1].sizeKb = slowBufSizeKb;

  if (uiCfg.mode === 'STOP_WHEN_FULL') {
    protoCfg.buffers[0].fillPolicy = BufferConfig.FillPolicy.DISCARD;
    protoCfg.buffers[1].fillPolicy = BufferConfig.FillPolicy.DISCARD;
  } else {
    protoCfg.buffers[0].fillPolicy = BufferConfig.FillPolicy.RING_BUFFER;
    protoCfg.buffers[1].fillPolicy = BufferConfig.FillPolicy.RING_BUFFER;
    protoCfg.flushPeriodMs = 30000;
    if (uiCfg.mode === 'LONG_TRACE') {
      protoCfg.writeIntoFile = true;
      protoCfg.fileWritePeriodMs = uiCfg.fileWritePeriodMs;
      protoCfg.maxFileSizeBytes = uiCfg.maxFileSizeMb * 1e6;
    }

    // Clear incremental state every 5 seconds when tracing into a ring
    // buffer.
    const incStateConfig = new TraceConfig.IncrementalStateConfig();
    incStateConfig.clearPeriodMs = 5000;
    protoCfg.incrementalStateConfig = incStateConfig;
  }

  const ftraceEvents = new Set<string>(uiCfg.ftrace ? uiCfg.ftraceEvents : []);
  const atraceCats = new Set<string>(uiCfg.atrace ? uiCfg.atraceCats : []);
  const atraceApps = new Set<string>();
  const chromeCategories = new Set<string>();
  uiCfg.chromeCategoriesSelected.forEach((it) => chromeCategories.add(it));
  uiCfg.chromeHighOverheadCategoriesSelected.forEach(
      (it) => chromeCategories.add(it));

  let procThreadAssociationPolling = false;
  let procThreadAssociationFtrace = false;
  let trackInitialOomScore = false;

  if (isAndroid) {
    const ds = new TraceConfig.DataSource();
    ds.config = new DataSourceConfig();
    ds.config.targetBuffer = 1;
    ds.config.name = 'android.packages_list';
    protoCfg.dataSources.push(ds);
  }

  if (uiCfg.cpuSched) {
    procThreadAssociationPolling = true;
    procThreadAssociationFtrace = true;
    uiCfg.ftrace = true;
    if (enableSchedBlockedReason(androidApiLevel)) {
      uiCfg.symbolizeKsyms = true;
    }
    ftraceEvents.add('sched/sched_switch');
    ftraceEvents.add('power/suspend_resume');
    ftraceEvents.add('sched/sched_wakeup');
    ftraceEvents.add('sched/sched_wakeup_new');
    ftraceEvents.add('sched/sched_waking');
    ftraceEvents.add('power/suspend_resume');
  }

  let sysStatsCfg: SysStatsConfig|undefined = undefined;

  if (uiCfg.cpuFreq) {
    ftraceEvents.add('power/cpu_frequency');
    ftraceEvents.add('power/cpu_idle');
    ftraceEvents.add('power/suspend_resume');

    sysStatsCfg = new SysStatsConfig();
    sysStatsCfg.cpufreqPeriodMs = uiCfg.cpuFreqPollMs;
  }

  if (uiCfg.gpuFreq) {
    ftraceEvents.add('power/gpu_frequency');
  }

  if (uiCfg.gpuMemTotal) {
    ftraceEvents.add('gpu_mem/gpu_mem_total');

    if (targetInfo.targetType !== 'CHROME') {
      const ds = new TraceConfig.DataSource();
      ds.config = new DataSourceConfig();
      ds.config.name = 'android.gpu.memory';
      protoCfg.dataSources.push(ds);
    }
  }

  if (uiCfg.cpuSyscall) {
    ftraceEvents.add('raw_syscalls/sys_enter');
    ftraceEvents.add('raw_syscalls/sys_exit');
  }

  if (uiCfg.batteryDrain) {
    const ds = new TraceConfig.DataSource();
    ds.config = new DataSourceConfig();
    if (targetInfo.targetType === 'CHROME_OS' ||
        targetInfo.targetType === 'LINUX') {
      ds.config.name = 'linux.sysfs_power';
    } else {
      ds.config.name = 'android.power';
      ds.config.androidPowerConfig = new AndroidPowerConfig();
      ds.config.androidPowerConfig.batteryPollMs = uiCfg.batteryDrainPollMs;
      ds.config.androidPowerConfig.batteryCounters = [
        AndroidPowerConfig.BatteryCounters.BATTERY_COUNTER_CAPACITY_PERCENT,
        AndroidPowerConfig.BatteryCounters.BATTERY_COUNTER_CHARGE,
        AndroidPowerConfig.BatteryCounters.BATTERY_COUNTER_CURRENT,
      ];
      ds.config.androidPowerConfig.collectPowerRails = true;
    }
    if (targetInfo.targetType !== 'CHROME') {
      protoCfg.dataSources.push(ds);
    }
  }

  if (uiCfg.boardSensors) {
    ftraceEvents.add('regulator/regulator_set_voltage');
    ftraceEvents.add('regulator/regulator_set_voltage_complete');
    ftraceEvents.add('power/clock_enable');
    ftraceEvents.add('power/clock_disable');
    ftraceEvents.add('power/clock_set_rate');
    ftraceEvents.add('power/suspend_resume');
  }

  if (uiCfg.cpuCoarse) {
    if (sysStatsCfg === undefined) sysStatsCfg = new SysStatsConfig();
    sysStatsCfg.statPeriodMs = uiCfg.cpuCoarsePollMs;
    sysStatsCfg.statCounters = [
      SysStatsConfig.StatCounters.STAT_CPU_TIMES,
      SysStatsConfig.StatCounters.STAT_FORK_COUNT,
    ];
  }

  if (uiCfg.memHiFreq) {
    procThreadAssociationPolling = true;
    procThreadAssociationFtrace = true;
    ftraceEvents.add('mm_event/mm_event_record');
    ftraceEvents.add('kmem/rss_stat');
    ftraceEvents.add('ion/ion_stat');
    ftraceEvents.add('dmabuf_heap/dma_heap_stat');
    ftraceEvents.add('kmem/ion_heap_grow');
    ftraceEvents.add('kmem/ion_heap_shrink');
  }

  if (procThreadAssociationFtrace) {
    ftraceEvents.add('sched/sched_process_exit');
    ftraceEvents.add('sched/sched_process_free');
    ftraceEvents.add('task/task_newtask');
    ftraceEvents.add('task/task_rename');
  }

  if (uiCfg.meminfo) {
    if (sysStatsCfg === undefined) sysStatsCfg = new SysStatsConfig();
    sysStatsCfg.meminfoPeriodMs = uiCfg.meminfoPeriodMs;
    sysStatsCfg.meminfoCounters = uiCfg.meminfoCounters.map((name) => {
      return MeminfoCounters[name as any as number] as any as number;
    });
  }

  if (uiCfg.vmstat) {
    if (sysStatsCfg === undefined) sysStatsCfg = new SysStatsConfig();
    sysStatsCfg.vmstatPeriodMs = uiCfg.vmstatPeriodMs;
    sysStatsCfg.vmstatCounters = uiCfg.vmstatCounters.map((name) => {
      return VmstatCounters[name as any as number] as any as number;
    });
  }

  if (uiCfg.memLmk) {
    // For in-kernel LMK (roughly older devices until Go and Pixel 3).
    ftraceEvents.add('lowmemorykiller/lowmemory_kill');

    // For userspace LMKd (newer devices).
    // 'lmkd' is not really required because the code in lmkd.c emits events
    // with ATRACE_TAG_ALWAYS. We need something just to ensure that the final
    // config will enable atrace userspace events.
    atraceApps.add('lmkd');

    ftraceEvents.add('oom/oom_score_adj_update');
    procThreadAssociationPolling = true;
    trackInitialOomScore = true;
  }

  let heapprofd: HeapprofdConfig|undefined = undefined;
  if (uiCfg.heapProfiling) {
    // TODO(hjd): Check or inform user if buffer size are too small.
    const cfg = new HeapprofdConfig();
    cfg.samplingIntervalBytes = uiCfg.hpSamplingIntervalBytes;
    if (uiCfg.hpSharedMemoryBuffer >= 8192 &&
        uiCfg.hpSharedMemoryBuffer % 4096 === 0) {
      cfg.shmemSizeBytes = uiCfg.hpSharedMemoryBuffer;
    }
    for (const value of uiCfg.hpProcesses.split('\n')) {
      if (value === '') {
        // Ignore empty lines
      } else if (isNaN(+value)) {
        cfg.processCmdline.push(value);
      } else {
        cfg.pid.push(+value);
      }
    }
    if (uiCfg.hpContinuousDumpsInterval > 0) {
      const cdc = cfg.continuousDumpConfig = new NativeContinuousDumpConfig();
      cdc.dumpIntervalMs = uiCfg.hpContinuousDumpsInterval;
      if (uiCfg.hpContinuousDumpsPhase > 0) {
        cdc.dumpPhaseMs = uiCfg.hpContinuousDumpsPhase;
      }
    }
    cfg.blockClient = uiCfg.hpBlockClient;
    if (uiCfg.hpAllHeaps) {
      cfg.allHeaps = true;
    }
    heapprofd = cfg;
  }

  let javaHprof: JavaHprofConfig|undefined = undefined;
  if (uiCfg.javaHeapDump) {
    const cfg = new JavaHprofConfig();
    for (const value of uiCfg.jpProcesses.split('\n')) {
      if (value === '') {
        // Ignore empty lines
      } else if (isNaN(+value)) {
        cfg.processCmdline.push(value);
      } else {
        cfg.pid.push(+value);
      }
    }
    if (uiCfg.jpContinuousDumpsInterval > 0) {
      const cdc = cfg.continuousDumpConfig = new JavaContinuousDumpConfig();
      cdc.dumpIntervalMs = uiCfg.jpContinuousDumpsInterval;
      if (uiCfg.hpContinuousDumpsPhase > 0) {
        cdc.dumpPhaseMs = uiCfg.jpContinuousDumpsPhase;
      }
    }
    javaHprof = cfg;
  }

  if (uiCfg.procStats || procThreadAssociationPolling || trackInitialOomScore) {
    const ds = new TraceConfig.DataSource();
    ds.config = new DataSourceConfig();
    ds.config.targetBuffer = 1;  // Aux
    ds.config.name = 'linux.process_stats';
    ds.config.processStatsConfig = new ProcessStatsConfig();
    if (uiCfg.procStats) {
      ds.config.processStatsConfig.procStatsPollMs = uiCfg.procStatsPeriodMs;
    }
    if (procThreadAssociationPolling || trackInitialOomScore) {
      ds.config.processStatsConfig.scanAllProcessesOnStart = true;
    }
    if (targetInfo.targetType !== 'CHROME') {
      protoCfg.dataSources.push(ds);
    }
  }

  if (uiCfg.androidLogs) {
    const ds = new TraceConfig.DataSource();
    ds.config = new DataSourceConfig();
    ds.config.name = 'android.log';
    ds.config.androidLogConfig = new AndroidLogConfig();
    ds.config.androidLogConfig.logIds = uiCfg.androidLogBuffers.map((name) => {
      return AndroidLogId[name as any as number] as any as number;
    });

    if (targetInfo.targetType !== 'CHROME') {
      protoCfg.dataSources.push(ds);
    }
  }

  if (uiCfg.androidFrameTimeline) {
    const ds = new TraceConfig.DataSource();
    ds.config = new DataSourceConfig();
    ds.config.name = 'android.surfaceflinger.frametimeline';
    if (targetInfo.targetType !== 'CHROME') {
      protoCfg.dataSources.push(ds);
    }
  }

  if (uiCfg.androidGameInterventionList) {
    const ds = new TraceConfig.DataSource();
    ds.config = new DataSourceConfig();
    ds.config.name = 'android.game_interventions';
    if (targetInfo.targetType !== 'CHROME') {
      protoCfg.dataSources.push(ds);
    }
  }

  if (uiCfg.androidNetworkTracing) {
    if (targetInfo.targetType !== 'CHROME') {
      const net = new TraceConfig.DataSource();
      net.config = new DataSourceConfig();
      net.config.name = 'android.network_packets';
      net.config.networkPacketTraceConfig = new NetworkPacketTraceConfig();
      net.config.networkPacketTraceConfig.pollMs =
          uiCfg.androidNetworkTracingPollMs;
      protoCfg.dataSources.push(net);

      // Record package info so that Perfetto can display the package name for
      // network packet events based on the event uid.
      const pkg = new TraceConfig.DataSource();
      pkg.config = new DataSourceConfig();
      pkg.config.name = 'android.packages_list';
      protoCfg.dataSources.push(pkg);
    }
  }

  if (uiCfg.chromeLogs) {
    chromeCategories.add('log');
  }

  if (uiCfg.taskScheduling) {
    chromeCategories.add('toplevel');
    chromeCategories.add('toplevel.flow');
    chromeCategories.add('scheduler');
    chromeCategories.add('sequence_manager');
    chromeCategories.add('disabled-by-default-toplevel.flow');
  }

  if (uiCfg.ipcFlows) {
    chromeCategories.add('toplevel');
    chromeCategories.add('toplevel.flow');
    chromeCategories.add('disabled-by-default-ipc.flow');
    chromeCategories.add('mojom');
  }

  if (uiCfg.jsExecution) {
    chromeCategories.add('toplevel');
    chromeCategories.add('v8');
  }

  if (uiCfg.webContentRendering) {
    chromeCategories.add('toplevel');
    chromeCategories.add('blink');
    chromeCategories.add('cc');
    chromeCategories.add('gpu');
  }

  if (uiCfg.uiRendering) {
    chromeCategories.add('toplevel');
    chromeCategories.add('cc');
    chromeCategories.add('gpu');
    chromeCategories.add('viz');
    chromeCategories.add('ui');
    chromeCategories.add('views');
  }

  if (uiCfg.inputEvents) {
    chromeCategories.add('toplevel');
    chromeCategories.add('benchmark');
    chromeCategories.add('evdev');
    chromeCategories.add('input');
    chromeCategories.add('disabled-by-default-toplevel.flow');
  }

  if (uiCfg.navigationAndLoading) {
    chromeCategories.add('loading');
    chromeCategories.add('net');
    chromeCategories.add('netlog');
    chromeCategories.add('navigation');
    chromeCategories.add('browser');
  }

  // linux.perf stack sampling
  if (uiCfg.tracePerf) {
    const ds = new TraceConfig.DataSource();
    ds.config = new DataSourceConfig();
    ds.config.name = 'linux.perf';

    const perfEventConfig = new PerfEventConfig();
    perfEventConfig.timebase = new Timebase();
    perfEventConfig.timebase.frequency = uiCfg.timebaseFrequency;
    // TODO: The timestampClock needs to be changed to MONOTONIC once we start
    // offering a choice of counter to record on through the recording UI, as
    // not all clocks are compatible with hardware counters).
    perfEventConfig.timebase.timestampClock = PerfClock.PERF_CLOCK_BOOTTIME;

    const callstackSampling = new CallstackSampling();
    if (uiCfg.targetCmdLine.length > 0) {
      const scope = new Scope();
      for (const cmdLine of uiCfg.targetCmdLine) {
        if (cmdLine == '') {
          continue;
        }
        scope.targetCmdline?.push(cmdLine.trim());
      }
      callstackSampling.scope = scope;
    }

    perfEventConfig.callstackSampling = callstackSampling;

    ds.config.perfEventConfig = perfEventConfig;
    protoCfg.dataSources.push(ds);
  }

  if (chromeCategories.size !== 0) {
    let chromeRecordMode;
    if (uiCfg.mode === 'STOP_WHEN_FULL') {
      chromeRecordMode = 'record-until-full';
    } else {
      chromeRecordMode = 'record-continuously';
    }
    const configStruct = {
      record_mode: chromeRecordMode,
      included_categories: [...chromeCategories.values()],
      // Only include explicitly selected categories
      excluded_categories: ['*'],
      memory_dump_config: {},
    };
    if (chromeCategories.has('disabled-by-default-memory-infra')) {
      configStruct.memory_dump_config = {
        allowed_dump_modes: ['background', 'light', 'detailed'],
        triggers: [{
          min_time_between_dumps_ms: 10000,
          mode: 'detailed',
          type: 'periodic_interval',
        }],
      };
    }
    const chromeConfig = new ChromeConfig();
    chromeConfig.clientPriority = ChromeConfig.ClientPriority.USER_INITIATED;
    chromeConfig.privacyFilteringEnabled = uiCfg.chromePrivacyFiltering;
    chromeConfig.traceConfig = JSON.stringify(configStruct);

    const traceDs = new TraceConfig.DataSource();
    traceDs.config = new DataSourceConfig();
    traceDs.config.name = 'org.chromium.trace_event';
    traceDs.config.chromeConfig = chromeConfig;
    protoCfg.dataSources.push(traceDs);

    // Configure "track_event" datasource for the Chrome SDK build.
    const trackEventDs = new TraceConfig.DataSource();
    trackEventDs.config = new DataSourceConfig();
    trackEventDs.config.name = 'track_event';
    trackEventDs.config.chromeConfig = chromeConfig;
    trackEventDs.config.trackEventConfig = new TrackEventConfig();
    trackEventDs.config.trackEventConfig.disabledCategories = ['*'];
    trackEventDs.config.trackEventConfig.enabledCategories =
        [...chromeCategories.values(), '__metadata'];
    trackEventDs.config.trackEventConfig.enableThreadTimeSampling = true;
    trackEventDs.config.trackEventConfig.timestampUnitMultiplier = 1000;
    trackEventDs.config.trackEventConfig.filterDynamicEventNames =
        uiCfg.chromePrivacyFiltering;
    trackEventDs.config.trackEventConfig.filterDebugAnnotations =
        uiCfg.chromePrivacyFiltering;
    protoCfg.dataSources.push(trackEventDs);

    const metadataDs = new TraceConfig.DataSource();
    metadataDs.config = new DataSourceConfig();
    metadataDs.config.name = 'org.chromium.trace_metadata';
    metadataDs.config.chromeConfig = chromeConfig;
    protoCfg.dataSources.push(metadataDs);

    if (chromeCategories.has('disabled-by-default-memory-infra')) {
      const memoryDs = new TraceConfig.DataSource();
      memoryDs.config = new DataSourceConfig();
      memoryDs.config.name = 'org.chromium.memory_instrumentation';
      memoryDs.config.chromeConfig = chromeConfig;
      protoCfg.dataSources.push(memoryDs);

      const HeapProfDs = new TraceConfig.DataSource();
      HeapProfDs.config = new DataSourceConfig();
      HeapProfDs.config.name = 'org.chromium.native_heap_profiler';
      HeapProfDs.config.chromeConfig = chromeConfig;
      protoCfg.dataSources.push(HeapProfDs);
    }

    if (chromeCategories.has('disabled-by-default-cpu_profiler') ||
        chromeCategories.has('disabled-by-default-cpu_profiler.debug')) {
      const dataSource = new TraceConfig.DataSource();
      dataSource.config = new DataSourceConfig();
      dataSource.config.name = 'org.chromium.sampler_profiler';
      dataSource.config.chromeConfig = chromeConfig;
      protoCfg.dataSources.push(dataSource);
    }
  }

  // Keep these last. The stages above can enrich them.

  if (sysStatsCfg !== undefined && targetInfo.targetType !== 'CHROME') {
    const ds = new TraceConfig.DataSource();
    ds.config = new DataSourceConfig();
    ds.config.name = 'linux.sys_stats';
    ds.config.sysStatsConfig = sysStatsCfg;
    protoCfg.dataSources.push(ds);
  }

  if (heapprofd !== undefined && targetInfo.targetType !== 'CHROME') {
    const ds = new TraceConfig.DataSource();
    ds.config = new DataSourceConfig();
    ds.config.targetBuffer = 0;
    ds.config.name = 'android.heapprofd';
    ds.config.heapprofdConfig = heapprofd;
    protoCfg.dataSources.push(ds);
  }

  if (javaHprof !== undefined && targetInfo.targetType !== 'CHROME') {
    const ds = new TraceConfig.DataSource();
    ds.config = new DataSourceConfig();
    ds.config.targetBuffer = 0;
    ds.config.name = 'android.java_hprof';
    ds.config.javaHprofConfig = javaHprof;
    protoCfg.dataSources.push(ds);
  }

  if (uiCfg.ftrace || uiCfg.atrace || ftraceEvents.size > 0 ||
      atraceCats.size > 0 || atraceApps.size > 0) {
    const ds = new TraceConfig.DataSource();
    ds.config = new DataSourceConfig();
    ds.config.name = 'linux.ftrace';
    ds.config.ftraceConfig = new FtraceConfig();
    // Override the advanced ftrace parameters only if the user has ticked the
    // "Advanced ftrace config" tab.
    if (uiCfg.ftrace) {
      if (uiCfg.ftraceBufferSizeKb) {
        ds.config.ftraceConfig.bufferSizeKb = uiCfg.ftraceBufferSizeKb;
      }
      if (uiCfg.ftraceDrainPeriodMs) {
        ds.config.ftraceConfig.drainPeriodMs = uiCfg.ftraceDrainPeriodMs;
      }
      if (uiCfg.symbolizeKsyms) {
        ds.config.ftraceConfig.symbolizeKsyms = true;
        ftraceEvents.add('sched/sched_blocked_reason');
      }
      for (const line of uiCfg.ftraceExtraEvents.split('\n')) {
        if (line.trim().length > 0) ftraceEvents.add(line.trim());
      }
    }

    if (uiCfg.atrace) {
      if (uiCfg.allAtraceApps) {
        atraceApps.clear();
        atraceApps.add('*');
      } else {
        for (const line of uiCfg.atraceApps.split('\n')) {
          if (line.trim().length > 0) atraceApps.add(line.trim());
        }
      }
    }

    if (atraceCats.size > 0 || atraceApps.size > 0) {
      ftraceEvents.add('ftrace/print');
    }

    let ftraceEventsArray: string[] = [];
    if (androidApiLevel && androidApiLevel === 28) {
      for (const ftraceEvent of ftraceEvents) {
        // On P, we don't support groups so strip all group names from ftrace
        // events.
        const groupAndName = ftraceEvent.split('/');
        if (groupAndName.length !== 2) {
          ftraceEventsArray.push(ftraceEvent);
          continue;
        }
        // Filter out any wildcard event groups which was not supported
        // before Q.
        if (groupAndName[1] === '*') {
          continue;
        }
        ftraceEventsArray.push(groupAndName[1]);
      }
    } else {
      ftraceEventsArray = Array.from(ftraceEvents);
    }

    ds.config.ftraceConfig.ftraceEvents = ftraceEventsArray;
    ds.config.ftraceConfig.atraceCategories = Array.from(atraceCats);
    ds.config.ftraceConfig.atraceApps = Array.from(atraceApps);

    if (enableCompactSched(androidApiLevel)) {
      const compact = new FtraceConfig.CompactSchedConfig();
      compact.enabled = true;
      ds.config.ftraceConfig.compactSched = compact;
    }

    if (targetInfo.targetType !== 'CHROME') {
      protoCfg.dataSources.push(ds);
    }
  }

  return protoCfg;
}

function toPbtxt(configBuffer: Uint8Array): string {
  const msg = TraceConfig.decode(configBuffer);
  const json = msg.toJSON();
  function snakeCase(s: string): string {
    return s.replace(/[A-Z]/g, (c) => '_' + c.toLowerCase());
  }
  // With the ahead of time compiled protos we can't seem to tell which
  // fields are enums.
  function isEnum(value: string): boolean {
    return value.startsWith('MEMINFO_') || value.startsWith('VMSTAT_') ||
        value.startsWith('STAT_') || value.startsWith('LID_') ||
        value.startsWith('BATTERY_COUNTER_') || value === 'DISCARD' ||
        value === 'RING_BUFFER' || value.startsWith('PERF_CLOCK_');
  }
  // Since javascript doesn't have 64 bit numbers when converting protos to
  // json the proto library encodes them as strings. This is lossy since
  // we can't tell which strings that look like numbers are actually strings
  // and which are actually numbers. Ideally we would reflect on the proto
  // definition somehow but for now we just hard code keys which have this
  // problem in the config.
  function is64BitNumber(key: string): boolean {
    return [
      'maxFileSizeBytes',
      'samplingIntervalBytes',
      'shmemSizeBytes',
      'pid',
      'frequency',
    ].includes(key);
  }
  function* message(msg: {}, indent: number): IterableIterator<string> {
    for (const [key, value] of Object.entries(msg)) {
      const isRepeated = Array.isArray(value);
      const isNested = typeof value === 'object' && !isRepeated;
      for (const entry of (isRepeated ? value as Array<{}>: [value])) {
        yield ' '.repeat(indent) + `${snakeCase(key)}${isNested ? '' : ':'} `;
        if (isString(entry)) {
          if (isEnum(entry) || is64BitNumber(key)) {
            yield entry;
          } else {
            yield `"${entry.replace(new RegExp('"', 'g'), '\\"')}"`;
          }
        } else if (typeof entry === 'number') {
          yield entry.toString();
        } else if (typeof entry === 'boolean') {
          yield entry.toString();
        } else if (typeof entry === 'object' && entry !== null) {
          yield '{\n';
          yield* message(entry, indent + 4);
          yield ' '.repeat(indent) + '}';
        } else {
          throw new Error(`Record proto entry "${entry}" with unexpected type ${
              typeof entry}`);
        }
        yield '\n';
      }
    }
  }
  return [...message(json, 0)].join('');
}
