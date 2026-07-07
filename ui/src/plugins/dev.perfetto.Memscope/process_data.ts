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
  {
    key: 'anon_swap',
    label: 'RSS Anon + Swap',
    counters: ['mem.rss.anon', 'mem.swap'],
  },
  {key: 'file', label: 'File', counters: ['mem.rss.file']},
  {key: 'dmabuf', label: 'DMA-BUF', counters: ['mem.dmabuf_rss']},
  {key: 'rss', label: 'Total RSS', counters: ['mem.rss']},
];

export interface OomScoreBucket {
  readonly name: string;
  readonly color: string;
  readonly minScore: number;
  readonly maxScore: number;
}

export const OOM_SCORE_BUCKETS: readonly OomScoreBucket[] = [
  {
    name: 'Native (< 0)',
    color: 'var(--pf-color-primary)',
    minScore: -1000,
    maxScore: -1,
  },
  {
    name: 'Foreground (0)',
    color: 'var(--pf-color-success)',
    minScore: 0,
    maxScore: 0,
  },
  {
    name: 'Visible (1-99)',
    color: 'var(--pf-color-success)',
    minScore: 1,
    maxScore: 99,
  },
  {
    name: 'Perceptible (100-299)',
    color: 'var(--pf-color-warning)',
    minScore: 100,
    maxScore: 299,
  },
  {
    name: 'Service (300-599)',
    color: 'var(--pf-color-warning)',
    minScore: 300,
    maxScore: 599,
  },
  {
    name: 'Cached (600-899)',
    color: 'var(--pf-color-danger)',
    minScore: 600,
    maxScore: 899,
  },
  {
    name: 'Cached (900+)',
    color: 'var(--pf-color-danger)',
    minScore: 900,
    maxScore: 1001,
  },
];

// The OOM bucket an oom_score_adj value falls into, or undefined if it's out of
// every range.
export function oomScoreBucket(score: number): OomScoreBucket | undefined {
  return OOM_SCORE_BUCKETS.find(
    (b) => score >= b.minScore && score <= b.maxScore,
  );
}

// The bucket name without its trailing "(range)" suffix, e.g. "Foreground".
export function oomBucketLabel(bucket: OomScoreBucket): string {
  return bucket.name.replace(/ \(.*\)$/, '');
}

// Per-process memory row (latest value only, for the table).
export interface ProcessMemoryRow {
  readonly processName: string;
  readonly pid: number;
  readonly rssKb: number;
  readonly anonKb: number;
  readonly fileKb: number;
  readonly shmemKb: number;
  readonly swapKb: number;
  readonly dmabufKb: number;
  readonly oomScore: number;
  readonly debuggable: boolean;
  readonly ageSeconds: number | null;
  // RSS time series in KB, sorted ascending by ts (nanoseconds). Used to
  // render a per-row sparkline.
  readonly rssTrendKb: readonly number[];
}
