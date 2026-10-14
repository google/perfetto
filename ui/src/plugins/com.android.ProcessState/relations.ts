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
// by the android_process_state trace_processor plugin from snapshots and
// incremental ActivityManager track events — onto the stable `_ps_*` relations
// the explorer reads.

import type {Engine} from '../../trace_processor/engine';
import {NUM} from '../../trace_processor/query_result';

const SETUP_SQL: ReadonlyArray<string> = [
  `CREATE PERFETTO TABLE _ps_snapshot AS
   SELECT id, ts,
          IFNULL(LEAD(ts) OVER (ORDER BY ts, id) - ts,
                 MAX((SELECT end_ts FROM trace_bounds) - ts, 0)) AS dur,
          seq_id,
          reason,
          is_full_update,
          top_pid,
          top_proc_state,
          target_pids,
          UPPER(COALESCE(replace(reason, 'OOM_ADJ_REASON_', ''), 'INITIAL')) AS name
   FROM __intrinsic_android_process_state_snapshot`,

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
   SELECT snapshot_id, svc_id AS service_id, owning_pid, uid, name,
          is_foreground, foreground_service_type, start_requested
   FROM __intrinsic_android_process_state_service`,

  `CREATE PERFETTO VIEW _ps_service_binding AS
   SELECT snapshot_id, bind_id, service_id, intent_bind_id, client_pid,
          client_uid, foreground, flags, intent_action
   FROM __intrinsic_android_process_state_service_binding`,

  `CREATE PERFETTO VIEW _ps_provider AS
   SELECT snapshot_id, provider_id, owning_pid, uid, authority, component_name
   FROM __intrinsic_android_process_state_provider`,

  `CREATE PERFETTO VIEW _ps_provider_binding AS
   SELECT snapshot_id, bind_id, provider_id, client_pid, client_uid, stable
   FROM __intrinsic_android_process_state_provider_binding`,

  `CREATE PERFETTO VIEW _ps_trigger_event AS
   SELECT id, ts, seq_id, kind, action, component_name,
          target_pid, target_uid, caller_pid, caller_uid, detail,
          service_id, bind_id, intent_bind_id, provider_id
   FROM __intrinsic_android_process_state_trigger_event`,
];

/**
 * Builds the `_ps_*` relations the explorer reads. Returns the number of
 * process rows available (0 → no graph snapshot data in this trace).
 */
export async function buildProcessState(engine: Engine): Promise<number> {
  try {
    for (const sql of SETUP_SQL) {
      await engine.query(sql);
    }
  } catch {
    return 0;
  }
  const res = await engine.query(`SELECT count(*) AS n FROM _ps_process`);
  return res.iter({n: NUM}).n;
}
