// Copyright (C) 2026 The Android Open Source Project
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

import type {SectionRegistry} from './index';

export function registerOccupancySection(reg: SectionRegistry): void {
  reg.registerSection({
    id: 'com.meta.GpuCompute.Section.Occupancy',
    title: 'Occupancy',
    order: 2,
    launchMetrics: [
      'occupancy_limit_blocks',
      'occupancy_limit_registers',
      'occupancy_limit_shared_mem',
      'occupancy_limit_warps',
      'occupancy_limit_barriers',
      'sm__maximum_warps_per_active_cycle_pct',
      'sm__maximum_warps_avg_per_active_cycle',
    ],
    counterMetrics: [
      'sm__warps_active.avg.pct_of_peak_sustained_active',
      'sm__warps_active.avg.per_cycle_active',
    ],
    analysisPrompt:
      `Occupancy is the ratio of active warps/wavefronts per compute unit to the maximum possible.\n` +
      `Higher occupancy helps hide memory latency but doesn't always mean better performance.\n` +
      `\n` +
      `Use the following rules to diagnose occupancy issues:\n` +
      `\n` +
      `Block/workgroup size rules:\n` +
      `- Block size should be a multiple of the warp/wavefront size (32 on NVIDIA, 64 on AMD)\n` +
      `- Very small block sizes limit parallelism within each compute unit\n` +
      `- Very large block sizes may limit occupancy due to register or shared memory pressure\n` +
      `\n` +
      `Grid size and waves rules:\n` +
      `- Waves per compute unit < 1 means the grid is too small to fill the GPU\n` +
      `- Waves per compute unit between 1 and 2 may cause tail effects where some units finish early\n` +
      `- Ideally waves per compute unit should be >= 4 for good load balancing\n` +
      `\n` +
      `When analyzing these metrics:\n` +
      `1. Identify the limiting factor for occupancy\n` +
      `2. Compare theoretical vs achieved occupancy to detect workload imbalance\n` +
      `3. Assess whether higher occupancy would benefit this kernel\n` +
      `4. Suggest specific resource reductions to improve occupancy if beneficial\n` +
      `\n` +
      `Provide concise, actionable analysis with specific recommendations.`,
    tables: [
      {
        description: (t) =>
          `Occupancy is the ratio of the number of active ${t.warp.plural} per ${t.sm.title} to the maximum number of possible active ${t.warp.plural}. Another way to view occupancy is the percentage of the hardware's ability to process ${t.warp.plural} that is actively in use. Higher occupancy does not always result in higher performance, however, low occupancy always reduces the ability to hide latencies, resulting in overall performance degradation. The launch configuration (${t.block.title} size, ${t.grid.title} size, resource usage) determines how many ${t.warp.plural} can be active simultaneously.`,
        rows: [
          {
            id: 'occupancy_limit_blocks',
            label: (t) => `${t.block.title} Limit ${t.sm.title}`,
            unit: (t) => `${t.block.name}`,
            importance: 'optional',
          },
          {
            id: 'occupancy_limit_registers',
            label: (t) => `${t.block.title} Limit Registers`,
            unit: (t) => `${t.block.name}`,
            importance: 'optional',
          },
          {
            id: 'occupancy_limit_shared_mem',
            label: (t) => `${t.block.title} Limit ${t.sharedMem.title}`,
            unit: (t) => `${t.block.name}`,
            importance: 'optional',
          },
          {
            id: 'occupancy_limit_warps',
            label: (t) => `${t.block.title} Limit ${t.warp.pluralTitle}`,
            unit: (t) => `${t.block.name}`,
            importance: 'optional',
          },
          {
            id: 'occupancy_limit_barriers',
            label: (t) => `${t.block.title} Limit Barriers`,
            unit: (t) => `${t.block.name}`,
            importance: 'optional',
          },
          {
            id: 'sm__maximum_warps_per_active_cycle_pct',
            label: () => 'Theoretical Occupancy',
            unit: () => '%',
            importance: 'required',
          },
          {
            id: 'sm__maximum_warps_avg_per_active_cycle',
            label: (t) =>
              `Theoretical Active ${t.warp.pluralTitle} per ${t.sm.title}`,
            unit: (t) => `${t.warp.name}`,
            importance: 'optional',
          },
          {
            id: 'sm__warps_active.avg.pct_of_peak_sustained_active',
            label: () => 'Achieved Occupancy',
            unit: () => '%',
            importance: 'required',
            aggregation: 'avg',
          },
          {
            id: 'sm__warps_active.avg.per_cycle_active',
            label: (t) =>
              `Achieved Active ${t.warp.pluralTitle} Per ${t.sm.title}`,
            unit: (t) => `${t.warp.name}/cycle`,
            importance: 'optional',
            aggregation: 'avg',
          },
        ],
      },
    ],
  });
}
