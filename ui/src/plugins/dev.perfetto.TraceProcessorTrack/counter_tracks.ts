// Copyright (C) 2025 The Android Open Source Project
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
import {CounterOptions} from '../../components/tracks/base_counter_track';
import {TopLevelTrackGroup, TrackGroupSchema} from './types';

type CounterMode = CounterOptions['yMode'];

type DescriptionRenderer = () => m.Children;

interface CounterTrackTypeSchema {
  readonly type: string;
  readonly topLevelGroup: TopLevelTrackGroup;
  readonly group: string | TrackGroupSchema | undefined;
  readonly shareYAxis?: true;
  readonly mode?: CounterMode;

  /**
   * Optional function to provide a rich description renderer for the track.
   *
   * This function is called during track registration to generate custom
   * descriptive content that will be displayed to users. The function receives
   * the track name as input and returns a Mithril render function that produces
   * the actual description content.
   *
   * If the track has a description in the trace, that will be used
   * automatically so you don't need to define one here.
   *
   * @param trackDetails.name - The raw name of the track from the trace.
   * @param trackDetails.description - The description from the trace, if
   * available.
   * @returns A Mithril render function that produces the description content
   *
   * @example
   * ```typescript
   * description: ({name}) => () => m('span', `Custom description for ${name}`)
   * ```
   */
  readonly description?: (trackDetails: {
    readonly name?: string;
    readonly description?: string;
  }) => DescriptionRenderer | undefined;
}

