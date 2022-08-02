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


import {produce} from 'immer';
import * as m from 'mithril';

import {Actions} from '../common/actions';
import {featureFlags} from '../common/feature_flags';
import {MeminfoCounters, VmstatCounters} from '../common/protos';
import {
  AdbRecordingTarget,
  getBuiltinChromeCategoryList,
  getDefaultRecordingTargets,
  hasActiveProbes,
  isAdbTarget,
  isAndroidP,
  isAndroidTarget,
  isChromeTarget,
  isCrOSTarget,
  isLinuxTarget,
  LoadedConfig,
  MAX_TIME,
  RecordingTarget,
  RecordMode,
} from '../common/state';
import {AdbOverWebUsb} from '../controller/adb';
import {
  createEmptyRecordConfig,
  RecordConfig,
} from '../controller/record_config_types';

import {globals} from './globals';
import {createPage, PageAttrs} from './pages';
import {
  autosaveConfigStore,
  recordConfigStore,
  recordTargetStore,
} from './record_config';
import {
  CategoriesCheckboxList,
  CodeSnippet,
  CompactProbe,
  Dropdown,
  DropdownAttrs,
  Probe,
  ProbeAttrs,
  Slider,
  SliderAttrs,
  Textarea,
  TextareaAttrs,
  Toggle,
  ToggleAttrs,
} from './record_widgets';

export const PERSIST_CONFIG_FLAG = featureFlags.register({
  id: 'persistConfigsUI',
  name: 'Config persistence UI',
  description: 'Show experimental config persistence UI on the record page.',
  defaultValue: true,
});

export const POLL_INTERVAL_MS = [250, 500, 1000, 2500, 5000, 30000, 60000];

export const ATRACE_CATEGORIES = new Map<string, string>();
ATRACE_CATEGORIES.set('adb', 'ADB');
ATRACE_CATEGORIES.set('aidl', 'AIDL calls');
ATRACE_CATEGORIES.set('am', 'Activity Manager');
ATRACE_CATEGORIES.set('audio', 'Audio');
ATRACE_CATEGORIES.set('binder_driver', 'Binder Kernel driver');
ATRACE_CATEGORIES.set('binder_lock', 'Binder global lock trace');
ATRACE_CATEGORIES.set('bionic', 'Bionic C library');
ATRACE_CATEGORIES.set('camera', 'Camera');
ATRACE_CATEGORIES.set('dalvik', 'ART & Dalvik');
ATRACE_CATEGORIES.set('database', 'Database');
ATRACE_CATEGORIES.set('gfx', 'Graphics');
ATRACE_CATEGORIES.set('hal', 'Hardware Modules');
ATRACE_CATEGORIES.set('input', 'Input');
ATRACE_CATEGORIES.set('network', 'Network');
ATRACE_CATEGORIES.set('nnapi', 'Neural Network API');
ATRACE_CATEGORIES.set('pm', 'Package Manager');
ATRACE_CATEGORIES.set('power', 'Power Management');
ATRACE_CATEGORIES.set('res', 'Resource Loading');
ATRACE_CATEGORIES.set('rro', 'Resource Overlay');
ATRACE_CATEGORIES.set('rs', 'RenderScript');
ATRACE_CATEGORIES.set('sm', 'Sync Manager');
ATRACE_CATEGORIES.set('ss', 'System Server');
ATRACE_CATEGORIES.set('vibrator', 'Vibrator');
ATRACE_CATEGORIES.set('video', 'Video');
ATRACE_CATEGORIES.set('view', 'View System');
ATRACE_CATEGORIES.set('webview', 'WebView');
ATRACE_CATEGORIES.set('wm', 'Window Manager');

export const LOG_BUFFERS = new Map<string, string>();
LOG_BUFFERS.set('LID_CRASH', 'Crash');
LOG_BUFFERS.set('LID_DEFAULT', 'Main');
LOG_BUFFERS.set('LID_EVENTS', 'Binary events');
LOG_BUFFERS.set('LID_KERNEL', 'Kernel');
LOG_BUFFERS.set('LID_RADIO', 'Radio');
LOG_BUFFERS.set('LID_SECURITY', 'Security');
LOG_BUFFERS.set('LID_STATS', 'Stats');
LOG_BUFFERS.set('LID_SYSTEM', 'System');

export const FTRACE_CATEGORIES = new Map<string, string>();
FTRACE_CATEGORIES.set('binder/*', 'binder');
FTRACE_CATEGORIES.set('block/*', 'block');
FTRACE_CATEGORIES.set('clk/*', 'clk');
FTRACE_CATEGORIES.set('ext4/*', 'ext4');
FTRACE_CATEGORIES.set('f2fs/*', 'f2fs');
FTRACE_CATEGORIES.set('i2c/*', 'i2c');
FTRACE_CATEGORIES.set('irq/*', 'irq');
FTRACE_CATEGORIES.set('kmem/*', 'kmem');
FTRACE_CATEGORIES.set('memory_bus/*', 'memory_bus');
FTRACE_CATEGORIES.set('mmc/*', 'mmc');
FTRACE_CATEGORIES.set('oom/*', 'oom');
FTRACE_CATEGORIES.set('power/*', 'power');
FTRACE_CATEGORIES.set('regulator/*', 'regulator');
FTRACE_CATEGORIES.set('sched/*', 'sched');
FTRACE_CATEGORIES.set('sync/*', 'sync');
FTRACE_CATEGORIES.set('task/*', 'task');
FTRACE_CATEGORIES.set('task/*', 'task');
FTRACE_CATEGORIES.set('vmscan/*', 'vmscan');
FTRACE_CATEGORIES.set('fastrpc/*', 'fastrpc');

