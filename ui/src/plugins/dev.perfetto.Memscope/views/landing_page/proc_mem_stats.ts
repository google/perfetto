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
}

export type ProcWithMem = readonly ProcMemStat[];

// Returns a list processes that have memory dumps/smaps/profiles in the trace.
export async function loadProcessMemoryStats(
  engine: Engine,
): Promise<ProcWithMem> {
  const result = await engine.query(`
    SELECT
      p.upid,
      p.pid,
      COALESCE(p.cmdline, p.name, '<unknown>') AS procName,
      (
        SELECT count(*)
        FROM heap_graph g
        WHERE g.upid = p.upid
      ) AS heapDumps,
      (
        SELECT count(DISTINCT ts)
        FROM profiler_smaps s
        WHERE s.upid = p.upid
      ) AS smapsSnapshots,
      (
        SELECT count(DISTINCT ts)
        FROM heap_profile_allocation a
        WHERE a.upid = p.upid
      ) AS nativeDumps
    FROM process p
    WHERE heapDumps > 0 OR smapsSnapshots > 0 OR nativeDumps > 0
    ORDER BY p.upid;
  `);
  return materializeRows(result, {
    upid: NUM,
    pid: NUM,
    procName: STR,
    heapDumps: NUM,
    smapsSnapshots: NUM,
    nativeDumps: NUM,
  });
}

// Scores a process to determine how relevant it is for the landing page.
// Higher score = more relevant. We weight by data type and count to pick
// the process with the richest memory analysis data.
export function scoreProc(p: ProcMemStat): number {
  // Heap dumps are the richest data source, followed by smaps, then profiles.
  return p.heapDumps * 3 + p.smapsSnapshots * 2 + p.nativeDumps * 1;
}

export function pickBestProc(procs: ProcWithMem): ProcMemStat | undefined {
  if (procs.length === 0) return undefined;
  return procs.reduce((best, p) => (scoreProc(p) > scoreProc(best) ? p : best));
}

export async function getBestProcess(
  engine: Engine,
): Promise<number | undefined> {
  const procs = await loadProcessMemoryStats(engine);
  return pickBestProc(procs)?.upid;
}
