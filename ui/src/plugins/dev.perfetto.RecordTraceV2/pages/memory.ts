// Copyright (C) 2024 The Android Open Source Project
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

import {assertExists} from '../../../base/logging';
import {splitLinesNonEmpty} from '../../../base/string_utils';
import protos from '../../../protos';
import {
  ADV_PROC_ASSOC_BUF_ID,
  ADV_PROC_ASSOC_PROBE_ID,
  PROC_STATS_DS_NAME,
} from './advanced';
import {RecordProbe, RecordSubpage} from '../config/config_interfaces';
import {TraceConfigBuilder} from '../config/trace_config_builder';
import {TypedMultiselect} from './widgets/multiselect';
import {POLL_INTERVAL_SLIDER, Slider} from './widgets/slider';
import {Textarea} from './widgets/textarea';
import {Toggle} from './widgets/toggle';

const SYS_STAT_DS = 'linux.sys_stats';

export function memoryRecordSection(): RecordSubpage {
  return {
    kind: 'PROBES_PAGE',
    id: 'memory',
    title: 'Memory',
    subtitle: 'Physical mem, VM, LMK',
    icon: 'memory',
    probes: [
      heapProfiling(),
      heapDumps(),
      meminfo(),
      vmstat(),
      hifreq(),
      lmk(),
      polledProcStats(),
    ],
  };
}

function heapProfiling(): RecordProbe {
  const settings = {
    targetProcs: new Textarea({
      title: 'Names or pids of the processes to track (required)',
      docsLink:
        'https://perfetto.dev/docs/data-sources/native-heap-profiler#heapprofd-targets',
      placeholder:
        'One per line, e.g.:\n' +
        'system_server\n' +
        'com.google.android.apps.photos\n' +
        '1503',
    }),
    samplingBytes: new Slider({
      title: 'Sampling interval',
      description: 'Trades off accuracy vs overhead in the target process',
      cssClass: '.thin',
      default: 4096,
      values: [
        1, 16, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536,
        131072, 262144, 524288, 1048576,
      ],
      unit: 'B',
      min: 1,
    }),
    dumpInterval: new Slider({
      title: 'Continuous dump interval',
      description: 'Time between following dumps (0 = only dump at the end)',
      values: SAMPLING_TIMES_MS,
      cssClass: '.thin',
      unit: 'ms',
      min: 0,
    }),
    dumpPhase: new Slider({
      title: 'Continuous dumps phase',
      description: 'Time before first dump',
      values: SAMPLING_TIMES_MS,
      cssClass: '.thin',
      unit: 'ms',
      min: 0,
    }),
    shmemKB: new Slider({
      title: 'Shared memory buffer',
      values: SMB_VALUES_KB,
      default: 8192,
      cssClass: '.thin',
      unit: 'KB',
    }),
    blockClient: new Toggle({
      title: 'Block client',
      cssClass: '.thin',
      default: true,
      descr: `Slow down target application if profiler cannot keep up.`,
    }),
    allHeaps: new Toggle({
      title: 'All custom allocators (Q+)',
      cssClass: '.thin',
      descr:
        'If the target application exposes custom allocators, also ' +
        'sample from those.',
    }),
  };
  return {
    id: 'mem_hprof',
    title: 'Native heap profiling',
    image: 'rec_native_heap_profiler.png',
    description:
      'Track native heap allocations & deallocations of an Android ' +
      'process. (Available on Android 10+)',
    supportedPlatforms: ['ANDROID', 'LINUX'],
    settings,
    genConfig: function (tc: TraceConfigBuilder) {
      const s = settings;
      const [cmdlines, pids] = extractCmdlinesAndPids(s.targetProcs.text);
      tc.addDataSource('android.heapprofd').heapprofdConfig = {
        samplingIntervalBytes: s.samplingBytes.value,
        shmemSizeBytes: s.shmemKB.value * 1024,
        blockClient: s.blockClient.enabled,
        allHeaps: s.allHeaps.enabled,
        processCmdline: cmdlines.length > 0 ? cmdlines : undefined,
        pid: pids.length > 0 ? pids : undefined,
        continuousDumpConfig:
          s.dumpInterval.value == 0
            ? undefined
            : {
                dumpIntervalMs: s.dumpInterval.value,
                dumpPhaseMs: s.dumpPhase.value,
              },
      };
    },
  };
}

