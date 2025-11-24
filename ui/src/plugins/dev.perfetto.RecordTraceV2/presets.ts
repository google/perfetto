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

const LOGCAT_DEFAULT = [
  protos.AndroidLogId.LID_DEFAULT,
  protos.AndroidLogId.LID_SYSTEM,
  protos.AndroidLogId.LID_CRASH,
  protos.AndroidLogId.LID_EVENTS,
];

// Android-specific presets
export const ANDROID_PRESETS: Preset[] = [
  {
    id: 'default',
    title: 'Default',
    subtitle: 'The default config for general purpose tracing',
    icon: 'auto_awesome',
    session: {
      mode: 'STOP_WHEN_FULL',
      bufSizeKb: 64 * 1024,
      durationMs: 10000,
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
            allApps: false,
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
      mode: 'STOP_WHEN_FULL',
      bufSizeKb: 64 * 1024,
      durationMs: 30000,
      maxFileSizeMb: 500,
      fileWritePeriodMs: 2500,
      compression: false,
      probes: {
        atrace: {
          settings: {
            categories: ATRACE_BATTERY,
            apps: '',
            allApps: false,
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
      mode: 'STOP_WHEN_FULL',
      bufSizeKb: 64 * 1024,
      durationMs: 30000,
      maxFileSizeMb: 500,
      fileWritePeriodMs: 2500,
      compression: false,
      probes: {
        cpu_sched: {settings: {}},
        atrace: {
          settings: {
            categories: ATRACE_THERMAL,
            apps: '',
            allApps: false,
          },
        },
        power_rails: {settings: {pollMs: 1000}},
        power_voltages: {settings: {}},
        cpu_usage: {settings: {pollMs: 1000}},
        cpu_freq: {settings: {pollMs: 1000}},
      },
    },
  },
];

// Linux-specific presets
export const LINUX_PRESETS: Preset[] = [
  {
    id: 'default',
    title: 'Default',
    subtitle: 'General purpose CPU and system tracing',
    icon: 'auto_awesome',
    session: {
      mode: 'STOP_WHEN_FULL',
      bufSizeKb: 64 * 1024,
      durationMs: 10000,
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
      mode: 'STOP_WHEN_FULL',
      bufSizeKb: 64 * 1024,
      durationMs: 10000,
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
      return [];
    default:
      return [];
  }
}