export const COUNTER_TRACK_SCHEMAS: ReadonlyArray<CounterTrackTypeSchema> = [
  {
    type: 'acpm_cooling_device_counter',
    topLevelGroup: 'THERMALS',
    group: 'ACPM Cooling Devices',
  },
  {
    type: 'acpm_thermal_temperature',
    topLevelGroup: 'THERMALS',
    group: 'ACPM Temperature',
  },
  {
    type: 'android_energy_estimation_breakdown_per_uid',
    topLevelGroup: 'POWER',
    group: 'Android Energy Estimates (per uid)',
  },
  {
    type: 'android_energy_estimation_breakdown',
    topLevelGroup: 'POWER',
    group: 'Android Energy Estimates',
  },
  {
    type: 'atrace_counter',
    topLevelGroup: 'PROCESS',
    group: undefined,
  },
  {
    type: 'battery_counter',
    topLevelGroup: 'POWER',
    group: 'Battery Counters',
  },
  {
    type: 'battery_stats',
    topLevelGroup: 'POWER',
    group: 'Battery Stats',
  },
  {
    type: 'bcl_irq',
    topLevelGroup: undefined,
    group: 'BCL IRQ',
  },
  {
    type: 'block_io',
    topLevelGroup: 'IO',
    group: 'Block IO',
  },
  {
    type: 'buddyinfo',
    topLevelGroup: 'MEMORY',
    group: 'Buddyinfo',
  },
  {
    type: 'chrome_process_stats',
    topLevelGroup: 'PROCESS',
    group: undefined,
  },
  {
    type: 'clock_frequency',
    topLevelGroup: 'HARDWARE',
    group: 'Clock Frequency',
  },
  {
    type: 'clock_state',
    topLevelGroup: 'HARDWARE',
    group: 'Clock State',
  },
  {
    type: 'cooling_device_counter',
    topLevelGroup: 'THERMALS',
    group: 'Cooling Devices',
  },
  {
    type: 'cpu_capacity',
    topLevelGroup: 'CPU',
    group: 'CPU Capacity',
  },
  {
    type: 'cpu_frequency_throttle',
    topLevelGroup: 'CPU',
    group: 'CPU Frequency Throttling',
  },
  {
    type: 'cpu_max_frequency_limit',
    topLevelGroup: 'CPU',
    group: 'CPU Max Frequency',
  },
  {
    type: 'cpu_min_frequency_limit',
    topLevelGroup: 'CPU',
    group: 'CPU Min Frequency',
  },
  {
    type: 'cpu_nr_running',
    topLevelGroup: 'CPU',
    group: 'CPU Number Running',
  },
  {
    type: 'cpu_utilization',
    topLevelGroup: 'CPU',
    group: 'CPU Utilization',
  },
  {
    type: 'cpustat',
    topLevelGroup: 'CPU',
    group: 'CPU Stat',
  },
  {
    type: 'cros_ec_sensorhub_data',
    topLevelGroup: 'HARDWARE',
    group: 'ChromeOS EC Sensorhub',
  },
  {
    type: 'diskstat',
    topLevelGroup: 'IO',
    group: 'Diskstat',
  },
  {
    type: 'etw_meminfo',
    topLevelGroup: 'MEMORY',
    group: 'ETW Memory Counters',
  },
  {
    type: 'f2fs_iostat_latency',
    topLevelGroup: 'IO',
    group: 'F2FS IOStat Latency',
  },
  {
    type: 'f2fs_iostat',
    topLevelGroup: 'IO',
    group: 'F2FS IOStat',
  },
  {
    type: 'fastrpc_change',
    topLevelGroup: 'PROCESS',
    group: 'Fastrpc',
  },
  {
    type: 'fastrpc',
    topLevelGroup: 'HARDWARE',
    group: 'Fastrpc',
  },
  {
    type: 'fuchsia_counter',
    topLevelGroup: 'PROCESS',
    group: undefined,
  },
  {
    type: 'gpu_counter',
    topLevelGroup: 'GPU',
    group: 'GPU Counters',
  },
  {
    type: 'gpu_memory',
    topLevelGroup: 'GPU',
    group: undefined,
  },
  {
    type: 'ion_change',
    topLevelGroup: 'THREAD',
    group: undefined,
  },
  {
    type: 'ion',
    topLevelGroup: 'MEMORY',
    group: undefined,
  },
  {
    type: 'json_counter_thread_fallback',
    topLevelGroup: 'THREAD',
    group: undefined,
  },
  {
    type: 'json_counter',
    topLevelGroup: 'PROCESS',
    group: undefined,
  },
  {
    type: 'linux_device_frequency',
    topLevelGroup: 'HARDWARE',
    group: 'Linux Device Frequency',
  },
  {
    type: 'linux_rpm',
    topLevelGroup: 'HARDWARE',
    group: 'Linux RPM',
  },
  {
    type: 'meminfo',
    topLevelGroup: 'MEMORY',
    group: 'Meminfo',
  },
  {
    type: 'metatrace_counter',
    topLevelGroup: 'THREAD',
    group: undefined,
  },
  {
    type: 'mm_event_thread_fallback',
    topLevelGroup: 'THREAD',
    group: 'MM Event',
  },
  {
    type: 'mm_event',
    topLevelGroup: 'PROCESS',
    group: 'MM Event',
  },
  {
    type: 'net_kfree_skb',
    topLevelGroup: 'NETWORK',
    group: 'Network Packet Frees',
  },
  {
    type: 'net_receive',
    topLevelGroup: 'NETWORK',
    group: 'Network Receive',
    mode: 'rate',
  },
  {
    type: 'net_transmit',
    topLevelGroup: 'NETWORK',
    group: 'Network Send',
    mode: 'rate',
  },
  {
    type: 'num_forks',
    topLevelGroup: 'SYSTEM',
    group: undefined,
  },
  {
    type: 'num_irq_total',
    topLevelGroup: 'SYSTEM',
    group: undefined,
  },
  {
    type: 'num_irq',
    topLevelGroup: 'SYSTEM',
    group: 'IRQ Count',
  },
  {
    type: 'num_softirq_total',
    topLevelGroup: 'SYSTEM',
    group: undefined,
  },
  {
    type: 'num_softirq',
    topLevelGroup: 'SYSTEM',
    group: 'Softirq Count',
  },
  {
    type: 'oom_score_adj_thread_fallback',
    topLevelGroup: 'THREAD',
    group: undefined,
  },
  {
    type: 'oom_score_adj',
    topLevelGroup: 'PROCESS',
    group: undefined,
  },
  {
    type: 'pixel_cpm_counters',
    topLevelGroup: 'HARDWARE',
    group: 'CPM Counters',
  },
  {
    type: 'proc_stat_runtime',
    topLevelGroup: 'PROCESS',
    group: undefined,
  },
  {
    type: 'process_gpu_memory',
    topLevelGroup: 'PROCESS',
    group: undefined,
  },
  {
    type: 'process_memory_thread_fallback',
    topLevelGroup: 'THREAD',
    group: undefined,
  },
  {
    type: 'process_memory',
    topLevelGroup: 'PROCESS',
    group: undefined,
  },
  {
    type: 'psi',
    group: 'PSI',
    topLevelGroup: 'SYSTEM',
    mode: 'rate',
  },
  {
    type: 'screen_state',
    topLevelGroup: 'SYSTEM',
    group: 'Screen State',
  },
  {
    type: 'smaps',
    topLevelGroup: 'PROCESS',
    group: undefined,
  },
  {
    type: 'sysprop_counter',
    topLevelGroup: 'SYSTEM',
    group: undefined,
  },
  {
    type: 'thermal_temperature_sys',
    topLevelGroup: 'THERMALS',
    group: 'Temperature (/sys)',
  },
  {
    type: 'thermal_temperature',
    topLevelGroup: 'THERMALS',
    group: 'Temperature',
  },
  {
    type: 'ufs_clkgating',
    topLevelGroup: 'IO',
    group: undefined,
  },
  {
    type: 'ufs_command_count',
    topLevelGroup: 'IO',
    group: undefined,
  },
  {
    type: 'virtgpu_latency',
    topLevelGroup: 'GPU',
    group: 'Virtgpu Latency',
  },
  {
    type: 'virtgpu_num_free',
    topLevelGroup: 'GPU',
    group: 'Virtgpu num_free',
  },
  {
    type: 'vmstat',
    topLevelGroup: 'MEMORY',
    group: 'vmstat',
  },
  {
    type: 'vulkan_device_mem_allocation',
    topLevelGroup: 'GPU',
    group: 'Vulkan Allocations',
  },
  {
    type: 'vulkan_device_mem_bind',
    topLevelGroup: 'GPU',
    group: 'Vulkan Binds',
  },
  {
    type: 'vulkan_driver_mem',
    topLevelGroup: 'GPU',
    group: 'Vulkan Driver Memory',
  },
  {
    type: 'battery_status',
    topLevelGroup: 'POWER',
    group: undefined,
  },
  {
    type: 'battery_plugged_status',
    topLevelGroup: 'POWER',
    group: undefined,
  },
  {
    type: 'ion',
    topLevelGroup: 'MEMORY',
    group: undefined,
  },
  {
    type: 'ion_change',
    topLevelGroup: 'MEMORY',
    group: undefined,
  },
  {
    type: 'android_dma_heap_change',
    topLevelGroup: 'THREAD',
    group: undefined,
  },
  {
    type: 'pixel_fwtp_counters',
    topLevelGroup: 'HARDWARE',
    group: 'Pixel Firmware',
  },
];