function heapDumps(): RecordProbe {
  const settings = {
    targetProcs: new Textarea({
      title: 'Names or pids of the processes to track (required)',
      docsLink: 'https://perfetto.dev/docs/data-sources/java-heap-profiler',
      placeholder:
        'One per line, e.g.:\n' +
        'system_server\n' +
        'com.google.android.apps.photos\n' +
        '1503',
    }),
    dumpInterval: new Slider({
      title: 'Continuous dump interval',
      description: 'Time between following dumps (0 = only dump at the end)',
      values: SAMPLING_TIMES_MS,
      cssClass: '.thin',
      unit: 'ms',
      min: 0,
    }),
    dumpPhase: new Slider({
      title: 'Continuous dumps phase',
      description: 'Time before first dump',
      values: SAMPLING_TIMES_MS,
      cssClass: '.thin',
      unit: 'ms',
      min: 0,
    }),
  };
  return {
    id: 'mem_heapdumps',
    title: 'Java heap dumps',
    image: 'rec_java_heap_dump.png',
    description:
      'Dump information about the Java object graph of an ' +
      'Android app. (Available on Android 11+)',
    supportedPlatforms: ['ANDROID'],
    settings,
    genConfig: function (tc: TraceConfigBuilder) {
      const s = settings;
      const [cmdlines, pids] = extractCmdlinesAndPids(s.targetProcs.text);
      tc.addDataSource('android.java_hprof').javaHprofConfig = {
        processCmdline: cmdlines.length > 0 ? cmdlines : undefined,
        pid: pids.length > 0 ? pids : undefined,
        continuousDumpConfig:
          s.dumpInterval.value == 0
            ? undefined
            : {
                dumpIntervalMs: s.dumpInterval.value,
                dumpPhaseMs: s.dumpPhase.value,
              },
      };
    },
  };
}

function meminfo(): RecordProbe {
  const meminfoCounters = new Map<string, protos.MeminfoCounters>();
  for (const x in protos.MeminfoCounters) {
    if (
      typeof protos.MeminfoCounters[x] === 'number' &&
      !`${x}`.endsWith('_UNSPECIFIED')
    ) {
      meminfoCounters.set(
        x.replace('MEMINFO_', '').toLowerCase(),
        protos.MeminfoCounters[x],
      );
    }
  }
  const settings = {
    pollMs: new Slider(POLL_INTERVAL_SLIDER),
    counters: new TypedMultiselect<protos.MeminfoCounters>({
      options: meminfoCounters,
    }),
  };
  return {
    id: 'mem_meminfo',
    image: 'rec_meminfo.png',
    title: 'Kernel meminfo',
    description: 'Polling of /proc/meminfo',
    supportedPlatforms: ['ANDROID', 'LINUX', 'CHROME_OS'],
    settings,
    genConfig: function (tc: TraceConfigBuilder) {
      const ds = tc.addDataSource(SYS_STAT_DS);
      // sysStatsConfig is shared with other probes, don't clobber.
      const cfg = (ds.sysStatsConfig ??= {});
      cfg.meminfoPeriodMs = settings.pollMs.value;
      cfg.meminfoCounters = settings.counters.selectedValues();
    },
  };
}

function vmstat(): RecordProbe {
  const vmstatCounters = new Map<string, protos.VmstatCounters>();
  for (const x in protos.VmstatCounters) {
    if (
      typeof protos.VmstatCounters[x] === 'number' &&
      !`${x}`.endsWith('_UNSPECIFIED')
    ) {
      vmstatCounters.set(
        x.replace('VMSTAT_', '').toLowerCase(),
        protos.VmstatCounters[x],
      );
    }
  }
  const settings = {
    pollMs: new Slider(POLL_INTERVAL_SLIDER),
    counters: new TypedMultiselect<protos.VmstatCounters>({
      options: vmstatCounters,
    }),
  };
  return {
    id: 'mem_vmstat',
    title: 'Virtual memory stats',
    image: 'rec_vmstat.png',
    description:
      'Periodically polls virtual memory stats from /proc/vmstat. ' +
      'Allows to gather statistics about swap, eviction, ' +
      'compression and pagecache efficiency',
    supportedPlatforms: ['ANDROID', 'LINUX', 'CHROME_OS'],
    settings,
    genConfig: function (tc: TraceConfigBuilder) {
      const ds = tc.addDataSource(SYS_STAT_DS);
      // sysStatsConfig is shared with other probes, don't clobber.
      const cfg = (ds.sysStatsConfig ??= {});
      cfg.vmstatPeriodMs = settings.pollMs.value;
      cfg.vmstatCounters = settings.counters.selectedValues();
    },
  };
}

