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

import protos from '../../protos';
import {RecordSessionSchema} from './serialization_schema';

export interface Preset {
  readonly id: string;
  readonly title: string;
  readonly subtitle: string;
  readonly icon: string;
  readonly session: RecordSessionSchema;
}

const ATRACE_DEFAULT = [
  'aidl',
  'am',
  'binder_driver',
  'camera',
  'dalvik',
  'disk',
  'freq',
  'gfx',
  'hal',
  'idle',
  'input',
  'memory',
  'memreclaim',
  'network',
  'power',
  'res',
  'sched',
  'ss',
  'sync',
  'thermal',
  'view',
  'webview',
  'wm',
  'workq',
];

const ATRACE_BATTERY = [
  'aidl',
  'am',
  'binder_driver',
  'network',
  'nnapi',
  'pm',
  'power',
  'ss',
  'thermal',
  'wm',
];

const ATRACE_THERMAL = [
  'freq',
  'idle',
  'power',
  'sched',
  'thermal',
  'thermal_tj',
  'workq',
];

const ATRACE_GRAPHICS = [
  'aidl',
  'am',
  'binder_driver',
  'gfx',
  'hal',
  'idle',
  'input',
  'memory',
  'power',
  'sched',
  'thermal',
  'view',
  'webview',
  'wm',
  'workq',
];

const FTRACE_GRAPHICS = ['power'];

const LOGCAT_DEFAULT = [
  protos.AndroidLogId.LID_DEFAULT,
  protos.AndroidLogId.LID_SYSTEM,
  protos.AndroidLogId.LID_CRASH,
  protos.AndroidLogId.LID_EVENTS,
];

// Chrome-specific presets
const CHROME_DEFAULT_PRESET: Preset = {
  id: 'default',
  title: 'Default',
  subtitle: 'Common Chrome trace events',
  icon: 'public',
  session: {
    kind: 'probes',
    mode: 'STOP_WHEN_FULL',
    bufSizeKb: 256 * 1024,
    durationMs: 30_000,
    maxFileSizeMb: 500,
    fileWritePeriodMs: 2500,
    compression: false,
    probes: {
      chrome_tracing: {
        settings: {
          'Task Scheduling': true,
          'Javascript execution': true,
          'Web content rendering, layout and compositing': true,
          'UI rendering and surface compositing': true,
          'Input events': true,
          'Navigation and loading': true,
        },
      },
    },
  },
};
const CHROME_V8_PRESET: Preset = {
  id: 'v8',
  title: 'V8',
  subtitle: 'JavaScript, wasm & GC',
  icon: 'mode_fan',
  session: {
    kind: 'probes',
    mode: 'STOP_WHEN_FULL',
    bufSizeKb: 256 * 1024,
    durationMs: 30_000,
    maxFileSizeMb: 500,
    fileWritePeriodMs: 2500,
    compression: false,
    probes: {
      chrome_tracing: {
        settings: {
          'Task Scheduling': true,
          'Javascript execution': true,
          'Navigation and loading': true,
          'categories': [
            'v8',
            'v8.execute',
            'v8.wasm',
            'v8.memory',
            'blink.user_timing',
            'disabled-by-default-v8.gc',
            'disabled-by-default-v8.compile',
            'disabled-by-default-v8.cpu_profiler',
          ],
        },
      },
    },
  },
};

export const CHROME_PRESETS: Preset[] = [
  CHROME_DEFAULT_PRESET,
  CHROME_V8_PRESET,
];

