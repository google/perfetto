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
import {StandardGroup} from '../dev.perfetto.StandardGroups';
import {Anchor} from '../../widgets/anchor';
import {Icons} from '../../base/semantic_icons';

export interface SliceTrackGroupSchema {
  name: string;
  expanded?: true;
}

type DescriptionRenderer = () => m.Children;

interface SliceTrackTypeSchema {
  readonly type: string;
  readonly group: string | SliceTrackGroupSchema | undefined;
  readonly topLevelGroup: 'PROCESS' | 'THREAD' | StandardGroup | undefined;

  /**
   * Optional function to customize the display name of the track.
   *
   * This function is called during track registration to transform the raw
   * track name into a more user-friendly display name.
   *
   * @param trackName - The name of the track as inferred by the UI.
   * @returns The transformed display name to show in the UI.
   *
   * @example
   * ```typescript
   * displayName: (name) =>`${name} (Custom)`
   * ```
   */
  readonly displayName?: (trackName: string) => string;

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

export const SLICE_TRACK_SCHEMAS: ReadonlyArray<SliceTrackTypeSchema> = [
  {
    type: 'battery_stats',
    topLevelGroup: 'POWER',
    group: 'Battery Stats',
  },
  {
    type: 'bluetooth_trace_event',
    topLevelGroup: 'SYSTEM',
    group: 'Bluetooth',
  },
  {
    type: 'app_wakelock_events',
    topLevelGroup: 'POWER',
    group: 'App Wakelocks',
  },
  {
    type: 'legacy_async_process_slice',
    topLevelGroup: 'PROCESS',
    group: undefined,
  },
  {
    type: 'legacy_async_global_slice',
    topLevelGroup: undefined,
    group: 'Global Legacy Events',
  },
  {
    type: 'legacy_chrome_global_instants',
    group: undefined,
    topLevelGroup: undefined,
  },
  {
    type: 'android_device_state',
    topLevelGroup: 'SYSTEM',
    group: undefined,
  },
  {
    type: 'android_lmk',
    topLevelGroup: 'PROCESS',
    group: undefined,
  },
  {
    type: 'chrome_process_instant',
    topLevelGroup: 'PROCESS',
    group: undefined,
  },
  {
    type: 'drm_vblank',
    topLevelGroup: 'HARDWARE',
    group: 'DRM VBlank',
  },
  {
    type: 'disp_dpu_underrun',
    topLevelGroup: 'HARDWARE',
    group: 'Display',
  },
  {
    type: 'disp_vblank_irq_enable',
    topLevelGroup: 'HARDWARE',
    group: 'Display',
  },
  {
    type: 'drm_sched_ring',
    topLevelGroup: 'HARDWARE',
    group: 'DRM Sched Ring',
  },
  {
    type: 'drm_fence',
    topLevelGroup: 'HARDWARE',
    group: 'DRM Fence',
  },
  {
    type: 'interconnect_events',
    topLevelGroup: 'HARDWARE',
    group: undefined,
  },
  {
    type: 'cpu_irq',
    topLevelGroup: 'CPU',
    group: 'IRQs',
  },
  {
    type: 'cpu_softirq',
    topLevelGroup: 'CPU',
    group: 'Softirqs',
  },
  {
    type: 'cpu_hrtimer',
    topLevelGroup: 'CPU',
    group: 'HRTimers',
  },
  {
    type: 'net_socket_set_state',
    topLevelGroup: 'NETWORK',
    group: 'Socket Set State',
  },
  {
    type: 'net_tcp_retransmit_skb',
    topLevelGroup: 'NETWORK',
    group: 'TCP Retransmit SKB',
  },
  {
    type: 'cpu_napi_gro',
    topLevelGroup: 'CPU',
    group: 'NAPI GRO',
  },
  {
    type: 'ufs_command_tag',
    topLevelGroup: 'IO',
    group: 'UFS Command Tag',
  },
  {
    type: 'wakesource_wakelock',
    topLevelGroup: 'POWER',
    group: 'Kernel Wakelocks',
  },
  {
    type: 'dumpstate_wakelocks',
    topLevelGroup: 'POWER',
    group: 'Kernel Wakelocks',
  },
  {
    type: 'cpu_funcgraph',
    topLevelGroup: 'CPU',
    group: 'Funcgraph',
  },
  {
    type: 'android_ion_allocations',
    topLevelGroup: 'MEMORY',
    group: 'ION',
  },
  {
    type: 'android_fs',
    topLevelGroup: 'IO',
    group: undefined,
  },
  {
    type: 'cpu_mali_irq',
    topLevelGroup: 'CPU',
    group: undefined,
  },
  {
    type: 'mali_mcu_state',
    topLevelGroup: 'GPU',
    group: undefined,
  },
  {
    type: 'pkvm_hypervisor',
    topLevelGroup: 'HYPERVISOR',
    group: undefined,
  },
  {
    type: 'virtgpu_queue_event',
    topLevelGroup: 'GPU',
    group: 'Virtio GPU Events',
  },
  {
    type: 'virtio_video_queue_event',
    topLevelGroup: 'SYSTEM',
    group: 'Virtio Video Queue Events',
  },
  {
    type: 'virtio_video_command',
    topLevelGroup: 'SYSTEM',
    group: 'Virtio Video Command Events',
  },
  {
    type: 'android_camera_event',
    topLevelGroup: 'HARDWARE',
    group: undefined,
  },
  {
    type: 'gpu_render_stage',
    topLevelGroup: 'GPU',
    group: 'Render Stage',
  },
  {
    type: 'vulkan_events',
    topLevelGroup: 'GPU',
    group: undefined,
  },
  {
    type: 'gpu_log',
    topLevelGroup: 'GPU',
    group: undefined,
  },
  {
    type: 'graphics_frame_event',
    topLevelGroup: 'GPU',
    group: undefined,
  },
  {
    type: 'triggers',
    topLevelGroup: 'SYSTEM',
    group: undefined,
  },
  {
    type: 'network_packets',
    topLevelGroup: 'NETWORK',
    group: undefined,
  },
  {
    type: 'pixel_modem_event',
    topLevelGroup: 'HARDWARE',
    group: undefined,
  },
  {
    type: 'statsd_atoms',
    topLevelGroup: 'SYSTEM',
    group: undefined,
  },
  {
    type: 'atrace_async_slice',
    topLevelGroup: 'PROCESS',
    group: undefined,
  },
  {
    type: 'atrace_async_slice_for_track',
    topLevelGroup: 'PROCESS',
    group: undefined,
  },
  {
    type: 'thread_execution',
    topLevelGroup: 'THREAD',
    group: undefined,
    description: () => {
      return () =>
        m(
          'p',
          `Shows general thread execution instrumentation from various sources
           (e.g. atrace, track event, syscall) all appearing on a single
           timeline.`,
        );
    },
  },
  {
    type: 'thread_funcgraph',
    topLevelGroup: 'THREAD',
    group: undefined,
    displayName: (trackName) => `${trackName} (funcgraph)`,
  },
  {
    type: 'art_method_tracing',
    topLevelGroup: 'THREAD',
    group: undefined,
    displayName: (trackName) => `${trackName} (ART)`,
    description: ({description}) => {
      return () =>
        m('div', [
          m(
            'p',
            description ??
              'Shows ART (Android Runtime) method entry and exit events.',
          ),
          m(
            'p',
            `These represent Java/Kotlin method calls traced at the runtime
             level. Due to the performace impact of method tracing, it's very
             likely the performance shown here is signifcantly different to
             performance when method tracing is turned off.`,
          ),
          m('br'),
          m(
            Anchor,
            {
              href: 'https://developer.android.com/reference/android/os/Debug#startMethodTracing()',
              target: '_blank',
              icon: Icons.ExternalLink,
            },
            'Documentation',
          ),
        ]);
    },
  },
  {
    type: 'etw_fileio',
    topLevelGroup: 'IO',
    group: undefined,
  },
];