export function RecSettings(cssClass: string) {
  const S = (x: number) => x * 1000;
  const M = (x: number) => x * 1000 * 60;
  const H = (x: number) => x * 1000 * 60 * 60;

  const cfg = globals.state.recordConfig;

  const recButton = (mode: RecordMode, title: string, img: string) => {
    const checkboxArgs = {
      checked: cfg.mode === mode,
      onchange: (e: InputEvent) => {
        const checked = (e.target as HTMLInputElement).checked;
        if (!checked) return;
        const traceCfg = produce(globals.state.recordConfig, (draft) => {
          draft.mode = mode;
        });
        globals.dispatch(Actions.setRecordConfig({config: traceCfg}));
      },
    };
    return m(
        `label${cfg.mode === mode ? '.selected' : ''}`,
        m(`input[type=radio][name=rec_mode]`, checkboxArgs),
        m(`img[src=${globals.root}assets/${img}]`),
        m('span', title));
  };

  return m(
      `.record-section${cssClass}`,
      m('header', 'Recording mode'),
      m('.record-mode',
        recButton('STOP_WHEN_FULL', 'Stop when full', 'rec_one_shot.png'),
        recButton('RING_BUFFER', 'Ring buffer', 'rec_ring_buf.png'),
        recButton('LONG_TRACE', 'Long trace', 'rec_long_trace.png')),

      m(Slider, {
        title: 'In-memory buffer size',
        icon: '360',
        values: [4, 8, 16, 32, 64, 128, 256, 512],
        unit: 'MB',
        set: (cfg, val) => cfg.bufferSizeMb = val,
        get: (cfg) => cfg.bufferSizeMb,
      } as SliderAttrs),

      m(Slider, {
        title: 'Max duration',
        icon: 'timer',
        values: [S(10), S(15), S(30), S(60), M(5), M(30), H(1), H(6), H(12)],
        isTime: true,
        unit: 'h:m:s',
        set: (cfg, val) => cfg.durationMs = val,
        get: (cfg) => cfg.durationMs,
      } as SliderAttrs),
      m(Slider, {
        title: 'Max file size',
        icon: 'save',
        cssClass: cfg.mode !== 'LONG_TRACE' ? '.hide' : '',
        values: [5, 25, 50, 100, 500, 1000, 1000 * 5, 1000 * 10],
        unit: 'MB',
        set: (cfg, val) => cfg.maxFileSizeMb = val,
        get: (cfg) => cfg.maxFileSizeMb,
      } as SliderAttrs),
      m(Slider, {
        title: 'Flush on disk every',
        cssClass: cfg.mode !== 'LONG_TRACE' ? '.hide' : '',
        icon: 'av_timer',
        values: [100, 250, 500, 1000, 2500, 5000],
        unit: 'ms',
        set: (cfg, val) => cfg.fileWritePeriodMs = val,
        get: (cfg) => cfg.fileWritePeriodMs || 0,
      } as SliderAttrs));
}

export function PowerSettings(cssClass: string) {
  const DOC_URL = 'https://perfetto.dev/docs/data-sources/battery-counters';
  const descr =
      [m('div',
         m('span', `Polls charge counters and instantaneous power draw from
                    the battery power management IC and the power rails from
                    the PowerStats HAL (`),
         m('a', {href: DOC_URL, target: '_blank'}, 'see docs for more'),
         m('span', ')'))];
  if (globals.isInternalUser) {
    descr.push(m(
        'div',
        m('span', 'Googlers: See '),
        m('a',
          {href: 'http://go/power-rails-internal-doc', target: '_blank'},
          'this doc'),
        m('span', ` for instructions on how to change the refault rail selection
                  on internal devices.`),
        ));
  }
  return m(
      `.record-section${cssClass}`,
      m(Probe,
        {
          title: 'Battery drain & power rails',
          img: 'rec_battery_counters.png',
          descr,
          setEnabled: (cfg, val) => cfg.batteryDrain = val,
          isEnabled: (cfg) => cfg.batteryDrain,
        } as ProbeAttrs,
        m(Slider, {
          title: 'Poll interval',
          cssClass: '.thin',
          values: POLL_INTERVAL_MS,
          unit: 'ms',
          set: (cfg, val) => cfg.batteryDrainPollMs = val,
          get: (cfg) => cfg.batteryDrainPollMs,
        } as SliderAttrs)),
      m(Probe, {
        title: 'Board voltages & frequencies',
        img: 'rec_board_voltage.png',
        descr: 'Tracks voltage and frequency changes from board sensors',
        setEnabled: (cfg, val) => cfg.boardSensors = val,
        isEnabled: (cfg) => cfg.boardSensors,
      } as ProbeAttrs));
}

export function GpuSettings(cssClass: string) {
  return m(
      `.record-section${cssClass}`,
      m(Probe, {
        title: 'GPU frequency',
        img: 'rec_cpu_freq.png',
        descr: 'Records gpu frequency via ftrace',
        setEnabled: (cfg, val) => cfg.gpuFreq = val,
        isEnabled: (cfg) => cfg.gpuFreq,
      } as ProbeAttrs),
      m(Probe, {
        title: 'GPU memory',
        img: 'rec_gpu_mem_total.png',
        descr: `Allows to track per process and global total GPU memory usages.
                (Available on recent Android 12+ kernels)`,
        setEnabled: (cfg, val) => cfg.gpuMemTotal = val,
        isEnabled: (cfg) => cfg.gpuMemTotal,
      } as ProbeAttrs));
}

export function CpuSettings(cssClass: string) {
  return m(
      `.record-section${cssClass}`,
      m(Probe,
        {
          title: 'Coarse CPU usage counter',
          img: 'rec_cpu_coarse.png',
          descr: `Lightweight polling of CPU usage counters via /proc/stat.
                    Allows to periodically monitor CPU usage.`,
          setEnabled: (cfg, val) => cfg.cpuCoarse = val,
          isEnabled: (cfg) => cfg.cpuCoarse,
        } as ProbeAttrs,
        m(Slider, {
          title: 'Poll interval',
          cssClass: '.thin',
          values: POLL_INTERVAL_MS,
          unit: 'ms',
          set: (cfg, val) => cfg.cpuCoarsePollMs = val,
          get: (cfg) => cfg.cpuCoarsePollMs,
        } as SliderAttrs)),
      m(Probe, {
        title: 'Scheduling details',
        img: 'rec_cpu_fine.png',
        descr: 'Enables high-detailed tracking of scheduling events',
        setEnabled: (cfg, val) => cfg.cpuSched = val,
        isEnabled: (cfg) => cfg.cpuSched,
      } as ProbeAttrs),
      m(Probe, {
        title: 'CPU frequency and idle states',
        img: 'rec_cpu_freq.png',
        descr: 'Records cpu frequency and idle state changes via ftrace',
        setEnabled: (cfg, val) => cfg.cpuFreq = val,
        isEnabled: (cfg) => cfg.cpuFreq,
      } as ProbeAttrs),
      m(Probe, {
        title: 'Syscalls',
        img: 'rec_syscalls.png',
        descr: `Tracks the enter and exit of all syscalls. On Android
                requires a userdebug or eng build.`,
        setEnabled: (cfg, val) => cfg.cpuSyscall = val,
        isEnabled: (cfg) => cfg.cpuSyscall,
      } as ProbeAttrs));
}

