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

// Per-process capture availability query shared by the timeline-first Memory
// page. Keeps section ordering fast without waiting for every subsection's
// heavier SQL queries.

import type {Trace} from '../../../../public/trace';
import {LONG_NULL, NUM, STR} from '../../../../trace_processor/query_result';

export interface SourceFacts {
  readonly samples: number;
  readonly spanS?: number;
}

export interface CaptureInfo {
  readonly processName: string;
  readonly pid: number;
  readonly smaps: SourceFacts;
  readonly dumps: number;
  readonly hasBitmaps: boolean;
  readonly native: SourceFacts;
}

function makeSourceFacts(
  samples: number,
  minTs: bigint | null,
  maxTs: bigint | null,
): SourceFacts {
  if (samples === 0) return {samples: 0};
  const spanS =
    samples > 1 && minTs !== null && maxTs !== null
      ? Number(maxTs - minTs) / 1e9
      : undefined;
  return {samples, spanS};
}

export async function loadCaptureInfo(
  trace: Trace,
  upid: number,
): Promise<CaptureInfo> {
  const res = await trace.engine.query(`
    WITH
    smaps AS (
      SELECT COUNT(DISTINCT ts) AS n, MIN(ts) AS min_ts, MAX(ts) AS max_ts
      FROM profiler_smaps WHERE upid = ${upid}
    ),
    native AS (
      SELECT COUNT(DISTINCT ts) AS n, MIN(ts) AS min_ts, MAX(ts) AS max_ts
      FROM heap_profile_allocation WHERE upid = ${upid}
    )
    SELECT
      coalesce(
        (SELECT COALESCE(cmdline, name) FROM process WHERE upid = ${upid}),
        '<unknown>'
      ) AS pname,
      coalesce((SELECT pid FROM process WHERE upid = ${upid}), 0) AS pid,
      smaps.n AS smaps_n,
      smaps.min_ts AS smaps_min_ts,
      smaps.max_ts AS smaps_max_ts,
      (
        SELECT COUNT(*) FROM heap_graph WHERE upid = ${upid}
      ) AS dumps,
      EXISTS(
        SELECT 1
        FROM heap_graph_object o
        JOIN heap_graph_class c ON c.id = o.type_id
        WHERE o.upid = ${upid} AND o.reachable
          AND (c.name = 'android.graphics.Bitmap'
            OR c.deobfuscated_name = 'android.graphics.Bitmap')
        LIMIT 1
      ) AS has_bitmaps,
      native.n AS native_n,
      native.min_ts AS native_min_ts,
      native.max_ts AS native_max_ts
    FROM smaps, native
  `);
  const row = res.firstRow({
    pname: STR,
    pid: NUM,
    smaps_n: NUM,
    smaps_min_ts: LONG_NULL,
    smaps_max_ts: LONG_NULL,
    dumps: NUM,
    has_bitmaps: NUM,
    native_n: NUM,
    native_min_ts: LONG_NULL,
    native_max_ts: LONG_NULL,
  });
  return {
    processName: row.pname,
    pid: row.pid,
    smaps: makeSourceFacts(row.smaps_n, row.smaps_min_ts, row.smaps_max_ts),
    dumps: row.dumps,
    hasBitmaps: row.has_bitmaps !== 0,
    native: makeSourceFacts(row.native_n, row.native_min_ts, row.native_max_ts),
  };
}
