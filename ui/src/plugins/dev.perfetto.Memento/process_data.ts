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

// Shared types and constants used by both chart_builders and tab_processes.

export type ProcessGrouping = 'category' | 'oom_score';

export type ProcessMetric = 'rss' | 'anon_swap' | 'file' | 'dmabuf';

export const PROCESS_METRIC_OPTIONS: ReadonlyArray<{
  key: ProcessMetric;
  label: string;
  counters: readonly string[];
}> = [
  {key: 'rss', label: 'Total RSS', counters: ['mem.rss']},
  {
    key: 'anon_swap',
    label: 'Anon + Swap',
    counters: ['mem.rss.anon', 'mem.swap'],
  },
  {key: 'file', label: 'File', counters: ['mem.rss.file']},
  {key: 'dmabuf', label: 'DMA-BUF', counters: ['mem.dmabuf_rss']},
];

export interface OomScoreBucket {
  readonly name: string;
  readonly color: string;
  readonly minScore: number;
  readonly maxScore: number;
}

export const OOM_SCORE_BUCKETS: readonly OomScoreBucket[] = [
  {name: 'Native (< 0)', color: '#1565c0', minScore: -1000, maxScore: -1},
  {name: 'Foreground (0)', color: '#4caf50', minScore: 0, maxScore: 0},
  {name: 'Visible (1-99)', color: '#8bc34a', minScore: 1, maxScore: 99},
  {
    name: 'Perceptible (100-299)',
    color: '#ff9800',
    minScore: 100,
    maxScore: 299,
  },
  {name: 'Service (300-599)', color: '#ff5722', minScore: 300, maxScore: 599},
  {name: 'Cached (600-899)', color: '#9c27b0', minScore: 600, maxScore: 899},
  {name: 'Cached (900+)', color: '#f44336', minScore: 900, maxScore: 1001},
];

// Per-process memory row (latest value only, for the table).
export interface ProcessMemoryRow {
  processName: string;
  pid: number;
  rssKb: number;
  anonKb: number;
  fileKb: number;
  shmemKb: number;
  swapKb: number;
  dmabufKb: number;
  oomScore: number;
  debuggable: boolean;
  ageSeconds: number | null;
}
