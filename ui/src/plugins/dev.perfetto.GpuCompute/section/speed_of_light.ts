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

import {registerSection, registerWellKnownMetric} from './index';

export function registerSpeedOfLightSection(): void {
  registerWellKnownMetric('duration', [
    'gpu__time_duration.sum',
    'GRBM_TIME_DUR_max',
  ]);
  registerWellKnownMetric('cycles', [
    'gpc__cycles_elapsed.max',
    'GRBM_GUI_ACTIVE_avr',
  ]);
  registerWellKnownMetric('frequency', [
    'gpc__cycles_elapsed.avg.per_second',
    'GRBM_GUI_ACTIVE_avr_per_second',
  ]);
  registerWellKnownMetric(
    'compute_throughput',
    'sm__throughput.avg.pct_of_peak_sustained_elapsed',
  );
  registerWellKnownMetric(
    'memory_throughput',
    'gpu__compute_memory_throughput.avg.pct_of_peak_sustained_elapsed',
  );

  registerSection({
    id: 'dev.perfetto.GpuCompute.Section.SpeedOfLight',
    title: 'Speed of Light Throughput',
    order: 0,
    launchMetrics: ['GRBM_GUI_ACTIVE_avr_per_second'],
    counterMetrics: [
      'dram__cycles_elapsed.avg.per_second',
      'gpc__cycles_elapsed.avg.per_second',
      'gpc__cycles_elapsed.max',
      'gpu__compute_memory_throughput.avg.pct_of_peak_sustained_elapsed',
      'gpu__dram_throughput.avg.pct_of_peak_sustained_elapsed',
      'gpu__time_duration.sum',
      'l1tex__throughput.avg.pct_of_peak_sustained_active',
      'lts__throughput.avg.pct_of_peak_sustained_elapsed',
      'sm__cycles_active.avg',
      'sm__throughput.avg.pct_of_peak_sustained_elapsed',
      'GRBM_TIME_DUR_max',
      'GRBM_GUI_ACTIVE_avr',
      'SQ_BUSY_CYCLES_avr',
    ],
    analysisPrompt:
      `These metrics provide a high-level overview of GPU compute activity and memory system utilization.\n` +
      `\n` +
      `When analyzing these metrics:\n` +
      `1. Determine overall GPU utilization from active vs total cycles\n` +
      `2. Compute L2 cache hit rate to assess memory hierarchy efficiency\n` +
      `3. Look at memory request volume to understand traffic patterns\n` +
      `4. Classify the kernel as compute-bound, memory-bound, or latency-bound\n` +
      `5. Compare achieved vs theoretical throughput to identify optimization opportunities\n` +
      `\n` +
      `Provide concise, actionable analysis with specific recommendations.`,
    tables: [
      {
        description: () =>
          `High-level overview of GPU compute activity and memory system utilization. Shows overall GPU busy cycles, cache hierarchy efficiency, and memory throughput to identify whether the kernel is compute-bound, memory-bound, or underutilizing resources.`,
        rows: [
          {
            id: 'dram__cycles_elapsed.avg.per_second',
            label: () => 'DRAM Frequency',
            unit: () => 'hz',
            importance: 'optional',
            aggregation: 'avg',
          },
          {
            id: 'gpc__cycles_elapsed.avg.per_second',
            label: (t) => `${t.sm.title} Frequency`,
            unit: () => 'hz',
            importance: 'optional',
            aggregation: 'avg',
          },
          {
            id: 'gpc__cycles_elapsed.max',
            label: () => 'Elapsed Cycles',
            unit: () => 'cycle',
            importance: 'optional',
            aggregation: 'sum',
          },
          {
            id: 'gpu__compute_memory_throughput.avg.pct_of_peak_sustained_elapsed',
            label: () => 'Memory Throughput',
            unit: () => '%',
            importance: 'required',
            aggregation: 'avg',
          },
          {
            id: 'gpu__dram_throughput.avg.pct_of_peak_sustained_elapsed',
            label: () => 'DRAM Throughput',
            unit: () => '%',
            importance: 'optional',
            aggregation: 'avg',
          },
          {
            id: 'gpu__time_duration.sum',
            label: () => 'Duration',
            unit: () => 'nsecond',
            importance: 'optional',
            aggregation: 'sum',
          },
          {
            id: 'l1tex__throughput.avg.pct_of_peak_sustained_active',
            label: () => 'L1 Cache Throughput',
            unit: () => '%',
            importance: 'optional',
            aggregation: 'avg',
          },
          {
            id: 'lts__throughput.avg.pct_of_peak_sustained_elapsed',
            label: () => 'L2 Cache Throughput',
            unit: () => '%',
            importance: 'optional',
            aggregation: 'avg',
          },
          {
            id: 'sm__cycles_active.avg',
            label: (t) => `${t.sm.title} Active Cycles`,
            unit: () => 'cycle',
            importance: 'optional',
            aggregation: 'sum',
          },
          {
            id: 'sm__throughput.avg.pct_of_peak_sustained_elapsed',
            label: (t) => `Compute (${t.sm.title}) Throughput`,
            unit: () => '%',
            importance: 'required',
            aggregation: 'avg',
          },
        ],
      },
      {
        description: () =>
          `High-level overview of GPU compute activity and memory system utilization. Shows overall GPU busy cycles, cache hierarchy efficiency, and memory throughput to identify whether the kernel is compute-bound, memory-bound, or underutilizing resources.`,
        rows: [
          {
            id: 'GRBM_GUI_ACTIVE_avr_per_second',
            label: (t) => `${t.sm.title} Frequency`,
            unit: () => 'hz',
            importance: 'optional',
          },
          {
            id: 'GRBM_TIME_DUR_max',
            label: () => 'Duration',
            unit: () => 'nsecond',
            importance: 'optional',
            aggregation: 'sum',
          },
          {
            id: 'GRBM_GUI_ACTIVE_avr',
            label: () => 'Elapsed Cycles',
            unit: () => 'cycle',
            importance: 'optional',
            aggregation: 'sum',
          },
          {
            id: 'SQ_BUSY_CYCLES_avr',
            label: (t) => `${t.sm.title} Active Cycles`,
            unit: () => 'cycle',
            importance: 'required',
            aggregation: 'sum',
          },
        ],
      },
    ],
  });
}
