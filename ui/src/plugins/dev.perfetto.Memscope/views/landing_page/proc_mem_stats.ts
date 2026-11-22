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

import type {Engine} from '../../../../trace_processor/engine';
import {
  materializeRows,
  NUM,
  STR,
} from '../../../../trace_processor/query_result';

// Per-process memory-capture counts, used to populate and score the process
// picker on the overview page.
export interface ProcMemStat {
  readonly upid: number;
  readonly pid: number;
  readonly procName: string;
  readonly heapDumps: number;
  readonly smapsSnapshots: number;
  readonly nativeDumps: number;
  // Relevance for the landing page, see memscope_process_mem_stats.
  readonly score: number;
}

export type ProcWithMem = readonly ProcMemStat[];

// Returns a list processes that have memory dumps/smaps/profiles in the trace.
export async function loadProcessMemoryStats(
  engine: Engine,
): Promise<ProcWithMem> {
  const result = await engine.query(`
    INCLUDE PERFETTO MODULE memscope.processes;
    SELECT
      upid,
      pid,
      process_name AS procName,
      heap_dumps AS heapDumps,
      smaps_snapshots AS smapsSnapshots,
      native_dumps AS nativeDumps,
      score
    FROM memscope_process_mem_stats
    ORDER BY upid;
  `);
  return materializeRows(result, {
    upid: NUM,
    pid: NUM,
    procName: STR,
    heapDumps: NUM,
    smapsSnapshots: NUM,
    nativeDumps: NUM,
    score: NUM,
  });
}

export function pickBestProc(procs: ProcWithMem): ProcMemStat | undefined {
  if (procs.length === 0) return undefined;
  return procs.reduce((best, p) => (p.score > best.score ? p : best));
}

export async function getBestProcess(
  engine: Engine,
): Promise<number | undefined> {
  const procs = await loadProcessMemoryStats(engine);
  return pickBestProc(procs)?.upid;
}