export function HeapSettings(cssClass: string) {
  const valuesForMS = [
    0,
    1000,
    10 * 1000,
    30 * 1000,
    60 * 1000,
    5 * 60 * 1000,
    10 * 60 * 1000,
    30 * 60 * 1000,
    60 * 60 * 1000,
  ];
  const valuesForShMemBuff = [
    0,
    512,
    1024,
    2 * 1024,
    4 * 1024,
    8 * 1024,
    16 * 1024,
    32 * 1024,
    64 * 1024,
    128 * 1024,
    256 * 1024,
    512 * 1024,
    1024 * 1024,
    64 * 1024 * 1024,
    128 * 1024 * 1024,
    256 * 1024 * 1024,
    512 * 1024 * 1024,
  ];

  return m(
      `.${cssClass}`,
      m(Textarea, {
        title: 'Names or pids of the processes to track',
        docsLink:
            'https://perfetto.dev/docs/data-sources/native-heap-profiler#heapprofd-targets',
        placeholder: 'One per line, e.g.:\n' +
            'system_server\n' +
            'com.google.android.apps.photos\n' +
            '1503',
        set: (cfg, val) => cfg.hpProcesses = val,
        get: (cfg) => cfg.hpProcesses,
      } as TextareaAttrs),
      m(Slider, {
        title: 'Sampling interval',
        cssClass: '.thin',
        values: [
          /* eslint-disable no-multi-spaces */
          0,     1,     2,      4,      8,      16,      32,   64,
          128,   256,   512,    1024,   2048,   4096,    8192, 16384,
          32768, 65536, 131072, 262144, 524288, 1048576,
          /* eslint-enable no-multi-spaces */
        ],
        unit: 'B',
        min: 0,
        set: (cfg, val) => cfg.hpSamplingIntervalBytes = val,
        get: (cfg) => cfg.hpSamplingIntervalBytes,
      } as SliderAttrs),
      m(Slider, {
        title: 'Continuous dumps interval ',
        description: 'Time between following dumps (0 = disabled)',
        cssClass: '.thin',
        values: valuesForMS,
        unit: 'ms',
        min: 0,
        set: (cfg, val) => {
          cfg.hpContinuousDumpsInterval = val;
        },
        get: (cfg) => cfg.hpContinuousDumpsInterval,
      } as SliderAttrs),
      m(Slider, {
        title: 'Continuous dumps phase',
        description: 'Time before first dump',
        cssClass: `.thin${
            globals.state.recordConfig.hpContinuousDumpsInterval === 0 ?
                '.greyed-out' :
                ''}`,
        values: valuesForMS,
        unit: 'ms',
        min: 0,
        disabled: globals.state.recordConfig.hpContinuousDumpsInterval === 0,
        set: (cfg, val) => cfg.hpContinuousDumpsPhase = val,
        get: (cfg) => cfg.hpContinuousDumpsPhase,
      } as SliderAttrs),
      m(Slider, {
        title: `Shared memory buffer`,
        cssClass: '.thin',
        values: valuesForShMemBuff.filter(
            (value) => value === 0 || value >= 8192 && value % 4096 === 0),
        unit: 'B',
        min: 0,
        set: (cfg, val) => cfg.hpSharedMemoryBuffer = val,
        get: (cfg) => cfg.hpSharedMemoryBuffer,
      } as SliderAttrs),
      m(Toggle, {
        title: 'Block client',
        cssClass: '.thin',
        descr: `Slow down target application if profiler cannot keep up.`,
        setEnabled: (cfg, val) => cfg.hpBlockClient = val,
        isEnabled: (cfg) => cfg.hpBlockClient,
      } as ToggleAttrs),
      m(Toggle, {
        title: 'All custom allocators (Q+)',
        cssClass: '.thin',
        descr: `If the target application exposes custom allocators, also
sample from those.`,
        setEnabled: (cfg, val) => cfg.hpAllHeaps = val,
        isEnabled: (cfg) => cfg.hpAllHeaps,
      } as ToggleAttrs),
      // TODO(hjd): Add advanced options.
  );
}

export function JavaHeapDumpSettings(cssClass: string) {
  const valuesForMS = [
    0,
    1000,
    10 * 1000,
    30 * 1000,
    60 * 1000,
    5 * 60 * 1000,
    10 * 60 * 1000,
    30 * 60 * 1000,
    60 * 60 * 1000,
  ];

  return m(
      `.${cssClass}`,
      m(Textarea, {
        title: 'Names or pids of the processes to track',
        placeholder: 'One per line, e.g.:\n' +
            'com.android.vending\n' +
            '1503',
        set: (cfg, val) => cfg.jpProcesses = val,
        get: (cfg) => cfg.jpProcesses,
      } as TextareaAttrs),
      m(Slider, {
        title: 'Continuous dumps interval ',
        description: 'Time between following dumps (0 = disabled)',
        cssClass: '.thin',
        values: valuesForMS,
        unit: 'ms',
        min: 0,
        set: (cfg, val) => {
          cfg.jpContinuousDumpsInterval = val;
        },
        get: (cfg) => cfg.jpContinuousDumpsInterval,
      } as SliderAttrs),
      m(Slider, {
        title: 'Continuous dumps phase',
        description: 'Time before first dump',
        cssClass: `.thin${
            globals.state.recordConfig.jpContinuousDumpsInterval === 0 ?
                '.greyed-out' :
                ''}`,
        values: valuesForMS,
        unit: 'ms',
        min: 0,
        disabled: globals.state.recordConfig.jpContinuousDumpsInterval === 0,
        set: (cfg, val) => cfg.jpContinuousDumpsPhase = val,
        get: (cfg) => cfg.jpContinuousDumpsPhase,
      } as SliderAttrs),
  );
}

