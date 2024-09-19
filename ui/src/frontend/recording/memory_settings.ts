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

import m from 'mithril';
import {MeminfoCounters, VmstatCounters} from '../../protos';
import {globals} from '../globals';
import {
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
} from '../record_widgets';
import {POLL_INTERVAL_MS, RecordingSectionAttrs} from './recording_sections';

class HeapSettings implements m.ClassComponent<RecordingSectionAttrs> {
  view({attrs}: m.CVnode<RecordingSectionAttrs>) {
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
      `.${attrs.cssClass}`,
      m(Textarea, {
        title: 'Names or pids of the processes to track (required)',
        docsLink:
          'https://perfetto.dev/docs/data-sources/native-heap-profiler#heapprofd-targets',
        placeholder:
          'One per line, e.g.:\n' +
          'system_server\n' +
          'com.google.android.apps.photos\n' +
          '1503',
        set: (cfg, val) => (cfg.hpProcesses = val),
        get: (cfg) => cfg.hpProcesses,
      } as TextareaAttrs),
      m(Slider, {
        title: 'Sampling interval',
        cssClass: '.thin',
        values: [
          0, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192,
          16384, 32768, 65536, 131072, 262144, 524288, 1048576,
        ],
        unit: 'B',
        min: 0,
        set: (cfg, val) => (cfg.hpSamplingIntervalBytes = val),
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
          globals.state.recordConfig.hpContinuousDumpsInterval === 0
            ? '.greyed-out'
            : ''
        }`,
        values: valuesForMS,
        unit: 'ms',
        min: 0,
        disabled: globals.state.recordConfig.hpContinuousDumpsInterval === 0,
        set: (cfg, val) => (cfg.hpContinuousDumpsPhase = val),
        get: (cfg) => cfg.hpContinuousDumpsPhase,
      } as SliderAttrs),
      m(Slider, {
        title: `Shared memory buffer`,
        cssClass: '.thin',
        values: valuesForShMemBuff.filter(
          (value) => value === 0 || (value >= 8192 && value % 4096 === 0),
        ),
        unit: 'B',
        min: 0,
        set: (cfg, val) => (cfg.hpSharedMemoryBuffer = val),
        get: (cfg) => cfg.hpSharedMemoryBuffer,
      } as SliderAttrs),
      m(Toggle, {
        title: 'Block client',
        cssClass: '.thin',
        descr: `Slow down target application if profiler cannot keep up.`,
        setEnabled: (cfg, val) => (cfg.hpBlockClient = val),
        isEnabled: (cfg) => cfg.hpBlockClient,
      } as ToggleAttrs),
      m(Toggle, {
        title: 'All custom allocators (Q+)',
        cssClass: '.thin',
        descr: `If the target application exposes custom allocators, also
sample from those.`,
        setEnabled: (cfg, val) => (cfg.hpAllHeaps = val),
        isEnabled: (cfg) => cfg.hpAllHeaps,
      } as ToggleAttrs),
      // TODO(hjd): Add advanced options.
    );
  }
}

class JavaHeapDumpSettings implements m.ClassComponent<RecordingSectionAttrs> {
  view({attrs}: m.CVnode<RecordingSectionAttrs>) {
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
      `.${attrs.cssClass}`,
      m(Textarea, {
        title: 'Names or pids of the processes to track (required)',
        placeholder: 'One per line, e.g.:\n' + 'com.android.vending\n' + '1503',
        set: (cfg, val) => (cfg.jpProcesses = val),
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
          globals.state.recordConfig.jpContinuousDumpsInterval === 0
            ? '.greyed-out'
            : ''
        }`,
        values: valuesForMS,
        unit: 'ms',
        min: 0,
        disabled: globals.state.recordConfig.jpContinuousDumpsInterval === 0,
        set: (cfg, val) => (cfg.jpContinuousDumpsPhase = val),
        get: (cfg) => cfg.jpContinuousDumpsPhase,
      } as SliderAttrs),
    );
  }
}

