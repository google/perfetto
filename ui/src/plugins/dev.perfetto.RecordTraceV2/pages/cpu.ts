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

import protos from '../../../protos';
import {ADV_FTRACE_PROBE_ID, ADV_PROC_ASSOC_PROBE_ID} from './advanced';
import {RecordSubpage, RecordProbe} from '../config/config_interfaces';
import {TraceConfigBuilder} from '../config/trace_config_builder';
import {POLL_INTERVAL_SLIDER, Slider} from './widgets/slider';

const PROC_POLL_DS = 'linux.sys_stats';

export function cpuRecordSection(): RecordSubpage {
  return {
    kind: 'PROBES_PAGE',
    id: 'cpu',
    title: 'CPU',
    subtitle: 'CPU usage, scheduling, wakeups',
    icon: 'subtitles',
    probes: [cpuUsage(), sched(), cpuFreq(), syscalls()],
  };
}

function cpuUsage(): RecordProbe {
  const settings = {pollMs: new Slider(POLL_INTERVAL_SLIDER)};
  return {
    id: 'cpu_usage',
    image: 'rec_cpu_coarse.png',
    title: 'Coarse CPU usage counter',
    supportedPlatforms: ['ANDROID', 'LINUX', 'CHROME_OS'],
    description:
      'Lightweight polling of CPU usage counters via /proc/stat. ' +
      'Allows to periodically monitor CPU usage.',
    dependencies: [ADV_PROC_ASSOC_PROBE_ID],
    settings,
    genConfig: function (tc: TraceConfigBuilder) {
      const cfg = tc.addDataSource(PROC_POLL_DS);
      cfg.sysStatsConfig ??= {};
      cfg.sysStatsConfig.statPeriodMs = settings.pollMs.value;
      cfg.sysStatsConfig.statCounters ??= [];
      cfg.sysStatsConfig.statCounters.push(
        protos.SysStatsConfig.StatCounters.STAT_CPU_TIMES,
        protos.SysStatsConfig.StatCounters.STAT_FORK_COUNT,
      );
    },
  };
}

function sched(): RecordProbe {
  return {
    id: 'cpu_sched',
    image: 'rec_cpu_fine.png',
    title: 'Scheduling details',
    supportedPlatforms: ['ANDROID', 'LINUX', 'CHROME_OS'],
    dependencies: [ADV_FTRACE_PROBE_ID, ADV_PROC_ASSOC_PROBE_ID],
    description: 'Enables high-detailed tracking of scheduling events',
    genConfig: function (tc: TraceConfigBuilder) {
      tc.addFtraceEvents(
        'sched/sched_switch',
        'power/suspend_resume',
        'sched/sched_blocked_reason',
        'sched/sched_wakeup',
        'sched/sched_wakeup_new',
        'sched/sched_waking',
        'sched/sched_process_exit',
        'sched/sched_process_free',
        'task/task_newtask',
        'task/task_rename',
      );
    },
  };
}

function cpuFreq(): RecordProbe {
  const settings = {pollMs: new Slider(POLL_INTERVAL_SLIDER)};
  return {
    id: 'cpu_freq',
    image: 'rec_cpu_freq.png',
    title: 'CPU frequency and idle states',
    description:
      'Records cpu frequency and idle state changes via ftrace and sysfs',
    supportedPlatforms: ['ANDROID', 'LINUX', 'CHROME_OS'],
    settings,
    genConfig: function (tc: TraceConfigBuilder) {
      const cfg = tc.addDataSource(PROC_POLL_DS);
      cfg.sysStatsConfig ??= {};
      cfg.sysStatsConfig.cpufreqPeriodMs = settings.pollMs.value;
      tc.addFtraceEvents(
        'power/cpu_frequency',
        'power/cpu_idle',
        'power/suspend_resume',
      );
    },
  };
}

function syscalls(): RecordProbe {
  return {
    id: 'cpu_syscalls',
    image: 'rec_syscalls.png',
    title: 'Syscalls',
    description:
      'Tracks the enter and exit of all syscalls. On Android' +
      'requires a userdebug or eng build.',
    supportedPlatforms: ['ANDROID', 'LINUX', 'CHROME_OS'],
    genConfig: function (tc: TraceConfigBuilder) {
      tc.addFtraceEvents(
        'raw_syscalls/sys_enter', //
        'raw_syscalls/sys_exit', //
      );
    },
  };
}