export function MemorySettings(cssClass: string) {
  const meminfoOpts = new Map<string, string>();
  for (const x in MeminfoCounters) {
    if (typeof MeminfoCounters[x] === 'number' &&
        !`${x}`.endsWith('_UNSPECIFIED')) {
      meminfoOpts.set(x, x.replace('MEMINFO_', '').toLowerCase());
    }
  }
  const vmstatOpts = new Map<string, string>();
  for (const x in VmstatCounters) {
    if (typeof VmstatCounters[x] === 'number' &&
        !`${x}`.endsWith('_UNSPECIFIED')) {
      vmstatOpts.set(x, x.replace('VMSTAT_', '').toLowerCase());
    }
  }
  return m(
      `.record-section${cssClass}`,
      m(Probe,
        {
          title: 'Native heap profiling',
          img: 'rec_native_heap_profiler.png',
          descr: `Track native heap allocations & deallocations of an Android
               process. (Available on Android 10+)`,
          setEnabled: (cfg, val) => cfg.heapProfiling = val,
          isEnabled: (cfg) => cfg.heapProfiling,
        } as ProbeAttrs,
        HeapSettings(cssClass)),
      m(Probe,
        {
          title: 'Java heap dumps',
          img: 'rec_java_heap_dump.png',
          descr: `Dump information about the Java object graph of an
          Android app. (Available on Android 11+)`,
          setEnabled: (cfg, val) => cfg.javaHeapDump = val,
          isEnabled: (cfg) => cfg.javaHeapDump,
        } as ProbeAttrs,
        JavaHeapDumpSettings(cssClass)),
      m(Probe,
        {
          title: 'Kernel meminfo',
          img: 'rec_meminfo.png',
          descr: 'Polling of /proc/meminfo',
          setEnabled: (cfg, val) => cfg.meminfo = val,
          isEnabled: (cfg) => cfg.meminfo,
        } as ProbeAttrs,
        m(Slider, {
          title: 'Poll interval',
          cssClass: '.thin',
          values: POLL_INTERVAL_MS,
          unit: 'ms',
          set: (cfg, val) => cfg.meminfoPeriodMs = val,
          get: (cfg) => cfg.meminfoPeriodMs,
        } as SliderAttrs),
        m(Dropdown, {
          title: 'Select counters',
          cssClass: '.multicolumn',
          options: meminfoOpts,
          set: (cfg, val) => cfg.meminfoCounters = val,
          get: (cfg) => cfg.meminfoCounters,
        } as DropdownAttrs)),
      m(Probe, {
        title: 'High-frequency memory events',
        img: 'rec_mem_hifreq.png',
        descr: `Allows to track short memory spikes and transitories through
                ftrace's mm_event, rss_stat and ion events. Available only
                on recent Android Q+ kernels`,
        setEnabled: (cfg, val) => cfg.memHiFreq = val,
        isEnabled: (cfg) => cfg.memHiFreq,
      } as ProbeAttrs),
      m(Probe, {
        title: 'Low memory killer',
        img: 'rec_lmk.png',
        descr: `Record LMK events. Works both with the old in-kernel LMK
                and the newer userspace lmkd. It also tracks OOM score
                adjustments.`,
        setEnabled: (cfg, val) => cfg.memLmk = val,
        isEnabled: (cfg) => cfg.memLmk,
      } as ProbeAttrs),
      m(Probe,
        {
          title: 'Per process stats',
          img: 'rec_ps_stats.png',
          descr: `Periodically samples all processes in the system tracking:
                    their thread list, memory counters (RSS, swap and other
                    /proc/status counters) and oom_score_adj.`,
          setEnabled: (cfg, val) => cfg.procStats = val,
          isEnabled: (cfg) => cfg.procStats,
        } as ProbeAttrs,
        m(Slider, {
          title: 'Poll interval',
          cssClass: '.thin',
          values: POLL_INTERVAL_MS,
          unit: 'ms',
          set: (cfg, val) => cfg.procStatsPeriodMs = val,
          get: (cfg) => cfg.procStatsPeriodMs,
        } as SliderAttrs)),
      m(Probe,
        {
          title: 'Virtual memory stats',
          img: 'rec_vmstat.png',
          descr: `Periodically polls virtual memory stats from /proc/vmstat.
                    Allows to gather statistics about swap, eviction,
                    compression and pagecache efficiency`,
          setEnabled: (cfg, val) => cfg.vmstat = val,
          isEnabled: (cfg) => cfg.vmstat,
        } as ProbeAttrs,
        m(Slider, {
          title: 'Poll interval',
          cssClass: '.thin',
          values: POLL_INTERVAL_MS,
          unit: 'ms',
          set: (cfg, val) => cfg.vmstatPeriodMs = val,
          get: (cfg) => cfg.vmstatPeriodMs,
        } as SliderAttrs),
        m(Dropdown, {
          title: 'Select counters',
          cssClass: '.multicolumn',
          options: vmstatOpts,
          set: (cfg, val) => cfg.vmstatCounters = val,
          get: (cfg) => cfg.vmstatCounters,
        } as DropdownAttrs)));
}

function AtraceAppsList() {
  if (globals.state.recordConfig.allAtraceApps) {
    return m('div');
  }

  return m(Textarea, {
    placeholder: 'Apps to profile, one per line, e.g.:\n' +
        'com.android.phone\n' +
        'lmkd\n' +
        'com.android.nfc',
    cssClass: '.atrace-apps-list',
    set: (cfg, val) => cfg.atraceApps = val,
    get: (cfg) => cfg.atraceApps,
  } as TextareaAttrs);
}

export function AndroidSettings(cssClass: string) {
  return m(
      `.record-section${cssClass}`,
      m(Probe,
        {
          title: 'Atrace userspace annotations',
          img: 'rec_atrace.png',
          descr: `Enables C++ / Java codebase annotations (ATRACE_BEGIN() /
                    os.Trace())`,
          setEnabled: (cfg, val) => cfg.atrace = val,
          isEnabled: (cfg) => cfg.atrace,
        } as ProbeAttrs,
        m(Dropdown, {
          title: 'Categories',
          cssClass: '.multicolumn.atrace-categories',
          options: ATRACE_CATEGORIES,
          set: (cfg, val) => cfg.atraceCats = val,
          get: (cfg) => cfg.atraceCats,
        } as DropdownAttrs),
        m(Toggle, {
          title: 'Record events from all Android apps and services',
          descr: '',
          setEnabled: (cfg, val) => cfg.allAtraceApps = val,
          isEnabled: (cfg) => cfg.allAtraceApps,
        } as ToggleAttrs),
        AtraceAppsList()),
      m(Probe,
        {
          title: 'Event log (logcat)',
          img: 'rec_logcat.png',
          descr: `Streams the event log into the trace. If no buffer filter is
                    specified, all buffers are selected.`,
          setEnabled: (cfg, val) => cfg.androidLogs = val,
          isEnabled: (cfg) => cfg.androidLogs,
        } as ProbeAttrs,
        m(Dropdown, {
          title: 'Buffers',
          cssClass: '.multicolumn',
          options: LOG_BUFFERS,
          set: (cfg, val) => cfg.androidLogBuffers = val,
          get: (cfg) => cfg.androidLogBuffers,
        } as DropdownAttrs)),
      m(Probe, {
        title: 'Frame timeline',
        img: 'rec_frame_timeline.png',
        descr: `Records expected/actual frame timings from surface_flinger.
                    Requires Android 12 (S) or above.`,
        setEnabled: (cfg, val) => cfg.androidFrameTimeline = val,
        isEnabled: (cfg) => cfg.androidFrameTimeline,
      } as ProbeAttrs));
}


