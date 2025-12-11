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

import {RecordSubpage, RecordProbe} from '../config/config_interfaces';
import {FTRACE_DS, TraceConfigBuilder} from '../config/trace_config_builder';
import {TypedMultiselect} from './widgets/multiselect';
import {Slider} from './widgets/slider';
import {Toggle} from './widgets/toggle';

export const ADV_PROC_ASSOC_PROBE_ID = 'adv_proc_thread_assoc';
export const ADV_PROC_ASSOC_BUF_ID = 'proc_assoc';
export const PROC_STATS_DS_NAME = 'linux.process_stats';
export const ADV_FTRACE_PROBE_ID = 'advanced_ftrace';

export function advancedRecordSection(): RecordSubpage {
  return {
    kind: 'PROBES_PAGE',
    id: 'advanced',
    title: 'Advanced settings',
    subtitle: 'For ftrace wizards',
    icon: 'settings',
    probes: [ftraceCfg(), procThreadAssociation()],
  };
}

function ftraceCfg(): RecordProbe {
  const settings = {
    ksyms: new Toggle({
      title: 'Resolve kernel symbols',
      default: true,
      descr:
        'Enables lookup via /proc/kallsyms for workqueue, ' +
        'sched_blocked_reason and other events ' +
        '(userdebug/eng builds only).',
    }),
    genericEvents: new Toggle({
      title: 'Enable generic events (slow)',
      descr:
        'Enables capture of ftrace events that are not known at build time ' +
        'by perfetto as key-value string pairs. This is slow and expensive.',
    }),
    bufSize: new Slider({
      title: 'Buf size',
      cssClass: '.thin',
      values: [0, 512, 1024, 2 * 1024, 4 * 1024, 16 * 1024, 32 * 1024],
      unit: 'KB',
      zeroIsDefault: true,
    }),
    drainRate: new Slider({
      title: 'trace_pipe_raw read interval',
      cssClass: '.thin',
      values: [0, 100, 250, 500, 1000, 2500, 5000],
      unit: 'ms',
      zeroIsDefault: true,
    }),
    groups: new TypedMultiselect<string>({
      title: 'Event groups',
      options: new Map(
        Object.entries({
          binder: 'binder/*',
          block: 'block/*',
          clk: 'clk/*',
          devfreq: 'devfreq/*',
          ext4: 'ext4/*',
          f2fs: 'f2fs/*',
          i2c: 'i2c/*',
          irq: 'irq/*',
          kmem: 'kmem/*',
          memory_bus: 'memory_bus/*',
          mmc: 'mmc/*',
          oom: 'oom/*',
          power: 'power/*',
          regulator: 'regulator/*',
          sched: 'sched/*',
          sync: 'sync/*',
          task: 'task/*',
          vmscan: 'vmscan/*',
          fastrpc: 'fastrpc/*',
        }),
      ),
    }),
  };
  return {
    id: ADV_FTRACE_PROBE_ID,
    title: 'Advanced ftrace config',
    image: 'rec_ftrace.png',
    description:
      'Enable individual events and tune the kernel-tracing (ftrace) ' +
      'module. The events enabled here are in addition to those from ' +
      'enabled by other probes.',
    supportedPlatforms: ['ANDROID', 'CHROME_OS', 'LINUX'],
    settings,
    genConfig: function (tc: TraceConfigBuilder) {
      const ds = tc.addDataSource(FTRACE_DS);
      const cfg = (ds.ftraceConfig ??= {});
      cfg.bufferSizeKb = settings.bufSize.value || undefined;
      cfg.drainPeriodMs = settings.drainRate.value || undefined;
      cfg.symbolizeKsyms = settings.ksyms.enabled ? true : undefined;
      cfg.disableGenericEvents = !settings.genericEvents.enabled;
      cfg.ftraceEvents ??= [];
      cfg.ftraceEvents.push(...settings.groups.selectedValues());
    },
  };
}

function procThreadAssociation(): RecordProbe {
  const ftraceEvents = [
    'sched/sched_process_exit',
    'sched/sched_process_free',
    'task/task_newtask',
    'task/task_rename',
  ];
  const settings = {
    initialScan: new Toggle({
      title: 'Scan all processes at startup',
      descr: 'Reports all /proc/* processes when starting',
      default: true,
    }),
  };
  return {
    id: ADV_PROC_ASSOC_PROBE_ID,
    title: 'Process<>thread association',
    description:
      'A union of ftrace events and /proc scrapers to capture thread<>process' +
      'associations as soon as they are seen from the cpu_pipe_raw. This is ' +
      'to capture the information about the whole process (e.g., cmdline).',
    supportedPlatforms: ['ANDROID', 'CHROME_OS', 'LINUX'],
    settings,
    genConfig: function (tc: TraceConfigBuilder) {
      tc.addFtraceEvents(...ftraceEvents);
      // Set to 1/16th of the main buffer size, with reasonable limits.
      const minMax = [256, 8 * 1024];
      const bufSizeKb = Math.min(
        Math.max(tc.defaultBuffer.sizeKb / 16, minMax[0]),
        minMax[1],
      );
      tc.addBuffer(ADV_PROC_ASSOC_BUF_ID, bufSizeKb);

      const ds = tc.addDataSource(PROC_STATS_DS_NAME, ADV_PROC_ASSOC_BUF_ID);
      const cfg = (ds.processStatsConfig ??= {});
      cfg.scanAllProcessesOnStart = settings.initialScan.enabled || undefined;
    },
  };
}
