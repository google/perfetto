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

export function registerWorkloadAnalysisSection(): void {
  registerSection({
    id: 'com.meta.GpuCompute.Section.WorkloadAnalysis',
    title: 'Workload Analysis',
    order: 3,
    launchMetrics: [],
    counterMetrics: [
      'sm__inst_executed.avg.per_cycle_active',
      'sm__inst_executed.avg.per_cycle_elapsed',
      'sm__instruction_throughput.avg.pct_of_peak_sustained_active',
      'sm__pipe_alu_cycles_active.avg.pct_of_peak_sustained_active',
      'sm__inst_executed_pipe_alu.avg.pct_of_peak_sustained_active',
      'sm__pipe_fma_cycles_active.avg.pct_of_peak_sustained_active',
      'sm__inst_executed_pipe_fma.avg.pct_of_peak_sustained_active',
      'sm__pipe_fp16_cycles_active.avg.pct_of_peak_sustained_active',
      'sm__inst_executed_pipe_fp16.avg.pct_of_peak_sustained_active',
      'sm__pipe_fp32_cycles_active.avg.pct_of_peak_sustained_active',
      'sm__inst_executed_pipe_fp32.avg.pct_of_peak_sustained_active',
      'sm__pipe_fp64_cycles_active.avg.pct_of_peak_sustained_active',
      'sm__inst_executed_pipe_fp64.avg.pct_of_peak_sustained_active',
      'sm__pipe_tensor_cycles_active.avg.pct_of_peak_sustained_active',
    ],
    collapsedByDefault: true,
    analysisPrompt:
      `These metrics provide detailed analysis of compute resource utilization,\n` +
      `including instruction throughput and pipeline/instruction type breakdown.\n` +
      `\n` +
      `When analyzing these metrics:\n` +
      `1. Identify which compute pipelines or instruction types are most utilized\n` +
      `2. Look for imbalanced usage that might indicate inefficiencies\n` +
      `3. Consider if the workload could benefit from different precision or instruction patterns\n` +
      `\n` +
      `Provide concise, actionable analysis with specific recommendations for compute optimization.`,
    tables: [
      {
        description: (t) =>
          `Detailed analysis of the compute resources of the ${t.streamingMultiprocessor.plural} (${t.sm.pluralTitle}), including instruction throughput and the utilization of each available pipeline. Pipelines or instruction types with very high utilization might limit overall performance.`,
        rows: [
          {
            id: 'sm__inst_executed.avg.per_cycle_active',
            label: () => 'Executed IPC Active',
            unit: () => 'inst/cycle',
            importance: 'required',
            aggregation: 'avg',
          },
          {
            id: 'sm__inst_executed.avg.per_cycle_elapsed',
            label: () => 'Executed IPC Elapsed',
            unit: () => 'inst/cycle',
            importance: 'optional',
            aggregation: 'avg',
          },
          {
            id: 'sm__instruction_throughput.avg.pct_of_peak_sustained_active',
            label: (t) => `${t.sm.title} Busy`,
            unit: () => '%',
            importance: 'required',
            aggregation: 'avg',
          },
          {
            id: 'sm__pipe_alu_cycles_active.avg.pct_of_peak_sustained_active',
            label: () => 'ALU Pipe Active',
            unit: () => '%',
            importance: 'optional',
            aggregation: 'avg',
          },
          {
            id: 'sm__inst_executed_pipe_alu.avg.pct_of_peak_sustained_active',
            label: () => 'ALU Pipe Inst Executed',
            unit: () => '%',
            importance: 'optional',
            aggregation: 'avg',
          },
          {
            id: 'sm__pipe_fma_cycles_active.avg.pct_of_peak_sustained_active',
            label: () => 'FMA Pipe Active',
            unit: () => '%',
            importance: 'optional',
            aggregation: 'avg',
          },
          {
            id: 'sm__inst_executed_pipe_fma.avg.pct_of_peak_sustained_active',
            label: () => 'FMA Pipe Inst Executed',
            unit: () => '%',
            importance: 'optional',
            aggregation: 'avg',
          },
          {
            id: 'sm__pipe_fp16_cycles_active.avg.pct_of_peak_sustained_active',
            label: () => 'FP16 Pipe Active',
            unit: () => '%',
            importance: 'optional',
            aggregation: 'avg',
          },
          {
            id: 'sm__inst_executed_pipe_fp16.avg.pct_of_peak_sustained_active',
            label: () => 'FP16 Pipe Inst Executed',
            unit: () => '%',
            importance: 'optional',
            aggregation: 'avg',
          },
          {
            id: 'sm__pipe_fp32_cycles_active.avg.pct_of_peak_sustained_active',
            label: () => 'FP32 Pipe Active',
            unit: () => '%',
            importance: 'optional',
            aggregation: 'avg',
          },
          {
            id: 'sm__inst_executed_pipe_fp32.avg.pct_of_peak_sustained_active',
            label: () => 'FP32 Pipe Inst Executed',
            unit: () => '%',
            importance: 'optional',
            aggregation: 'avg',
          },
          {
            id: 'sm__pipe_fp64_cycles_active.avg.pct_of_peak_sustained_active',
            label: () => 'FP64 Pipe Active',
            unit: () => '%',
            importance: 'optional',
            aggregation: 'avg',
          },
          {
            id: 'sm__inst_executed_pipe_fp64.avg.pct_of_peak_sustained_active',
            label: () => 'FP64 Pipe Inst Executed',
            unit: () => '%',
            importance: 'optional',
            aggregation: 'avg',
          },
          {
            id: 'sm__pipe_tensor_cycles_active.avg.pct_of_peak_sustained_active',
            label: (t) => `${t.tensor.title} Pipe Active`,
            unit: () => '%',
            importance: 'optional',
            aggregation: 'avg',
          },
        ],
      },
    ],
  });
}