function hifreq(): RecordProbe {
  return {
    id: 'mem_hifreq',
    title: 'High-frequency memory events',
    image: 'rec_mem_hifreq.png',
    dependencies: [ADV_PROC_ASSOC_PROBE_ID],
    description:
      'Allows to track short memory spikes and transitories through ' +
      "ftrace's mm_event, rss_stat and ion events. Available only " +
      'on recent Android Q+ kernels',
    supportedPlatforms: ['ANDROID'],
    genConfig: function (tc: TraceConfigBuilder) {
      tc.addFtraceEvents(
        'mm_event/mm_event_record',
        'kmem/rss_stat',
        'ion/ion_stat',
        'dmabuf_heap/dma_heap_stat',
        'kmem/ion_heap_grow',
        'kmem/ion_heap_shrink',
      );
    },
  };
}

function lmk(): RecordProbe {
  return {
    id: 'mem_lmk',
    title: 'Low memory killer',
    image: 'rec_lmk.png',
    dependencies: [ADV_PROC_ASSOC_PROBE_ID],
    description:
      'Record LMK events. Works both with the old in-kernel LMK ' +
      'and the newer userspace lmkd. It also tracks OOM score adjustments.',
    supportedPlatforms: ['ANDROID', 'LINUX', 'CHROME_OS'],
    genConfig: function (tc: TraceConfigBuilder) {
      tc.addFtraceEvents(
        // For in-kernel LMK (roughly older devices until Go and Pixel 3).
        'lowmemorykiller/lowmemory_kill',
        'oom/oom_score_adj_update',
      );

      // For userspace LMKd (newer devices).
      // 'lmkd' is not really required because the code in lmkd.c emits events
      // with ATRACE_TAG_ALWAYS. We need something just to ensure that the final
      // config will enable atrace userspace events.
      tc.addAtraceApps('lmkd');
    },
  };
}

function polledProcStats(): RecordProbe {
  const settings = {
    pollMs: new Slider(POLL_INTERVAL_SLIDER),
    procAge: new Toggle({title: 'Record process age'}),
    procRuntime: new Toggle({title: 'Record process runtime'}),
  };
  return {
    id: 'mem_proc_stat',
    title: 'Per process /proc/ stat polling',
    image: 'rec_ps_stats.png',
    dependencies: [ADV_PROC_ASSOC_PROBE_ID],
    description:
      'Periodically samples all processes in the system tracking: ' +
      'their thread list, memory counters (RSS, swap and other ' +
      '/proc/status counters) and oom_score_adj.',
    supportedPlatforms: ['ANDROID', 'LINUX', 'CHROME_OS'],
    settings,
    genConfig: function (tc: TraceConfigBuilder) {
      const ds = tc.addDataSource(PROC_STATS_DS_NAME, ADV_PROC_ASSOC_BUF_ID);
      // Because of the dependency on ADV_PROC_ASSOC_PROBE_ID, we expect
      // procThreadAssociation() to create the config first.
      const cfg = assertExists(ds.processStatsConfig);
      cfg.procStatsPollMs = settings.pollMs.value || undefined;
      cfg.recordProcessAge = settings.procAge.enabled || undefined;
      cfg.recordProcessRuntime = settings.procRuntime.enabled || undefined;
    },
  };
}

const SAMPLING_TIMES_MS = [
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

const SMB_VALUES_KB = [
  1024,
  2 * 1024,
  4 * 1024,
  8 * 1024,
  16 * 1024,
  32 * 1024,
  64 * 1024,
  128 * 1024,
];

function extractCmdlinesAndPids(text: string): [string[], number[]] {
  const cmdlines = [];
  const pids = [];
  for (const line of splitLinesNonEmpty(text)) {
    const num = parseInt(line);
    if (isNaN(num)) {
      cmdlines.push(line);
    } else {
      pids.push(num);
    }
  }
  return [cmdlines, pids];
}
