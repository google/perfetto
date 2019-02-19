// Copyright (C) 2018 The Android Open Source Project
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

import {
  AndroidLogConfig,
  AndroidLogId,
  AndroidPowerConfig,
  BufferConfig,
  DataSourceConfig,
  FtraceConfig,
  ProcessStatsConfig,
  SysStatsConfig,
  TraceConfig
} from '../common/protos';
import {MeminfoCounters, VmstatCounters} from '../common/protos';
import {RecordConfig} from '../common/state';

import {Controller} from './controller';
import {App} from './globals';

export function uint8ArrayToBase64(buffer: Uint8Array): string {
  return btoa(String.fromCharCode.apply(null, Array.from(buffer)));
}

export function genConfigProto(uiCfg: RecordConfig): Uint8Array {
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
  protoCfg.buffers[1].sizeKb = slowBufSizeKb;
  protoCfg.buffers[0].sizeKb = fastBufSizeKb;

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
  }

  const ftraceEvents = new Set<string>(uiCfg.ftrace ? uiCfg.ftraceEvents : []);
  const atraceCats = new Set<string>(uiCfg.atrace ? uiCfg.atraceCats : []);
  const atraceApps = new Set<string>();
  let procThreadAssociationPolling = false;
  let procThreadAssociationFtrace = false;
  let trackInitialOomScore = false;

  if (uiCfg.cpuSched || uiCfg.cpuLatency) {
    procThreadAssociationPolling = true;
    procThreadAssociationFtrace = true;
    ftraceEvents.add('sched/sched_switch');
    ftraceEvents.add('power/suspend_resume');
    if (uiCfg.cpuLatency) {
      ftraceEvents.add('sched/sched_wakeup');
      ftraceEvents.add('sched/sched_wakeup_new');
      ftraceEvents.add('power/suspend_resume');
    }
  }

  if (uiCfg.cpuFreq) {
    ftraceEvents.add('power/cpu_frequency');
    ftraceEvents.add('power/cpu_idle');
    ftraceEvents.add('power/suspend_resume');
  }

  if (procThreadAssociationFtrace) {
    ftraceEvents.add('sched/sched_process_exit');
    ftraceEvents.add('sched/sched_process_fork');
    ftraceEvents.add('sched/sched_process_free');
    ftraceEvents.add('task/task_rename');
  }

  if (uiCfg.batteryDrain) {
    const ds = new TraceConfig.DataSource();
    ds.config = new DataSourceConfig();
    ds.config.name = 'android.power';
    ds.config.androidPowerConfig = new AndroidPowerConfig();
    ds.config.androidPowerConfig.batteryPollMs = uiCfg.batteryDrainPollMs;
    ds.config.androidPowerConfig.batteryCounters = [
      AndroidPowerConfig.BatteryCounters.BATTERY_COUNTER_CAPACITY_PERCENT,
      AndroidPowerConfig.BatteryCounters.BATTERY_COUNTER_CHARGE,
      AndroidPowerConfig.BatteryCounters.BATTERY_COUNTER_CURRENT,
    ];
    protoCfg.dataSources.push(ds);
  }

  if (uiCfg.boardSensors) {
    ftraceEvents.add('regulator/regulator_set_voltage');
    ftraceEvents.add('regulator/regulator_set_voltage_complete');
    ftraceEvents.add('power/clock_enable');
    ftraceEvents.add('power/clock_disable');
    ftraceEvents.add('power/clock_set_rate');
    ftraceEvents.add('power/suspend_resume');
  }

  let sysStatsCfg: SysStatsConfig|undefined = undefined;

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
    ftraceEvents.add('kmem/rss_stat');
    ftraceEvents.add('kmem/mm_event');
    ftraceEvents.add('kmem/ion_heap_grow');
    ftraceEvents.add('kmem/ion_heap_shrink');
  }

  if (uiCfg.meminfo) {
    if (sysStatsCfg === undefined) sysStatsCfg = new SysStatsConfig();
    sysStatsCfg.meminfoPeriodMs = uiCfg.meminfoPeriodMs;
    sysStatsCfg.meminfoCounters = uiCfg.meminfoCounters.map(name => {
      // tslint:disable-next-line no-any
      return MeminfoCounters[name as any as number] as any as number;
    });
  }

  if (uiCfg.vmstat) {
    if (sysStatsCfg === undefined) sysStatsCfg = new SysStatsConfig();
    sysStatsCfg.vmstatPeriodMs = uiCfg.vmstatPeriodMs;
    sysStatsCfg.vmstatCounters = uiCfg.vmstatCounters.map(name => {
      // tslint:disable-next-line no-any
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
    protoCfg.dataSources.push(ds);
  }

  if (uiCfg.androidLogs) {
    const ds = new TraceConfig.DataSource();
    ds.config = new DataSourceConfig();
    ds.config.name = 'android.log';
    ds.config.androidLogConfig = new AndroidLogConfig();
    ds.config.androidLogConfig.logIds = uiCfg.androidLogBuffers.map(name => {
      // tslint:disable-next-line no-any
      return AndroidLogId[name as any as number] as any as number;
    });

    protoCfg.dataSources.push(ds);
  }

  // Keep these last. The stages above can enrich them.

  if (sysStatsCfg !== undefined) {
    const ds = new TraceConfig.DataSource();
    ds.config = new DataSourceConfig();
    ds.config.name = 'linux.sys_stats';
    ds.config.sysStatsConfig = sysStatsCfg;
    protoCfg.dataSources.push(ds);
  }

  if (uiCfg.ftrace || uiCfg.atraceApps.length > 0 || ftraceEvents.size > 0 ||
      atraceCats.size > 0 || atraceApps.size > 0) {
    const ds = new TraceConfig.DataSource();
    ds.config = new DataSourceConfig();
    ds.config.name = 'linux.ftrace';
    ds.config.ftraceConfig = new FtraceConfig();
    // Override the advanced ftrace parameters only if the user has ticked the
    // "Advanced ftrace config" tab.
    if (uiCfg.ftrace) {
      ds.config.ftraceConfig.bufferSizeKb = uiCfg.ftraceBufferSizeKb;
      ds.config.ftraceConfig.drainPeriodMs = uiCfg.ftraceDrainPeriodMs;
      for (const line of uiCfg.ftraceExtraEvents.split('\n')) {
        if (line.trim().length > 0) ftraceEvents.add(line.trim());
      }
    }
    for (const line of uiCfg.atraceApps.split('\n')) {
      if (line.trim().length > 0) atraceApps.add(line.trim());
    }

    if (atraceCats.size > 0 || atraceApps.size > 0) {
      ftraceEvents.add('ftrace/print');
    }

    ds.config.ftraceConfig.ftraceEvents = Array.from(ftraceEvents);
    ds.config.ftraceConfig.atraceCategories = Array.from(atraceCats);
    ds.config.ftraceConfig.atraceApps = Array.from(atraceApps);
    protoCfg.dataSources.push(ds);
  }

  const buffer = TraceConfig.encode(protoCfg).finish();
  return buffer;
}

export function toPbtxt(configBuffer: Uint8Array): string {
  const msg = TraceConfig.decode(configBuffer);
  const json = msg.toJSON();
  function snakeCase(s: string): string {
    return s.replace(/[A-Z]/g, c => '_' + c.toLowerCase());
  }
  // With the ahead of time compiled protos we can't seem to tell which
  // fields are enums.
  function looksLikeEnum(value: string): boolean {
    return value.startsWith('MEMINFO_') || value.startsWith('VMSTAT_') ||
        value.startsWith('STAT_') || value.startsWith('LID_') ||
        value.startsWith('BATTERY_COUNTER_') || value === 'DISCARD' ||
        value === 'RING_BUFFER';
  }
  function* message(msg: {}, indent: number): IterableIterator<string> {
    for (const [key, value] of Object.entries(msg)) {
      const isRepeated = Array.isArray(value);
      const isNested = typeof value === 'object' && !isRepeated;
      for (const entry of (isRepeated ? value as Array<{}> : [value])) {
        yield ' '.repeat(indent) + `${snakeCase(key)}${isNested ? '' : ':'} `;
        if (typeof entry === 'string') {
          yield looksLikeEnum(entry) ? entry : `"${entry}"`;
        } else if (typeof entry === 'number') {
          yield entry.toString();
        } else if (typeof entry === 'boolean') {
          yield entry.toString();
        } else {
          yield '{\n';
          yield* message(entry, indent + 4);
          yield ' '.repeat(indent) + '}';
        }
        yield '\n';
      }
    }
  }
  return [...message(json, 0)].join('');
}

export class RecordController extends Controller<'main'> {
  private app: App;
  private config: RecordConfig|null = null;

  constructor(args: {app: App}) {
    super('main');
    this.app = args.app;
  }

  run() {
    if (this.app.state.recordConfig === this.config) return;
    this.config = this.app.state.recordConfig;
    const configProto = genConfigProto(this.config);
    const configProtoText = toPbtxt(configProto);
    const commandline = `
      echo '${uint8ArrayToBase64(configProto)}' |
      base64 --decode |
      adb shell "perfetto -c - -o /data/misc/perfetto-traces/trace" &&
      adb pull /data/misc/perfetto-traces/trace /tmp/trace
    `;
    // TODO(hjd): This should not be TrackData after we unify the stores.
    this.app.publish('TrackData', {
      id: 'config',
      data: {
        commandline,
        pbtxt: configProtoText,
      }
    });
  }
}