export function ChromeSettings(cssClass: string) {
  return m(
      `.record-section${cssClass}`,
      CompactProbe({
        title: 'Task scheduling',
        setEnabled: (cfg, val) => cfg.taskScheduling = val,
        isEnabled: (cfg) => cfg.taskScheduling,
      }),
      CompactProbe({
        title: 'IPC flows',
        setEnabled: (cfg, val) => cfg.ipcFlows = val,
        isEnabled: (cfg) => cfg.ipcFlows,
      }),
      CompactProbe({
        title: 'Javascript execution',
        setEnabled: (cfg, val) => cfg.jsExecution = val,
        isEnabled: (cfg) => cfg.jsExecution,
      }),
      CompactProbe({
        title: 'Web content rendering, layout and compositing',
        setEnabled: (cfg, val) => cfg.webContentRendering = val,
        isEnabled: (cfg) => cfg.webContentRendering,
      }),
      CompactProbe({
        title: 'UI rendering & surface compositing',
        setEnabled: (cfg, val) => cfg.uiRendering = val,
        isEnabled: (cfg) => cfg.uiRendering,
      }),
      CompactProbe({
        title: 'Input events',
        setEnabled: (cfg, val) => cfg.inputEvents = val,
        isEnabled: (cfg) => cfg.inputEvents,
      }),
      CompactProbe({
        title: 'Navigation & Loading',
        setEnabled: (cfg, val) => cfg.navigationAndLoading = val,
        isEnabled: (cfg) => cfg.navigationAndLoading,
      }),
      CompactProbe({
        title: 'Chrome Logs',
        setEnabled: (cfg, val) => cfg.chromeLogs = val,
        isEnabled: (cfg) => cfg.chromeLogs,
      }),
      ChromeCategoriesSelection());
}

function ChromeCategoriesSelection() {
  // If we are attempting to record via the Chrome extension, we receive the
  // list of actually supported categories via DevTools. Otherwise, we fall back
  // to an integrated list of categories from a recent version of Chrome.
  let categories = globals.state.chromeCategories;
  if (!categories || !isChromeTarget(globals.state.recordingTarget)) {
    categories = getBuiltinChromeCategoryList();
  }

  const defaultCategories = new Map<string, string>();
  const disabledByDefaultCategories = new Map<string, string>();
  const disabledPrefix = 'disabled-by-default-';
  categories.forEach((cat) => {
    if (cat.startsWith(disabledPrefix)) {
      disabledByDefaultCategories.set(cat, cat.replace(disabledPrefix, ''));
    } else {
      defaultCategories.set(cat, cat);
    }
  });

  return m(
      '.chrome-categories',
      m(CategoriesCheckboxList, {
        categories: defaultCategories,
        title: 'Additional categories',
        get: (cfg) => cfg.chromeCategoriesSelected,
        set: (cfg, val) => cfg.chromeCategoriesSelected = val,
      }),
      m(CategoriesCheckboxList, {
        categories: disabledByDefaultCategories,
        title: 'High overhead categories',
        get: (cfg) => cfg.chromeHighOverheadCategoriesSelected,
        set: (cfg, val) => cfg.chromeHighOverheadCategoriesSelected = val,
      }));
}

export function AdvancedSettings(cssClass: string) {
  return m(
      `.record-section${cssClass}`,
      m(Probe,
        {
          title: 'Advanced ftrace config',
          img: 'rec_ftrace.png',
          descr: `Enable individual events and tune the kernel-tracing (ftrace)
                  module. The events enabled here are in addition to those from
                  enabled by other probes.`,
          setEnabled: (cfg, val) => cfg.ftrace = val,
          isEnabled: (cfg) => cfg.ftrace,
        } as ProbeAttrs,
        m(Toggle, {
          title: 'Resolve kernel symbols',
          cssClass: '.thin',
          descr: `Enables lookup via /proc/kallsyms for workqueue,
              sched_blocked_reason and other events (userdebug/eng builds only).`,
          setEnabled: (cfg, val) => cfg.symbolizeKsyms = val,
          isEnabled: (cfg) => cfg.symbolizeKsyms,
        } as ToggleAttrs),
        m(Slider, {
          title: 'Buf size',
          cssClass: '.thin',
          values: [0, 512, 1024, 2 * 1024, 4 * 1024, 16 * 1024, 32 * 1024],
          unit: 'KB',
          zeroIsDefault: true,
          set: (cfg, val) => cfg.ftraceBufferSizeKb = val,
          get: (cfg) => cfg.ftraceBufferSizeKb,
        } as SliderAttrs),
        m(Slider, {
          title: 'Drain rate',
          cssClass: '.thin',
          values: [0, 100, 250, 500, 1000, 2500, 5000],
          unit: 'ms',
          zeroIsDefault: true,
          set: (cfg, val) => cfg.ftraceDrainPeriodMs = val,
          get: (cfg) => cfg.ftraceDrainPeriodMs,
        } as SliderAttrs),
        m(Dropdown, {
          title: 'Event groups',
          cssClass: '.multicolumn.ftrace-events',
          options: FTRACE_CATEGORIES,
          set: (cfg, val) => cfg.ftraceEvents = val,
          get: (cfg) => cfg.ftraceEvents,
        } as DropdownAttrs),
        m(Textarea, {
          placeholder: 'Add extra events, one per line, e.g.:\n' +
              'sched/sched_switch\n' +
              'kmem/*',
          set: (cfg, val) => cfg.ftraceExtraEvents = val,
          get: (cfg) => cfg.ftraceExtraEvents,
        } as TextareaAttrs)));
}

function RecordHeader() {
  return m(
      '.record-header',
      m('.top-part',
        m('.target-and-status',
          RecordingPlatformSelection(),
          RecordingStatusLabel(),
          ErrorLabel()),
        recordingButtons()),
      RecordingNotes());
}

function RecordingPlatformSelection() {
  if (globals.state.recordingInProgress) return [];

  const availableAndroidDevices = globals.state.availableAdbDevices;
  const recordingTarget = globals.state.recordingTarget;

  const targets = [];
  for (const {os, name} of getDefaultRecordingTargets()) {
    targets.push(m('option', {value: os}, name));
  }
  for (const d of availableAndroidDevices) {
    targets.push(m('option', {value: d.serial}, d.name));
  }

  const selectedIndex = isAdbTarget(recordingTarget) ?
      targets.findIndex((node) => node.attrs.value === recordingTarget.serial) :
      targets.findIndex((node) => node.attrs.value === recordingTarget.os);

  return m(
      '.target',
      m(
          'label',
          'Target platform:',
          m('select',
            {
              selectedIndex,
              onchange: (e: Event) => {
                onTargetChange((e.target as HTMLSelectElement).value);
              },
              onupdate: (select) => {
                // Work around mithril bug
                // (https://github.com/MithrilJS/mithril.js/issues/2107): We may
                // update the select's options while also changing the
                // selectedIndex at the same time. The update of selectedIndex
                // may be applied before the new options are added to the select
                // element. Because the new selectedIndex may be outside of the
                // select's options at that time, we have to reselect the
                // correct index here after any new children were added.
                (select.dom as HTMLSelectElement).selectedIndex = selectedIndex;
              },
            },
            ...targets),
          ),
      m('.chip',
        {onclick: addAndroidDevice},
        m('button', 'Add ADB Device'),
        m('i.material-icons', 'add')));
}

