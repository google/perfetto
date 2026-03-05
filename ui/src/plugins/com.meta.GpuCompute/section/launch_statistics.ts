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

import {registerSection} from './index';

export function registerLaunchStatisticsSection(): void {
  registerSection({
    id: 'com.meta.GpuCompute.Section.LaunchStatistics',
    title: 'Launch Statistics',
    order: 1,
    launchMetrics: [
      'launch__block_size',
      'launch__func_cache_config',
      'launch__grid_size',
      'launch__registers_per_thread',
      'launch__shared_mem_config_size',
      'launch__shared_mem_per_block_dynamic',
      'launch__shared_mem_per_block_static',
      'launch__barriers_per_block',
      'launch__thread_count',
      'launch__waves_per_multiprocessor',
    ],
    counterMetrics: [],
    analysisPrompt:
      `These metrics describe the configuration used to launch the kernel, which defines the size of the work,\n` +
      `how it's divided into blocks, and the GPU resources needed to execute the kernel.\n` +
      `\n` +
      `Provide concise, actionable analysis with specific recommendations for launch configuration optimization.`,
    tables: [
      {
        description: (t) =>
          `Summary of the configuration used to launch the kernel. The launch configuration defines the size of the kernel ${t.grid.name}, the division of the ${t.grid.name} into ${t.block.plural}, and the GPU resources needed to execute the kernel. Choosing an efficient launch configuration maximizes device utilization.`,
        rows: [
          {
            id: 'launch__block_size',
            label: (t) => `${t.block.title} Size`,
            unit: () => '',
            importance: 'required',
          },
          {
            id: 'launch__func_cache_config',
            label: () => 'Function Cache Configuration',
            unit: () => '',
            importance: 'optional',
          },
          {
            id: 'launch__grid_size',
            label: (t) => `${t.grid.title} Size`,
            unit: () => '',
            importance: 'required',
          },
          {
            id: 'launch__registers_per_thread',
            label: (t) => `Registers Per ${t.thread.title}`,
            unit: (t) => `register/${t.thread.name}`,
            importance: 'optional',
          },
          {
            id: 'launch__shared_mem_config_size',
            label: (t) => `${t.sharedMem.title} Configuration Size`,
            unit: () => 'byte',
            importance: 'optional',
          },
          {
            id: 'launch__shared_mem_per_block_dynamic',
            label: (t) => `Dynamic ${t.sharedMem.title} Per ${t.block.title}`,
            unit: (t) => `byte/${t.block.name}`,
            importance: 'optional',
          },
          {
            id: 'launch__shared_mem_per_block_static',
            label: (t) => `Static ${t.sharedMem.title} Per ${t.block.title}`,
            unit: (t) => `byte/${t.block.name}`,
            importance: 'optional',
          },
          {
            id: 'launch__barriers_per_block',
            label: (t) => `Barriers Per ${t.block.title}`,
            unit: () => '',
            importance: 'optional',
          },
          {
            id: 'launch__thread_count',
            label: (t) => `${t.thread.pluralTitle}`,
            unit: (t) => `${t.thread.name}`,
            importance: 'optional',
          },
          {
            id: 'launch__waves_per_multiprocessor',
            label: (t) => `Waves Per ${t.sm.title}`,
            unit: () => '',
            importance: 'optional',
          },
        ],
      },
    ],
  });
}
