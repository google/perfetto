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

// Projects the `__intrinsic_android_process_state_*` intrinsic tables — filled
// by the process_state_importer trace_processor plugin from ProcessStateSnapshot
// packets (the data behind `dumpsys activity`) — onto the stable `_ps_*`
// relations the explorer reads. Enum fields (proc_state, capabilities, reason)
// already arrive as resolved names from the importer, so this is a plain rename
// projection with no enum logic of its own.

import type {Engine} from '../../trace_processor/engine';
import {NUM} from '../../trace_processor/query_result';

// Only _ps_snapshot is materialized; the rest are plain views. This split is
// deliberate and measured. The explorer reads one snapshot at a time (select a
// snapshot -> its processes/services/bindings), and the intrinsic tables filter
// on snapshot_id in ~a few ms, so views over them are perfectly fine for a
// single-snapshot read. The one exception is the timeline slice's gap-free `dur`
// (a LEAD() window): a window function blocks predicate push-down, so fetching a
// single slice by id would recompute dur for *every* snapshot. On a long trace
// (tens of thousands of snapshots) that one query alone took ~110s. Precomputing
// it once into a table makes the slice fetch instant; everything else stays a
// cheap view.
const SETUP_SQL: ReadonlyArray<string> = [
  // Snapshots, with the gap-free slice span (dur) and display name precomputed
  // once. Filtered by id (the table's primary key, already fast), so no index is
  // needed; the point is just to evaluate the LEAD() window a single time.
  `CREATE PERFETTO TABLE _ps_snapshot AS
   SELECT id, ts,
          IFNULL(LEAD(ts) OVER (ORDER BY ts) - ts,
                 (SELECT end_ts FROM trace_bounds) - ts) AS dur,
          reason,
          UPPER(COALESCE(reason, 'snapshot')) AS name
   FROM __intrinsic_android_process_state_snapshot`,

  // The track-event reconstruction path has no process name (the
  // process_state_changed_event doesn't carry one), so fall back to the name
  // from trace_processor's `process` table (process-tree / ftrace) for the
  // process live at the snapshot's timestamp. The one-shot dumpsys snapshot
  // already has names, so its rows are unaffected. This subquery only ever runs
  // for the ~processes of the single selected snapshot, so a view is fine.
  `CREATE PERFETTO VIEW _ps_process AS
   SELECT pr.snapshot_id, pr.pid, pr.uid,
          COALESCE(pr.name, (
            SELECT p.name FROM process p
            WHERE p.pid = pr.pid
              AND (p.start_ts IS NULL OR p.start_ts <= s.ts)
              AND (p.end_ts IS NULL OR p.end_ts >= s.ts)
            LIMIT 1)) AS name,
          pr.oom_score, pr.proc_state, pr.capabilities, pr.persistent
   FROM __intrinsic_android_process_state_process pr
   JOIN __intrinsic_android_process_state_snapshot s ON s.id = pr.snapshot_id`,

  `CREATE PERFETTO VIEW _ps_service AS
   SELECT snapshot_id, svc_id AS service_id, owning_pid, name
   FROM __intrinsic_android_process_state_service`,

  `CREATE PERFETTO VIEW _ps_service_binding AS
   SELECT snapshot_id, service_id, client_pid, foreground
   FROM __intrinsic_android_process_state_service_binding`,

  `CREATE PERFETTO VIEW _ps_provider AS
   SELECT snapshot_id, provider_id, owning_pid, authority
   FROM __intrinsic_android_process_state_provider`,

  `CREATE PERFETTO VIEW _ps_provider_binding AS
   SELECT snapshot_id, provider_id, client_pid, stable
   FROM __intrinsic_android_process_state_provider_binding`,
];

/**
 * Builds the `_ps_*` relations the explorer reads. Returns the number of
 * process rows available (0 → no process-state data in this trace; the plugin
 * should stay inactive).
 */
export async function buildProcessState(engine: Engine): Promise<number> {
  try {
    for (const sql of SETUP_SQL) {
      await engine.query(sql);
    }
  } catch {
    // The intrinsic tables don't exist (a trace_processor without the
    // process_state_importer plugin) — leave the plugin inactive.
    return 0;
  }
  const res = await engine.query(`SELECT count(*) AS n FROM _ps_process`);
  return res.iter({n: NUM}).n;
}