// |target| can be the TargetOs or the android serial.
function onTargetChange(target: string) {
  const recordingTarget: RecordingTarget =
      globals.state.availableAdbDevices.find((d) => d.serial === target) ||
      getDefaultRecordingTargets().find((t) => t.os === target) ||
      getDefaultRecordingTargets()[0];

  if (isChromeTarget(recordingTarget)) {
    globals.dispatch(Actions.setFetchChromeCategories({fetch: true}));
  }

  globals.dispatch(Actions.setRecordingTarget({target: recordingTarget}));
  recordTargetStore.save(target);
  globals.rafScheduler.scheduleFullRedraw();
}

function Instructions(cssClass: string) {
  return m(
      `.record-section.instructions${cssClass}`,
      m('header', 'Recording command'),
      PERSIST_CONFIG_FLAG.get() ?
          m('button.permalinkconfig',
            {
              onclick: () => {
                globals.dispatch(
                    Actions.createPermalink({isRecordingConfig: true}));
              },
            },
            'Share recording settings') :
          null,
      RecordingSnippet(),
      BufferUsageProgressBar(),
      m('.buttons', StopCancelButtons()),
      recordingLog());
}

export function loadedConfigEqual(
    cfg1: LoadedConfig, cfg2: LoadedConfig): boolean {
  return cfg1.type === 'NAMED' && cfg2.type === 'NAMED' ?
      cfg1.name === cfg2.name :
      cfg1.type === cfg2.type;
}

export function loadConfigButton(
    config: RecordConfig, configType: LoadedConfig): m.Vnode {
  return m(
      'button',
      {
        class: 'config-button',
        title: 'Apply configuration settings',
        disabled: loadedConfigEqual(configType, globals.state.lastLoadedConfig),
        onclick: () => {
          globals.dispatch(Actions.setRecordConfig({config, configType}));
          globals.rafScheduler.scheduleFullRedraw();
        },
      },
      m('i.material-icons', 'file_upload'));
}

export function displayRecordConfigs() {
  const configs = [];
  if (autosaveConfigStore.hasSavedConfig) {
    configs.push(m('.config', [
      m('span.title-config', m('strong', 'Latest started recording')),
      loadConfigButton(autosaveConfigStore.get(), {type: 'AUTOMATIC'}),
    ]));
  }
  for (const validated of recordConfigStore.recordConfigs) {
    const item = validated.result;
    configs.push(m('.config', [
      m('span.title-config', item.title),
      loadConfigButton(item.config, {type: 'NAMED', name: item.title}),
      m('button',
        {
          class: 'config-button',
          title: 'Overwrite configuration with current settings',
          onclick: () => {
            if (confirm(`Overwrite config "${
                    item.title}" with current settings?`)) {
              recordConfigStore.overwrite(globals.state.recordConfig, item.key);
              globals.dispatch(Actions.setRecordConfig({
                config: item.config,
                configType: {type: 'NAMED', name: item.title},
              }));
              globals.rafScheduler.scheduleFullRedraw();
            }
          },
        },
        m('i.material-icons', 'save')),
      m('button',
        {
          class: 'config-button',
          title: 'Remove configuration',
          onclick: () => {
            recordConfigStore.delete(item.key);
            globals.rafScheduler.scheduleFullRedraw();
          },
        },
        m('i.material-icons', 'delete')),
    ]));

    const errorItems = [];
    for (const extraKey of validated.extraKeys) {
      errorItems.push(m('li', `${extraKey} is unrecognised`));
    }
    for (const invalidKey of validated.invalidKeys) {
      errorItems.push(m('li', `${invalidKey} contained an invalid value`));
    }

    if (errorItems.length > 0) {
      configs.push(
          m('.parsing-errors',
            'One or more errors have been found while loading configuration "' +
                item.title + '". Loading is possible, but make sure to check ' +
                'the settings afterwards.',
            m('ul', errorItems)));
    }
  }
  return configs;
}

export const ConfigTitleState = {
  title: '',
  getTitle: () => {
    return ConfigTitleState.title;
  },
  setTitle: (value: string) => {
    ConfigTitleState.title = value;
  },
  clearTitle: () => {
    ConfigTitleState.title = '';
  },
};

export function Configurations(cssClass: string) {
  const canSave = recordConfigStore.canSave(ConfigTitleState.getTitle());
  return m(
      `.record-section${cssClass}`,
      m('header', 'Save and load configurations'),
      m('.input-config',
        [
          m('input', {
            value: ConfigTitleState.title,
            placeholder: 'Title for config',
            oninput() {
              ConfigTitleState.setTitle(this.value);
              globals.rafScheduler.scheduleFullRedraw();
            },
          }),
          m('button',
            {
              class: 'config-button',
              disabled: !canSave,
              title: canSave ? 'Save current config' :
                               'Duplicate name, saving disabled',
              onclick: () => {
                recordConfigStore.save(
                    globals.state.recordConfig, ConfigTitleState.getTitle());
                globals.rafScheduler.scheduleFullRedraw();
                ConfigTitleState.clearTitle();
              },
            },
            m('i.material-icons', 'save')),
          m('button',
            {
              class: 'config-button',
              title: 'Clear current configuration',
              onclick: () => {
                if (confirm(
                        'Current configuration will be cleared. ' +
                        'Are you sure?')) {
                  globals.dispatch(Actions.setRecordConfig({
                    config: createEmptyRecordConfig(),
                    configType: {type: 'NONE'},
                  }));
                  globals.rafScheduler.scheduleFullRedraw();
                }
              },
            },
            m('i.material-icons', 'delete_forever')),
        ]),
      displayRecordConfigs());
}

function BufferUsageProgressBar() {
  if (!globals.state.recordingInProgress) return [];

  const bufferUsage = globals.bufferUsage ? globals.bufferUsage : 0.0;
  // Buffer usage is not available yet on Android.
  if (bufferUsage === 0) return [];

  return m(
      'label',
      'Buffer usage: ',
      m('progress', {max: 100, value: bufferUsage * 100}));
}