// Android-specific presets
export const ANDROID_PRESETS: Preset[] = [
  {
    id: 'default',
    title: 'Default',
    subtitle: 'The default config for general purpose tracing',
    icon: 'auto_awesome',
    session: {
      kind: 'probes',
      mode: 'STOP_WHEN_FULL',
      bufSizeKb: 64 * 1024,
      durationMs: 10_000,
      maxFileSizeMb: 500,
      fileWritePeriodMs: 2500,
      compression: false,
      probes: {
        cpu_usage: {settings: {pollMs: 1000}},
        cpu_sched: {settings: {}},
        cpu_freq: {settings: {pollMs: 1000}},
        atrace: {
          settings: {
            categories: ATRACE_DEFAULT,
            apps: '',
            allApps: true,
          },
        },
        logcat: {
          settings: {
            buffers: LOGCAT_DEFAULT,
          },
        },
        android_frame_timeline: {settings: {}},
      },
    },
  },
  {
    id: 'battery',
    title: 'Battery',
    subtitle: 'Battery usage and power consumption',
    icon: 'battery_profile',
    session: {
      kind: 'probes',
      mode: 'STOP_WHEN_FULL',
      bufSizeKb: 64 * 1024,
      durationMs: 30_000,
      maxFileSizeMb: 500,
      fileWritePeriodMs: 2500,
      compression: false,
      probes: {
        atrace: {
          settings: {
            categories: ATRACE_BATTERY,
            apps: '',
            allApps: true,
          },
        },
        power_rails: {settings: {pollMs: 1000}},
        cpu_usage: {settings: {pollMs: 1000}},
      },
    },
  },
  {
    id: 'thermal',
    title: 'Thermal',
    subtitle: 'Thermal throttling and mitigation',
    icon: 'thermostat',
    session: {
      kind: 'probes',
      mode: 'STOP_WHEN_FULL',
      bufSizeKb: 64 * 1024,
      durationMs: 30_000,
      maxFileSizeMb: 500,
      fileWritePeriodMs: 2500,
      compression: false,
      probes: {
        cpu_sched: {settings: {}},
        atrace: {
          settings: {
            categories: ATRACE_THERMAL,
            apps: '',
            allApps: true,
          },
        },
        power_rails: {settings: {pollMs: 1000}},
        power_voltages: {settings: {}},
        cpu_usage: {settings: {pollMs: 1000}},
        cpu_freq: {settings: {pollMs: 1000}},
      },
    },
  },
  {
    id: 'graphics',
    title: 'Graphics',
    subtitle: 'Graphics pipeline and system compositor',
    icon: 'layers',
    session: {
      kind: 'probes',
      mode: 'STOP_WHEN_FULL',
      bufSizeKb: 64 * 1024,
      durationMs: 30000,
      maxFileSizeMb: 500,
      fileWritePeriodMs: 2500,
      compression: false,
      probes: {
        cpu_usage: {settings: {pollMs: 1000}},
        cpu_sched: {settings: {}},
        cpu_freq: {settings: {pollMs: 1000}},
        gpu_frequency: {settings: {}},
        gpu_memory: {settings: {}},
        gpu_work_period: {settings: {}},
        mem_proc_stat: {settings: {pollMs: 1000}},
        android_frame_timeline: {settings: {}},
        atrace: {
          settings: {
            categories: ATRACE_GRAPHICS,
            apps: '',
            allApps: true,
          },
        },
        advanced_ftrace: {settings: {groups: FTRACE_GRAPHICS}},
      },
    },
  },
  {...CHROME_DEFAULT_PRESET, id: 'chrome', title: 'Chrome'},
  CHROME_V8_PRESET,
];

// Linux-specific presets
export const LINUX_PRESETS: Preset[] = [
  {
    id: 'default',
    title: 'Default',
    subtitle: 'General purpose CPU and system tracing',
    icon: 'auto_awesome',
    session: {
      kind: 'probes',
      mode: 'STOP_WHEN_FULL',
      bufSizeKb: 64 * 1024,
      durationMs: 10_000,
      maxFileSizeMb: 500,
      fileWritePeriodMs: 2500,
      compression: false,
      probes: {
        cpu_usage: {settings: {pollMs: 1000}},
        cpu_sched: {settings: {}},
        cpu_freq: {settings: {pollMs: 1000}},
        process_stats: {settings: {pollMs: 1000}},
        sys_stats: {settings: {pollMs: 1000}},
      },
    },
  },
  {
    id: 'scheduling',
    title: 'Scheduling',
    subtitle: 'CPU scheduling and process activity',
    icon: 'schedule',
    session: {
      kind: 'probes',
      mode: 'STOP_WHEN_FULL',
      bufSizeKb: 64 * 1024,
      durationMs: 10_000,
      maxFileSizeMb: 500,
      fileWritePeriodMs: 2500,
      compression: false,
      probes: {
        cpu_sched: {settings: {}},
        cpu_freq: {settings: {pollMs: 100}},
        process_stats: {settings: {pollMs: 100}},
      },
    },
  },
];

// Legacy export for backward compatibility
export const PRESETS = ANDROID_PRESETS;

export function getPresetsForPlatform(platform: string): Preset[] {
  switch (platform) {
    case 'ANDROID':
      return ANDROID_PRESETS;
    case 'LINUX':
      return LINUX_PRESETS;
    case 'CHROME':
    case 'CHROME_OS':
      return CHROME_PRESETS;
    default:
      return [];
  }
}