export class MemorySettings implements m.ClassComponent<RecordingSectionAttrs> {
  view({attrs}: m.CVnode<RecordingSectionAttrs>) {
    const meminfoOpts = new Map<string, string>();
    for (const x in MeminfoCounters) {
      if (
        typeof MeminfoCounters[x] === 'number' &&
        !`${x}`.endsWith('_UNSPECIFIED')
      ) {
        meminfoOpts.set(x, x.replace('MEMINFO_', '').toLowerCase());
      }
    }
    const vmstatOpts = new Map<string, string>();
    for (const x in VmstatCounters) {
      if (
        typeof VmstatCounters[x] === 'number' &&
        !`${x}`.endsWith('_UNSPECIFIED')
      ) {
        vmstatOpts.set(x, x.replace('VMSTAT_', '').toLowerCase());
      }
    }
    return m(
      `.record-section${attrs.cssClass}`,
      m(
        Probe,
        {
          title: 'Native heap profiling',
          img: 'rec_native_heap_profiler.png',
          descr: `Track native heap allocations & deallocations of an Android
               process. (Available on Android 10+)`,
          setEnabled: (cfg, val) => (cfg.heapProfiling = val),
          isEnabled: (cfg) => cfg.heapProfiling,
        } as ProbeAttrs,
        m(HeapSettings, attrs),
      ),
      m(
        Probe,
        {
          title: 'Java heap dumps',
          img: 'rec_java_heap_dump.png',
          descr: `Dump information about the Java object graph of an
          Android app. (Available on Android 11+)`,
          setEnabled: (cfg, val) => (cfg.javaHeapDump = val),
          isEnabled: (cfg) => cfg.javaHeapDump,
        } as ProbeAttrs,
        m(JavaHeapDumpSettings, attrs),
      ),
      m(
        Probe,
        {
          title: 'Kernel meminfo',
          img: 'rec_meminfo.png',
          descr: 'Polling of /proc/meminfo',
          setEnabled: (cfg, val) => (cfg.meminfo = val),
          isEnabled: (cfg) => cfg.meminfo,
        } as ProbeAttrs,
        m(Slider, {
          title: 'Poll interval',
          cssClass: '.thin',
          values: POLL_INTERVAL_MS,
          unit: 'ms',
          set: (cfg, val) => (cfg.meminfoPeriodMs = val),
          get: (cfg) => cfg.meminfoPeriodMs,
        } as SliderAttrs),
        m(Dropdown, {
          title: 'Select counters',
          cssClass: '.multicolumn',
          options: meminfoOpts,
          set: (cfg, val) => (cfg.meminfoCounters = val),
          get: (cfg) => cfg.meminfoCounters,
        } as DropdownAttrs),
      ),
      m(Probe, {
        title: 'High-frequency memory events',
        img: 'rec_mem_hifreq.png',
        descr: `Allows to track short memory spikes and transitories through
                ftrace's mm_event, rss_stat and ion events. Available only
                on recent Android Q+ kernels`,
        setEnabled: (cfg, val) => (cfg.memHiFreq = val),
        isEnabled: (cfg) => cfg.memHiFreq,
      } as ProbeAttrs),
      m(Probe, {
        title: 'Low memory killer',
        img: 'rec_lmk.png',
        descr: `Record LMK events. Works both with the old in-kernel LMK
                and the newer userspace lmkd. It also tracks OOM score
                adjustments.`,
        setEnabled: (cfg, val) => (cfg.memLmk = val),
        isEnabled: (cfg) => cfg.memLmk,
      } as ProbeAttrs),
      m(
        Probe,
        {
          title: 'Per process stats',
          img: 'rec_ps_stats.png',
          descr: `Periodically samples all processes in the system tracking:
                    their thread list, memory counters (RSS, swap and other
                    /proc/status counters) and oom_score_adj.`,
          setEnabled: (cfg, val) => (cfg.procStats = val),
          isEnabled: (cfg) => cfg.procStats,
        } as ProbeAttrs,
        m(Slider, {
          title: 'Poll interval',
          cssClass: '.thin',
          values: POLL_INTERVAL_MS,
          unit: 'ms',
          set: (cfg, val) => (cfg.procStatsPeriodMs = val),
          get: (cfg) => cfg.procStatsPeriodMs,
        } as SliderAttrs),
      ),
      m(
        Probe,
        {
          title: 'Virtual memory stats',
          img: 'rec_vmstat.png',
          descr: `Periodically polls virtual memory stats from /proc/vmstat.
                    Allows to gather statistics about swap, eviction,
                    compression and pagecache efficiency`,
          setEnabled: (cfg, val) => (cfg.vmstat = val),
          isEnabled: (cfg) => cfg.vmstat,
        } as ProbeAttrs,
        m(Slider, {
          title: 'Poll interval',
          cssClass: '.thin',
          values: POLL_INTERVAL_MS,
          unit: 'ms',
          set: (cfg, val) => (cfg.vmstatPeriodMs = val),
          get: (cfg) => cfg.vmstatPeriodMs,
        } as SliderAttrs),
        m(Dropdown, {
          title: 'Select counters',
          cssClass: '.multicolumn',
          options: vmstatOpts,
          set: (cfg, val) => (cfg.vmstatCounters = val),
          get: (cfg) => cfg.vmstatCounters,
        } as DropdownAttrs),
      ),
    );
  }
}