function RecordingNotes() {
  const sideloadUrl =
      'https://perfetto.dev/docs/contributing/build-instructions#get-the-code';
  const linuxUrl = 'https://perfetto.dev/docs/quickstart/linux-tracing';
  const cmdlineUrl =
      'https://perfetto.dev/docs/quickstart/android-tracing#perfetto-cmdline';
  const extensionURL = `https://chrome.google.com/webstore/detail/
      perfetto-ui/lfmkphfpdbjijhpomgecfikhfohaoine`;

  const notes: m.Children = [];

  const msgFeatNotSupported =
      m('span', `Some probes are only supported in Perfetto versions running
      on Android Q+. `);

  const msgPerfettoNotSupported =
      m('span', `Perfetto is not supported natively before Android P. `);

  const msgSideload =
      m('span',
        `If you have a rooted device you can `,
        m('a',
          {href: sideloadUrl, target: '_blank'},
          `sideload the latest version of
         Perfetto.`));

  const msgRecordingNotSupported =
      m('.note',
        `Recording Perfetto traces from the UI is not supported natively
     before Android Q. If you are using a P device, please select 'Android P'
     as the 'Target Platform' and `,
        m('a',
          {href: cmdlineUrl, target: '_blank'},
          `collect the trace using ADB.`));

  const msgChrome =
      m('.note',
        `To trace Chrome from the Perfetto UI, you need to install our `,
        m('a', {href: extensionURL, target: '_blank'}, 'Chrome extension'),
        ' and then reload this page.');

  const msgLinux =
      m('.note',
        `Use this `,
        m('a', {href: linuxUrl, target: '_blank'}, `quickstart guide`),
        ` to get started with tracing on Linux.`);

  const msgLongTraces = m(
      '.note',
      `Recording in long trace mode through the UI is not supported. Please copy
    the command and `,
      m('a',
        {href: cmdlineUrl, target: '_blank'},
        `collect the trace using ADB.`));

  const msgZeroProbes =
      m('.note',
        'It looks like you didn\'t add any probes. ' +
            'Please add at least one to get a non-empty trace.');

  if (!hasActiveProbes(globals.state.recordConfig)) {
    notes.push(msgZeroProbes);
  }

  if (isAdbTarget(globals.state.recordingTarget)) {
    notes.push(msgRecordingNotSupported);
  }
  switch (globals.state.recordingTarget.os) {
    case 'Q':
      break;
    case 'P':
      notes.push(m('.note', msgFeatNotSupported, msgSideload));
      break;
    case 'O':
      notes.push(m('.note', msgPerfettoNotSupported, msgSideload));
      break;
    case 'L':
      notes.push(msgLinux);
      break;
    case 'C':
      if (!globals.state.extensionInstalled) notes.push(msgChrome);
      break;
    case 'CrOS':
      if (!globals.state.extensionInstalled) notes.push(msgChrome);
      break;
    default:
  }
  if (globals.state.recordConfig.mode === 'LONG_TRACE') {
    notes.unshift(msgLongTraces);
  }

  return notes.length > 0 ? m('div', notes) : [];
}

function RecordingSnippet() {
  const target = globals.state.recordingTarget;

  // We don't need commands to start tracing on chrome
  if (isChromeTarget(target)) {
    return globals.state.extensionInstalled ?
        m('div',
          m('label',
            `To trace Chrome from the Perfetto UI you just have to press
         'Start Recording'.`)) :
        [];
  }
  return m(CodeSnippet, {text: getRecordCommand(target)});
}

function getRecordCommand(target: RecordingTarget) {
  const data = globals.trackDataStore.get('config') as
          {commandline: string, pbtxt: string, pbBase64: string} |
      null;

  const cfg = globals.state.recordConfig;
  let time = cfg.durationMs / 1000;

  if (time > MAX_TIME) {
    time = MAX_TIME;
  }

  const pbBase64 = data ? data.pbBase64 : '';
  const pbtx = data ? data.pbtxt : '';
  let cmd = '';
  if (isAndroidP(target)) {
    cmd += `echo '${pbBase64}' | \n`;
    cmd += 'base64 --decode | \n';
    cmd += 'adb shell "perfetto -c - -o /data/misc/perfetto-traces/trace"\n';
  } else {
    cmd +=
        isAndroidTarget(target) ? 'adb shell perfetto \\\n' : 'perfetto \\\n';
    cmd += '  -c - --txt \\\n';
    cmd += '  -o /data/misc/perfetto-traces/trace \\\n';
    cmd += '<<EOF\n\n';
    cmd += pbtx;
    cmd += '\nEOF\n';
  }
  return cmd;
}

function recordingButtons() {
  const state = globals.state;
  const target = state.recordingTarget;
  const recInProgress = state.recordingInProgress;

  const start =
      m(`button`,
        {
          class: recInProgress ? '' : 'selected',
          onclick: onStartRecordingPressed,
        },
        'Start Recording');

  const buttons: m.Children = [];

  if (isAndroidTarget(target)) {
    if (!recInProgress && isAdbTarget(target) &&
        globals.state.recordConfig.mode !== 'LONG_TRACE') {
      buttons.push(start);
    }
  } else if (isChromeTarget(target) && state.extensionInstalled) {
    buttons.push(start);
  }
  return m('.button', buttons);
}

function StopCancelButtons() {
  if (!globals.state.recordingInProgress) return [];

  const stop =
      m(`button.selected`,
        {onclick: () => globals.dispatch(Actions.stopRecording({}))},
        'Stop');

  const cancel =
      m(`button`,
        {onclick: () => globals.dispatch(Actions.cancelRecording({}))},
        'Cancel');

  return [stop, cancel];
}

function onStartRecordingPressed() {
  location.href = '#!/record/instructions';
  globals.rafScheduler.scheduleFullRedraw();
  autosaveConfigStore.save(globals.state.recordConfig);

  const target = globals.state.recordingTarget;
  if (isAndroidTarget(target) || isChromeTarget(target)) {
    globals.logging.logEvent('Record Trace', `Record trace (${target.os})`);
    globals.dispatch(Actions.startRecording({}));
  }
}

function RecordingStatusLabel() {
  const recordingStatus = globals.state.recordingStatus;
  if (!recordingStatus) return [];
  return m('label', recordingStatus);
}

export function ErrorLabel() {
  const lastRecordingError = globals.state.lastRecordingError;
  if (!lastRecordingError) return [];
  return m('label.error-label', `Error:  ${lastRecordingError}`);
}

function recordingLog() {
  const logs = globals.recordingLog;
  if (logs === undefined) return [];
  return m('.code-snippet.no-top-bar', m('code', logs));
}

// The connection must be done in the frontend. After it, the serial ID will
// be inserted in the state, and the worker will be able to connect to the
// correct device.
async function addAndroidDevice() {
  let device: USBDevice;
  try {
    device = await new AdbOverWebUsb().findDevice();
  } catch (e) {
    const err = `No device found: ${e.name}: ${e.message}`;
    console.error(err, e);
    alert(err);
    return;
  }

  if (!device.serialNumber) {
    console.error('serial number undefined');
    return;
  }

  // After the user has selected a device with the chrome UI, it will be
  // available when listing all the available device from WebUSB. Therefore,
  // we update the list of available devices.
  await updateAvailableAdbDevices(device.serialNumber);
}

export async function updateAvailableAdbDevices(
    preferredDeviceSerial?: string) {
  const devices = await new AdbOverWebUsb().getPairedDevices();

  let recordingTarget: AdbRecordingTarget|undefined = undefined;

  const availableAdbDevices: AdbRecordingTarget[] = [];
  devices.forEach((d) => {
    if (d.productName && d.serialNumber) {
      // TODO(nicomazz): At this stage, we can't know the OS version, so we
      // assume it is 'Q'. This can create problems with devices with an old
      // version of perfetto. The os detection should be done after the adb
      // connection, from adb_record_controller
      availableAdbDevices.push(
          {name: d.productName, serial: d.serialNumber, os: 'Q'});
      if (preferredDeviceSerial && preferredDeviceSerial === d.serialNumber) {
        recordingTarget = availableAdbDevices[availableAdbDevices.length - 1];
      }
    }
  });

  globals.dispatch(
      Actions.setAvailableAdbDevices({devices: availableAdbDevices}));
  selectAndroidDeviceIfAvailable(availableAdbDevices, recordingTarget);
  globals.rafScheduler.scheduleFullRedraw();
  return availableAdbDevices;
}

function selectAndroidDeviceIfAvailable(
    availableAdbDevices: AdbRecordingTarget[],
    recordingTarget?: RecordingTarget) {
  if (!recordingTarget) {
    recordingTarget = globals.state.recordingTarget;
  }
  const deviceConnected = isAdbTarget(recordingTarget);
  const connectedDeviceDisconnected = deviceConnected &&
      availableAdbDevices.find(
          (e) => e.serial ===
              (recordingTarget as AdbRecordingTarget).serial) === undefined;

  if (availableAdbDevices.length) {
    // If there's an Android device available and the current selection isn't
    // one, select the Android device by default. If the current device isn't
    // available anymore, but another Android device is, select the other
    // Android device instead.
    if (!deviceConnected || connectedDeviceDisconnected) {
      recordingTarget = availableAdbDevices[0];
    }

    globals.dispatch(Actions.setRecordingTarget({target: recordingTarget}));
    return;
  }

  // If the currently selected device was disconnected, reset the recording
  // target to the default one.
  if (connectedDeviceDisconnected) {
    globals.dispatch(
        Actions.setRecordingTarget({target: getDefaultRecordingTargets()[0]}));
  }
}

function recordMenu(routePage: string) {
  const target = globals.state.recordingTarget;
  const chromeProbe =
      m('a[href="#!/record/chrome"]',
        m(`li${routePage === 'chrome' ? '.active' : ''}`,
          m('i.material-icons', 'laptop_chromebook'),
          m('.title', 'Chrome'),
          m('.sub', 'Chrome traces')));
  const cpuProbe =
      m('a[href="#!/record/cpu"]',
        m(`li${routePage === 'cpu' ? '.active' : ''}`,
          m('i.material-icons', 'subtitles'),
          m('.title', 'CPU'),
          m('.sub', 'CPU usage, scheduling, wakeups')));
  const gpuProbe =
      m('a[href="#!/record/gpu"]',
        m(`li${routePage === 'gpu' ? '.active' : ''}`,
          m('i.material-icons', 'aspect_ratio'),
          m('.title', 'GPU'),
          m('.sub', 'GPU frequency, memory')));
  const powerProbe =
      m('a[href="#!/record/power"]',
        m(`li${routePage === 'power' ? '.active' : ''}`,
          m('i.material-icons', 'battery_charging_full'),
          m('.title', 'Power'),
          m('.sub', 'Battery and other energy counters')));
  const memoryProbe =
      m('a[href="#!/record/memory"]',
        m(`li${routePage === 'memory' ? '.active' : ''}`,
          m('i.material-icons', 'memory'),
          m('.title', 'Memory'),
          m('.sub', 'Physical mem, VM, LMK')));
  const androidProbe =
      m('a[href="#!/record/android"]',
        m(`li${routePage === 'android' ? '.active' : ''}`,
          m('i.material-icons', 'android'),
          m('.title', 'Android apps & svcs'),
          m('.sub', 'atrace and logcat')));
  const advancedProbe =
      m('a[href="#!/record/advanced"]',
        m(`li${routePage === 'advanced' ? '.active' : ''}`,
          m('i.material-icons', 'settings'),
          m('.title', 'Advanced settings'),
          m('.sub', 'Complicated stuff for wizards')));
  const recInProgress = globals.state.recordingInProgress;

  const probes = [];
  if (isCrOSTarget(target) || isLinuxTarget(target)) {
    probes.push(cpuProbe, powerProbe, memoryProbe, chromeProbe, advancedProbe);
  } else if (isChromeTarget(target)) {
    probes.push(chromeProbe);
  } else {
    probes.push(
        cpuProbe,
        gpuProbe,
        powerProbe,
        memoryProbe,
        androidProbe,
        chromeProbe,
        advancedProbe);
  }

  return m(
      '.record-menu',
      {
        class: recInProgress ? 'disabled' : '',
        onclick: () => globals.rafScheduler.scheduleFullRedraw(),
      },
      m('header', 'Trace config'),
      m('ul',
        m('a[href="#!/record/buffers"]',
          m(`li${routePage === 'buffers' ? '.active' : ''}`,
            m('i.material-icons', 'tune'),
            m('.title', 'Recording settings'),
            m('.sub', 'Buffer mode, size and duration'))),
        m('a[href="#!/record/instructions"]',
          m(`li${routePage === 'instructions' ? '.active' : ''}`,
            m('i.material-icons.rec', 'fiber_manual_record'),
            m('.title', 'Recording command'),
            m('.sub', 'Manually record trace'))),
        PERSIST_CONFIG_FLAG.get() ?
            m('a[href="#!/record/config"]',
              {
                onclick: () => {
                  recordConfigStore.reloadFromLocalStorage();
                },
              },
              m(`li${routePage === 'config' ? '.active' : ''}`,
                m('i.material-icons', 'save'),
                m('.title', 'Saved configs'),
                m('.sub', 'Manage local configs'))) :
            null),
      m('header', 'Probes'),
      m('ul', probes));
}


export const RecordPage = createPage({
  view({attrs}: m.Vnode<PageAttrs>) {
    const SECTIONS: {[property: string]: (cssClass: string) => m.Child} = {
      buffers: RecSettings,
      instructions: Instructions,
      config: Configurations,
      cpu: CpuSettings,
      gpu: GpuSettings,
      power: PowerSettings,
      memory: MemorySettings,
      android: AndroidSettings,
      chrome: ChromeSettings,
      advanced: AdvancedSettings,
    };

    const pages: m.Children = [];
    // we need to remove the `/` character from the route
    let routePage = attrs.subpage ? attrs.subpage.substr(1) : '';
    if (!Object.keys(SECTIONS).includes(routePage)) {
      routePage = 'buffers';
    }
    for (const key of Object.keys(SECTIONS)) {
      const cssClass = routePage === key ? '.active' : '';
      pages.push(SECTIONS[key](cssClass));
    }

    return m(
        '.record-page',
        globals.state.recordingInProgress ? m('.hider') : [],
        m('.record-container',
          RecordHeader(),
          m('.record-container-content', recordMenu(routePage), pages)));
  },
});
