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

import m from 'mithril';
import {Time} from '../../base/time';
import {sqliteString} from '../../base/string_utils';
import type {Trace} from '../../public/trace';
import {
  LONG,
  LONG_NULL,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';
import {
  type ActiveBucketState,
  ALL_TARGETS_VALUE,
  type BucketState,
  type CategorySummary,
  classifyProcStateFamily,
  type ColorMode,
  type ComponentCategory,
  type ComponentTargetOption,
  type ProcStateFamily,
  type ProcessTimelineRow,
  type TimelineDataset,
  type TraceHeaderMetadata,
} from './types';

const BUCKET_NS = 100_000_000n;
const BUCKET_MS = 100;
const LINGER_MS = 5000;
const IDLE_THRESHOLD_MS = 1.0;
const CPU_SCALE_MS = 100;
const DEFAULT_MEM_SCALE_MB = 500;

interface RawInterval {
  readonly startNs: bigint;
  readonly endNs: bigint;
}

interface RawProcStateInterval {
  readonly startNs: bigint;
  readonly endNs: bigint;
  readonly state: string;
  readonly oomScore: number | null;
}

export class ComponentTimelineModel {
  readonly trace: Trace;

  isInitialized = false;
  isLoading = true;
  errorMessage: string | null = null;

  headerMeta: TraceHeaderMetadata = {
    uuid: '',
    device: 'android',
    build: 'unknown',
    trigger: null,
    ncpu: 8,
    nlmk: 0,
    hasFrameworkProcState: false,
    windowStartNs: 0n,
    windowEndNs: 0n,
  };

  categories: ReadonlyArray<CategorySummary> = [];
  selectedCategory: ComponentCategory = 'broadcast';
  selectedTarget: string = ALL_TARGETS_VALUE;
  colorMode: ColorMode = 'component';
  stateFilter: ActiveBucketState | ProcStateFamily | null = null;
  searchQuery = '';
  syncTimeline = true;

  dataset: TimelineDataset | null = null;
  frame = 0;
  speedMs = 1000;
  private timerId: ReturnType<typeof setInterval> | null = null;
  private readonly datasetCache = new Map<string, TimelineDataset>();
  readonly shownUpids = new Set<number>();
  readonly newlyShownUpids = new Set<number>();

  constructor(trace: Trace) {
    this.trace = trace;
  }

  get isPlaying(): boolean {
    return this.timerId !== null;
  }

  dispose(): void {
    this.stop();
  }

  async initialize(): Promise<void> {
    if (this.isInitialized) return;
    this.isLoading = true;
    this.errorMessage = null;
    try {
      const engine = this.trace.engine;

      const psCheck = await engine.tryQuery(
        `SELECT count() AS cnt FROM __intrinsic_android_process_state`,
      );
      const hasFrameworkProcState =
        psCheck.ok && psCheck.value.firstRow({cnt: NUM}).cnt > 0;

      await engine.query(`
        INCLUDE PERFETTO MODULE intervals.overlap;
        INCLUDE PERFETTO MODULE android.oom_adjuster;
        INCLUDE PERFETTO MODULE android.memory.lmk;
        INCLUDE PERFETTO MODULE android.job_scheduler_states_track_events;
      `);
      if (hasFrameworkProcState) {
        await engine.query(`INCLUDE PERFETTO MODULE android.process_state;`);
      }

      const metaRes = await engine.query(`
        SELECT
          COALESCE((SELECT min(ts) FROM sched WHERE dur > 0), trace_start()) AS min_sched_ns,
          COALESCE((SELECT max(ts + dur) FROM sched WHERE dur > 0), trace_end()) AS max_sched_ns,
          trace_start() AS trace_start_ns,
          trace_end() AS trace_end_ns,
          (SELECT count(DISTINCT ucpu) FROM sched WHERE ucpu IS NOT NULL) AS ncpu,
          (
            (SELECT count() FROM android_lmk_events) +
            (
              SELECT count()
              FROM slice
              WHERE name = 'process_died'
                AND (
                  extract_arg(arg_set_id, 'process_died_event.reason') = 'APP_EXIT_REASON_LOW_MEMORY'
                  OR extract_arg(arg_set_id, 'process_died_event.sub_reason') = 'APP_EXIT_SUBREASON_OOM_KILL'
                )
                AND COALESCE(extract_arg(arg_set_id, 'process_died_event.upid'), -1) NOT IN (
                  SELECT upid FROM android_lmk_events WHERE upid IS NOT NULL
                )
            )
          ) AS nlmk,
          (
            SELECT str_value
            FROM metadata
            WHERE name = 'android_build_fingerprint'
              AND str_value NOT GLOB '*isolated_storage*'
            LIMIT 1
          ) AS fingerprint,
          (SELECT str_value FROM metadata WHERE name = 'trace_uuid' LIMIT 1) AS trace_uuid,
          (SELECT str_value FROM metadata WHERE name = 'trace_trigger' LIMIT 1) AS trace_trigger
      `);

      const metaRow = metaRes.firstRow({
        min_sched_ns: LONG,
        max_sched_ns: LONG,
        trace_start_ns: LONG,
        trace_end_ns: LONG,
        ncpu: NUM,
        nlmk: NUM,
        fingerprint: STR_NULL,
        trace_uuid: STR_NULL,
        trace_trigger: STR_NULL,
      });

      let windowStartNs =
        metaRow.min_sched_ns - metaRow.trace_start_ns < 5_000_000_000n
          ? metaRow.trace_start_ns
          : metaRow.min_sched_ns;
      const windowEndNs =
        metaRow.trace_end_ns - metaRow.max_sched_ns < 5_000_000_000n
          ? metaRow.trace_end_ns
          : metaRow.max_sched_ns;

      if (windowEndNs - windowStartNs > 120_000_000_000n) {
        windowStartNs = windowEndNs - 120_000_000_000n;
      }

      let device = 'android';
      let build = 'trace';
      if (metaRow.fingerprint !== null && metaRow.fingerprint !== '') {
        const parts = metaRow.fingerprint.split('/');
        if (parts.length >= 2 && parts[1] !== '') {
          device = parts[1];
        }
        if (parts.length >= 4 && parts[3] !== '') {
          build = parts[3];
        }
      }

      this.headerMeta = {
        uuid: metaRow.trace_uuid ?? this.trace.traceInfo.uuid,
        device,
        build,
        trigger: metaRow.trace_trigger,
        ncpu: metaRow.ncpu > 0 ? metaRow.ncpu : 8,
        nlmk: metaRow.nlmk,
        hasFrameworkProcState,
        windowStartNs,
        windowEndNs,
      };

      await this.createHelperTables(
        windowStartNs,
        windowEndNs,
        hasFrameworkProcState,
      );
      await this.loadCategorySummaries();
      this.pickInitialSelection();
      await this.loadSelectedDataset();
      this.isInitialized = true;
    } catch (e) {
      this.errorMessage = e instanceof Error ? e.message : String(e);
    } finally {
      this.isLoading = false;
      m.redraw();
    }
  }

  private async createHelperTables(
    windowStartNs: bigint,
    windowEndNs: bigint,
    hasFrameworkProcState: boolean,
  ): Promise<void> {
    const engine = this.trace.engine;

    const procStateUnionSql = hasFrameworkProcState
      ? `
        UNION ALL
        SELECT
          psi.id AS slice_id,
          'proc_state' AS category,
          psi.state AS target,
          psi.upid,
          psi.pid,
          COALESCE(psi.process_name, 'pid ' || psi.pid) AS process_name,
          max(psi.ts, ${windowStartNs}) AS ts,
          min(
            psi.ts + iif(psi.dur < 0, ${windowEndNs} - psi.ts, psi.dur),
            ${windowEndNs}
          ) - max(psi.ts, ${windowStartNs}) AS dur,
          psi.state AS raw_name
        FROM _android_process_state_intervals psi
        WHERE psi.state NOT IN ('NONEXISTENT', 'EXITED')
          AND psi.process_name != 'system_server'
          AND psi.ts < ${windowEndNs}
          AND psi.ts + iif(psi.dur < 0, ${windowEndNs} - psi.ts, psi.dur) > ${windowStartNs}
      `
      : `
        UNION ALL
        SELECT
          CAST(NULL AS INT) AS slice_id,
          'proc_state' AS category,
          oai.bucket AS target,
          oai.upid,
          p.pid,
          COALESCE(oai.process_name, p.name, 'pid ' || p.pid) AS process_name,
          max(oai.ts, ${windowStartNs}) AS ts,
          min(oai.ts + oai.dur, ${windowEndNs}) - max(oai.ts, ${windowStartNs}) AS dur,
          oai.bucket AS raw_name
        FROM android_oom_adj_intervals oai
        JOIN process p USING (upid)
        WHERE oai.upid IS NOT NULL
          AND p.name IS NOT NULL
          AND p.name != 'system_server'
          AND p.name GLOB '*.*'
          AND p.name NOT GLOB '/*'
          AND oai.ts < ${windowEndNs}
          AND oai.ts + oai.dur > ${windowStartNs}
      `;

    await engine.query(`
      DROP TABLE IF EXISTS _android_component_timeline_events;
      CREATE PERFETTO TABLE _android_component_timeline_events AS
      WITH broadcast_raw AS (
        -- FrameworksBaseTrackEvent.broadcast_event (field 2008, slice name 'broadcast_delivered')
        SELECT
          s.id AS slice_id,
          s.ts AS finish_ts,
          s.dur AS slice_dur,
          s.name AS raw_name,
          extract_arg(s.arg_set_id, 'broadcast_event.action_name') AS target,
          COALESCE(
            extract_arg(s.arg_set_id, 'broadcast_event.receiver_upid'),
            (
              SELECT max(p2.upid)
              FROM process p2
              WHERE p2.pid = extract_arg(s.arg_set_id, 'broadcast_event.receiver_pid')
                AND p2.pid IS NOT NULL
            )
          ) AS upid,
          COALESCE(extract_arg(s.arg_set_id, 'broadcast_event.receive_delay_ms'), 0) AS receive_delay_ms,
          COALESCE(extract_arg(s.arg_set_id, 'broadcast_event.finish_delay_ms'), 0) AS finish_delay_ms
        FROM slice s
        WHERE s.name = 'broadcast_delivered'
          AND s.arg_set_id IS NOT NULL
      ),
      broadcast_events AS (
        SELECT
          b.slice_id,
          'broadcast' AS category,
          b.target,
          p.upid,
          p.pid,
          p.name AS process_name,
          iif(
            b.slice_dur > 0,
            b.finish_ts,
            max(
              ${windowStartNs},
              b.finish_ts - max(b.finish_delay_ms - b.receive_delay_ms, 0) * 1000000
            )
          ) AS ts,
          iif(
            b.slice_dur > 0,
            b.slice_dur,
            max((b.finish_delay_ms - b.receive_delay_ms) * 1000000, 100000000)
          ) AS dur,
          b.raw_name
        FROM broadcast_raw b
        JOIN process p USING (upid)
        WHERE b.target IS NOT NULL
          AND p.name IS NOT NULL
          AND p.name != 'system_server'
          AND b.finish_ts >= ${windowStartNs}
          AND b.finish_ts <= ${windowEndNs}

        UNION ALL

        -- FrameworksBaseTrackEvent.self_broadcast_event (field 2021, slice name 'self_broadcast_delivered')
        SELECT
          s.id AS slice_id,
          'broadcast' AS category,
          extract_arg(s.arg_set_id, 'self_broadcast_event.action_name') AS target,
          p.upid,
          p.pid,
          p.name AS process_name,
          s.ts,
          iif(s.dur > 0, s.dur, 100000000) AS dur,
          s.name AS raw_name
        FROM slice s
        JOIN process p ON p.upid = COALESCE(
          extract_arg(s.arg_set_id, 'self_broadcast_event.upid'),
          (
            SELECT max(p2.upid)
            FROM process p2
            WHERE p2.pid = extract_arg(s.arg_set_id, 'self_broadcast_event.pid')
              AND p2.pid IS NOT NULL
          )
        )
        WHERE s.name = 'self_broadcast_delivered'
          AND s.arg_set_id IS NOT NULL
          AND extract_arg(s.arg_set_id, 'self_broadcast_event.action_name') IS NOT NULL
          AND p.name IS NOT NULL
          AND p.name != 'system_server'
          AND s.ts >= ${windowStartNs}
          AND s.ts <= ${windowEndNs}
      ),
      service_raw AS (
        -- FrameworksBaseTrackEvent.service_state_changed_event (2014) & fg_service_state_changed_event (2016)
        SELECT
          s.id AS slice_id,
          s.ts,
          s.dur AS slice_dur,
          s.name AS raw_name,
          COALESCE(
            extract_arg(s.arg_set_id, 'service_state_changed_event.component_name'),
            extract_arg(s.arg_set_id, 'fg_service_state_changed_event.component_name')
          ) AS target,
          COALESCE(
            extract_arg(s.arg_set_id, 'service_state_changed_event.upid'),
            extract_arg(s.arg_set_id, 'fg_service_state_changed_event.upid'),
            (
              SELECT max(p2.upid)
              FROM process p2
              WHERE p2.pid = COALESCE(
                extract_arg(s.arg_set_id, 'service_state_changed_event.pid'),
                extract_arg(s.arg_set_id, 'fg_service_state_changed_event.pid')
              )
                AND p2.pid IS NOT NULL
            )
          ) AS upid
        FROM slice s
        WHERE s.name IN (
          'service_start',
          'service_stop',
          'service_binding',
          'service_published',
          'service_unbinding',
          'service_connection_removed',
          'service_restart_scheduled',
          'fgs_start',
          'fgs_stop'
        )
          AND s.arg_set_id IS NOT NULL
      ),
      service_with_lead AS (
        SELECT
          sr.*,
          lead(sr.ts) OVER (PARTITION BY sr.upid, sr.target ORDER BY sr.ts, sr.slice_id) AS next_ts,
          lead(sr.raw_name) OVER (PARTITION BY sr.upid, sr.target ORDER BY sr.ts, sr.slice_id) AS next_name
        FROM service_raw sr
        WHERE sr.upid IS NOT NULL AND sr.target IS NOT NULL
      ),
      service_events AS (
        SELECT
          swl.slice_id,
          'service' AS category,
          swl.target,
          p.upid,
          p.pid,
          p.name AS process_name,
          swl.ts,
          CASE
            WHEN swl.raw_name IN ('service_start', 'service_binding', 'service_published', 'fgs_start')
              AND swl.next_name IN ('service_stop', 'service_unbinding', 'service_connection_removed', 'fgs_stop')
              AND swl.next_ts > swl.ts
              THEN swl.next_ts - swl.ts
            WHEN swl.slice_dur > 0 THEN swl.slice_dur
            ELSE 200000000
          END AS dur,
          swl.raw_name
        FROM service_with_lead swl
        JOIN process p USING (upid)
        WHERE swl.raw_name IN (
          'service_start',
          'service_binding',
          'service_published',
          'service_restart_scheduled',
          'fgs_start'
        )
          AND p.name IS NOT NULL
          AND p.name != 'system_server'
          AND swl.ts >= ${windowStartNs}
          AND swl.ts <= ${windowEndNs}
      ),
      provider_raw AS (
        -- FrameworksBaseTrackEvent.provider_state_changed_event (2015)
        SELECT
          s.id AS slice_id,
          s.ts,
          s.dur AS slice_dur,
          s.name AS raw_name,
          extract_arg(s.arg_set_id, 'provider_state_changed_event.authority') AS target,
          COALESCE(
            extract_arg(s.arg_set_id, 'provider_state_changed_event.upid'),
            (
              SELECT max(p2.upid)
              FROM process p2
              WHERE p2.pid = extract_arg(s.arg_set_id, 'provider_state_changed_event.pid')
                AND p2.pid IS NOT NULL
            )
          ) AS upid
        FROM slice s
        WHERE s.name IN (
          'provider_published',
          'provider_acquired',
          'provider_released',
          'provider_died'
        )
          AND s.arg_set_id IS NOT NULL
      ),
      provider_with_lead AS (
        SELECT
          pr.*,
          lead(pr.ts) OVER (PARTITION BY pr.upid, pr.target ORDER BY pr.ts, pr.slice_id) AS next_ts,
          lead(pr.raw_name) OVER (PARTITION BY pr.upid, pr.target ORDER BY pr.ts, pr.slice_id) AS next_name
        FROM provider_raw pr
        WHERE pr.upid IS NOT NULL AND pr.target IS NOT NULL
      ),
      provider_events AS (
        SELECT
          pwl.slice_id,
          'provider' AS category,
          pwl.target,
          p.upid,
          p.pid,
          p.name AS process_name,
          pwl.ts,
          CASE
            WHEN pwl.raw_name IN ('provider_published', 'provider_acquired')
              AND pwl.next_name IN ('provider_released', 'provider_died')
              AND pwl.next_ts > pwl.ts
              THEN pwl.next_ts - pwl.ts
            WHEN pwl.slice_dur > 0 THEN pwl.slice_dur
            ELSE 200000000
          END AS dur,
          pwl.raw_name
        FROM provider_with_lead pwl
        JOIN process p USING (upid)
        WHERE pwl.raw_name IN ('provider_published', 'provider_acquired')
          AND p.name IS NOT NULL
          AND p.name != 'system_server'
          AND pwl.ts >= ${windowStartNs}
          AND pwl.ts <= ${windowEndNs}
      ),
      job_states_lead AS (
        -- FrameworksBaseTrackEvent.job_scheduler_job (2006) via __intrinsic_android_job_scheduler_track_events
        SELECT
          js.slice_id,
          js.ts,
          js.job_name,
          _android_js_extract_package_name(js.job_name) AS package_name,
          js.uid,
          js.job_id,
          js.state,
          lead(js.state, 1) OVER (PARTITION BY js.uid, js.job_name, js.job_id ORDER BY js.ts) AS lead_state,
          lead(js.ts, 1, ${windowEndNs}) OVER (PARTITION BY js.uid, js.job_name, js.job_id ORDER BY js.ts) AS ts_lead,
          lead(js.ts, 1) OVER (PARTITION BY js.uid, js.job_name, js.job_id ORDER BY js.ts) IS NULL AS is_end_slice
        FROM _android_js_job_state_events js
      ),
      job_started_intervals AS (
        SELECT
          row_number() OVER (ORDER BY ts, uid, job_id) AS job_seq,
          slice_id,
          ts,
          ts_lead - ts AS dur,
          job_name,
          package_name,
          uid
        FROM job_states_lead
        WHERE state = 'JOB_STATE_STARTED'
          AND (lead_state = 'JOB_STATE_FINISHED' OR is_end_slice = TRUE)
          AND ts_lead > ts
          AND ts >= ${windowStartNs}
          AND ts <= ${windowEndNs}
      ),
      job_matched_procs AS (
        SELECT
          j.slice_id,
          j.job_name AS target,
          p.upid,
          p.pid,
          p.name AS process_name,
          j.ts,
          j.dur,
          j.job_name AS raw_name,
          row_number() OVER (
            PARTITION BY j.job_seq
            ORDER BY
              CASE
                WHEN p.name = j.package_name THEN 0
                WHEN j.package_name != '' AND p.name GLOB j.package_name || ':*' THEN 1
                ELSE 2
              END,
              p.upid
          ) AS rn
        FROM job_started_intervals j
        JOIN process p ON (
          p.name = j.package_name
          OR (j.package_name != '' AND p.name GLOB j.package_name || ':*')
          OR (j.uid IS NOT NULL AND p.uid = j.uid)
        )
        WHERE p.name IS NOT NULL
          AND p.name != 'system_server'
      ),
      job_events AS (
        SELECT
          slice_id,
          'job' AS category,
          target,
          upid,
          pid,
          process_name,
          ts,
          dur,
          raw_name
        FROM job_matched_procs
        WHERE rn = 1
      ),
      activity_slices AS (
        -- Activities do not have Perfetto SDK TrackEvents yet; use ATrace slices only for Activities
        SELECT id, ts, dur, track_id, name FROM slice WHERE name GLOB 'performCreate:*'
        UNION ALL
        SELECT id, ts, dur, track_id, name FROM slice WHERE name GLOB 'performStart:*'
        UNION ALL
        SELECT id, ts, dur, track_id, name FROM slice WHERE name GLOB 'performResume:*'
        UNION ALL
        SELECT id, ts, dur, track_id, name FROM slice WHERE name GLOB 'performPause:*'
        UNION ALL
        SELECT id, ts, dur, track_id, name FROM slice WHERE name GLOB 'performStop:*'
        UNION ALL
        SELECT id, ts, dur, track_id, name FROM slice WHERE name GLOB 'performDestroy:*'
        UNION ALL
        SELECT id, ts, dur, track_id, name FROM slice WHERE name IN ('activityStart', 'activityResume', 'activityPause', 'activityStop', 'activityDestroy')
      ),
      activity_events AS (
        SELECT
          s.id AS slice_id,
          'activity' AS category,
          CASE
            WHEN instr(s.name, ':') > 0 THEN trim(substr(s.name, instr(s.name, ':') + 1))
            ELSE p.name || ' (' || s.name || ')'
          END AS target,
          t.upid,
          p.pid,
          p.name AS process_name,
          s.ts,
          iif(s.dur > 0, s.dur, 100000000) AS dur,
          s.name AS raw_name
        FROM activity_slices s
        JOIN thread_track tt ON s.track_id = tt.id
        JOIN thread t USING (utid)
        JOIN process p USING (upid)
        WHERE p.name IS NOT NULL
          AND p.name != 'system_server'
          AND s.ts >= ${windowStartNs}
          AND s.ts <= ${windowEndNs}
      ),
      all_raw_events AS (
        SELECT * FROM broadcast_events
        UNION ALL
        SELECT * FROM service_events
        UNION ALL
        SELECT * FROM provider_events
        UNION ALL
        SELECT * FROM job_events
        UNION ALL
        SELECT * FROM activity_events
        ${procStateUnionSql}
      ),
      fw_proc_meta AS (
        SELECT
          upid,
          max(hosting_type) AS hosting_type,
          max(trim(hosting_name, '{}')) AS hosting_name
        FROM __intrinsic_android_track_event_process
        WHERE upid IS NOT NULL
        GROUP BY upid
      )
      SELECT
        row_number() OVER (ORDER BY e.ts, e.upid) AS id,
        e.slice_id,
        e.category,
        coalesce(nullif(trim(e.target), ''), 'unknown') AS target,
        e.upid,
        e.pid,
        p.uid,
        e.process_name,
        e.ts,
        iif(e.dur > 0, e.dur, 1) AS dur,
        e.raw_name,
        fw.hosting_type,
        fw.hosting_name,
        CAST(e.upid AS TEXT) || ':' || e.category AS proc_cat_key,
        CAST(e.upid AS TEXT) || ':' || e.category || ':' || coalesce(nullif(trim(e.target), ''), 'unknown') AS proc_cat_target_key,
        e.category || ':' || coalesce(nullif(trim(e.target), ''), 'unknown') AS cat_target_key,
        iif(e.category != 'proc_state', e.upid, -1) AS active_upid
      FROM all_raw_events e
      LEFT JOIN process p USING (upid)
      LEFT JOIN fw_proc_meta fw USING (upid)
      WHERE e.upid IS NOT NULL;

      DROP TABLE IF EXISTS _android_component_concurrency;
      CREATE PERFETTO TABLE _android_component_concurrency AS
      WITH active_events AS (
        SELECT ts, dur, category
        FROM _android_component_timeline_events
        WHERE category != 'proc_state'
      )
      SELECT
        c.ts,
        coalesce(
          lead(c.ts) OVER (PARTITION BY c.group_name ORDER BY c.ts) - c.ts,
          max(trace_end() - c.ts, 0)
        ) AS dur,
        c.group_name AS category,
        c.value AS concurrency
      FROM intervals_overlap_count_by_group!(
        active_events, ts, dur, category
      ) AS c;

      DROP TABLE IF EXISTS _android_component_proc_lifecycle;
      CREATE PERFETTO TABLE _android_component_proc_lifecycle AS
      WITH track_event_proc AS (
        SELECT
          upid,
          min(fw_start_ts) AS fw_start_ts,
          min(fw_end_ts) AS fw_end_ts
        FROM __intrinsic_android_track_event_process
        WHERE upid IS NOT NULL
        GROUP BY upid
      ),
      process_died_te AS (
        SELECT
          COALESCE(
            extract_arg(s.arg_set_id, 'process_died_event.upid'),
            (
              SELECT max(p2.upid)
              FROM process p2
              WHERE p2.pid = extract_arg(s.arg_set_id, 'process_died_event.pid')
                AND p2.pid IS NOT NULL
            )
          ) AS upid,
          min(s.ts) AS died_ts,
          max(
            iif(
              extract_arg(s.arg_set_id, 'process_died_event.reason') = 'APP_EXIT_REASON_LOW_MEMORY'
              OR extract_arg(s.arg_set_id, 'process_died_event.sub_reason') = 'APP_EXIT_SUBREASON_OOM_KILL',
              1,
              0
            )
          ) AS is_low_mem
        FROM slice s
        WHERE s.name = 'process_died'
          AND s.arg_set_id IS NOT NULL
          AND s.ts >= ${windowStartNs}
        GROUP BY 1
      ),
      start_proc_slices AS (
        SELECT
          trim(substr(name, 12)) AS proc_name,
          min(ts) AS start_proc_ts
        FROM slice
        WHERE name GLOB 'Start proc:*' AND ts >= ${windowStartNs}
        GROUP BY trim(substr(name, 12))
      ),
      bind_app_slices AS (
        SELECT
          t.upid,
          min(s.ts) AS bind_app_ts
        FROM (SELECT ts, track_id FROM slice WHERE name = 'bindApplication' AND ts >= ${windowStartNs}) s
        JOIN thread_track tt ON s.track_id = tt.id
        JOIN thread t USING (utid)
        WHERE t.upid IS NOT NULL
        GROUP BY t.upid
      ),
      lmk_by_upid AS (
        SELECT
          upid,
          min(ts) AS lmk_ts,
          min(oom_score_adj) AS lmk_oom
        FROM android_lmk_events
        WHERE upid IS NOT NULL AND ts >= ${windowStartNs}
        GROUP BY upid
      ),
      lmk_by_pid AS (
        SELECT
          pid,
          min(ts) AS lmk_ts,
          min(oom_score_adj) AS lmk_oom
        FROM android_lmk_events
        WHERE pid IS NOT NULL AND ts >= ${windowStartNs}
        GROUP BY pid
      )
      SELECT
        p.upid,
        p.pid,
        p.name AS process_name,
        min(
          coalesce(tep.fw_start_ts, 9223372036854775807),
          coalesce(sp.start_proc_ts, 9223372036854775807),
          coalesce(ba.bind_app_ts, 9223372036854775807),
          iif(p.start_ts >= ${windowStartNs}, p.start_ts, 9223372036854775807)
        ) AS start_ts,
        coalesce(
          lu.lmk_ts,
          lp.lmk_ts,
          tep.fw_end_ts,
          pd.died_ts,
          iif(p.end_ts >= ${windowStartNs} AND p.end_ts < ${windowEndNs}, p.end_ts, NULL)
        ) AS death_ts,
        iif(
          lu.lmk_ts IS NOT NULL
          OR lp.lmk_ts IS NOT NULL
          OR coalesce(pd.is_low_mem, 0) = 1,
          1,
          0
        ) AS is_lmk,
        coalesce(lu.lmk_oom, lp.lmk_oom) AS oom_score
      FROM process p
      LEFT JOIN track_event_proc tep ON tep.upid = p.upid
      LEFT JOIN process_died_te pd ON pd.upid = p.upid
      LEFT JOIN start_proc_slices sp ON sp.proc_name = p.name
      LEFT JOIN bind_app_slices ba ON ba.upid = p.upid
      LEFT JOIN lmk_by_upid lu ON lu.upid = p.upid
      LEFT JOIN lmk_by_pid lp ON lp.pid = p.pid
      WHERE p.upid != 0 AND p.name IS NOT NULL AND p.name != 'system_server';
    `);
  }

  private async loadCategorySummaries(): Promise<void> {
    const engine = this.trace.engine;
    const targetRes = await engine.query(`
      SELECT
        category,
        target,
        count() AS event_cnt,
        count(DISTINCT upid) AS proc_cnt,
        min(ts) AS first_ts
      FROM _android_component_timeline_events
      GROUP BY category, target
      ORDER BY category, proc_cnt DESC, event_cnt DESC, first_ts ASC
    `);

    const targetsByCat = new Map<ComponentCategory, ComponentTargetOption[]>();
    for (
      const it = targetRes.iter({
        category: STR,
        target: STR,
        event_cnt: NUM,
        proc_cnt: NUM,
        first_ts: LONG,
      });
      it.valid();
      it.next()
    ) {
      const cat = it.category as ComponentCategory;
      let list = targetsByCat.get(cat);
      if (list === undefined) {
        list = [];
        targetsByCat.set(cat, list);
      }
      list.push({
        target: it.target,
        eventCount: it.event_cnt,
        procCount: it.proc_cnt,
        firstTs: it.first_ts,
      });
    }

    const catStatsRes = await engine.query(`
      SELECT
        category,
        count() AS event_cnt,
        count(DISTINCT upid) AS proc_cnt
      FROM _android_component_timeline_events
      GROUP BY category
    `);
    const statsByCat = new Map<
      ComponentCategory,
      {eventCount: number; procCount: number}
    >();
    for (
      const it = catStatsRes.iter({
        category: STR,
        event_cnt: NUM,
        proc_cnt: NUM,
      });
      it.valid();
      it.next()
    ) {
      statsByCat.set(it.category as ComponentCategory, {
        eventCount: it.event_cnt,
        procCount: it.proc_cnt,
      });
    }

    const allStatsRes = await engine.query(`
      SELECT
        count() AS event_cnt,
        count(DISTINCT upid) AS proc_cnt
      FROM _android_component_timeline_events
      WHERE category != 'proc_state'
    `);
    const allRow = allStatsRes.firstRow({event_cnt: NUM, proc_cnt: NUM});

    const catDefs: ReadonlyArray<{
      category: ComponentCategory;
      label: string;
    }> = [
      {category: 'broadcast', label: 'Broadcasts'},
      {category: 'service', label: 'Services'},
      {category: 'provider', label: 'Content Providers'},
      {category: 'job', label: 'Jobs'},
      {category: 'activity', label: 'Activities'},
      {category: 'proc_state', label: 'Process States'},
      {category: 'all', label: 'All Components'},
    ];

    this.categories = catDefs.map(({category, label}) => {
      if (category === 'all') {
        return {
          category,
          label,
          eventCount: allRow.event_cnt,
          procCount: allRow.proc_cnt,
          targets: [],
        };
      }
      const st = statsByCat.get(category) ?? {eventCount: 0, procCount: 0};
      return {
        category,
        label,
        eventCount: st.eventCount,
        procCount: st.procCount,
        targets: targetsByCat.get(category) ?? [],
      };
    });
  }

  private pickInitialSelection(): void {
    const bcastCat = this.categories.find((c) => c.category === 'broadcast');
    if (bcastCat !== undefined && bcastCat.eventCount > 0) {
      this.selectedCategory = 'broadcast';
      const preferredActions = [
        'android.intent.action.BOOT_COMPLETED',
        'android.intent.action.LOCKED_BOOT_COMPLETED',
        'android.intent.action.USER_UNLOCKED',
      ];
      for (const action of preferredActions) {
        if (bcastCat.targets.some((t) => t.target === action)) {
          this.selectedTarget = action;
          return;
        }
      }
      this.selectedTarget = ALL_TARGETS_VALUE;
      return;
    }

    const firstPopulated = this.categories.find((c) => c.eventCount > 0);
    if (firstPopulated !== undefined) {
      this.selectedCategory = firstPopulated.category;
      this.selectedTarget = ALL_TARGETS_VALUE;
    }
  }

  async selectCategory(category: ComponentCategory): Promise<void> {
    this.stop();
    this.selectedCategory = category;
    this.selectedTarget = ALL_TARGETS_VALUE;
    this.stateFilter = null;
    if (category === 'broadcast') {
      const bcastCat = this.categories.find((c) => c.category === 'broadcast');
      if (bcastCat !== undefined) {
        const bootTarget = bcastCat.targets.find(
          (t) => t.target === 'android.intent.action.BOOT_COMPLETED',
        );
        if (bootTarget !== undefined) {
          this.selectedTarget = bootTarget.target;
        }
      }
    }
    await this.loadSelectedDataset();
  }

  async selectTarget(target: string): Promise<void> {
    this.stop();
    this.selectedTarget = target;
    this.stateFilter = null;
    await this.loadSelectedDataset();
  }

  setColorMode(mode: ColorMode): void {
    this.colorMode = mode;
    this.stateFilter = null;
    m.redraw();
  }

  toggleStateFilter(key: ActiveBucketState | ProcStateFamily): void {
    this.stateFilter = this.stateFilter === key ? null : key;
    m.redraw();
  }

  async loadSelectedDataset(): Promise<void> {
    const cacheKey = `${this.selectedCategory}::${this.selectedTarget}`;
    const cached = this.datasetCache.get(cacheKey);
    if (cached !== undefined) {
      this.applyDataset(cached);
      return;
    }

    this.isLoading = true;
    m.redraw();
    try {
      const ds = await this.buildDataset(
        this.selectedCategory,
        this.selectedTarget,
      );
      this.datasetCache.set(cacheKey, ds);
      this.applyDataset(ds);
    } finally {
      this.isLoading = false;
      m.redraw();
    }
  }

  private applyDataset(ds: TimelineDataset): void {
    this.dataset = ds;
    this.shownUpids.clear();
    this.newlyShownUpids.clear();
    this.frame = Math.max(0, Math.min(ds.nb - 1, -ds.b0));
    this.updateShownUpidsForCurrentFrame();
  }

  private getCategoryWhereSql(
    category: ComponentCategory,
    target: string,
  ): string {
    const clauses: string[] = [];
    if (category === 'all') {
      clauses.push(`category != 'proc_state'`);
    } else {
      clauses.push(`category = ${sqliteString(category)}`);
    }
    if (target !== ALL_TARGETS_VALUE) {
      clauses.push(`target = ${sqliteString(target)}`);
    }
    return clauses.join(' AND ');
  }

  private describeDatasetLabels(
    category: ComponentCategory,
    target: string,
  ): {
    title: string;
    t0Label: string;
    nounPlural: string;
    activeVerb: string;
    activeShort: string;
  } {
    const shortTarget =
      target === ALL_TARGETS_VALUE
        ? ''
        : target.includes('.')
          ? (target.split('.').pop() ?? target)
          : target;

    switch (category) {
      case 'broadcast':
        return {
          title:
            target === ALL_TARGETS_VALUE
              ? 'All Broadcasts fan-out, 100 ms at a time'
              : `${shortTarget} fan-out, 100 ms at a time`,
          t0Label:
            target === ALL_TARGETS_VALUE
              ? 'first broadcast dispatch'
              : `first ${shortTarget} dispatch`,
          nounPlural: 'receivers',
          activeVerb: 'In onReceive',
          activeShort: 'onReceive',
        };
      case 'service':
        return {
          title:
            target === ALL_TARGETS_VALUE
              ? 'Services timeline, 100 ms at a time'
              : `Service ${shortTarget}, 100 ms at a time`,
          t0Label:
            target === ALL_TARGETS_VALUE
              ? 'first service event'
              : `first ${shortTarget} event`,
          nounPlural: 'service hosts',
          activeVerb: 'In Service',
          activeShort: 'service',
        };
      case 'provider':
        return {
          title:
            target === ALL_TARGETS_VALUE
              ? 'Content Providers timeline, 100 ms at a time'
              : `Provider ${shortTarget}, 100 ms at a time`,
          t0Label:
            target === ALL_TARGETS_VALUE
              ? 'first content provider call'
              : `first ${shortTarget} call`,
          nounPlural: 'providers',
          activeVerb: 'In Provider',
          activeShort: 'provider',
        };
      case 'job':
        return {
          title:
            target === ALL_TARGETS_VALUE
              ? 'JobScheduler timeline, 100 ms at a time'
              : `Job ${shortTarget}, 100 ms at a time`,
          t0Label:
            target === ALL_TARGETS_VALUE
              ? 'first job execution'
              : `first ${shortTarget} execution`,
          nounPlural: 'job hosts',
          activeVerb: 'In Job',
          activeShort: 'in job',
        };
      case 'activity':
        return {
          title:
            target === ALL_TARGETS_VALUE
              ? 'Activities lifecycle, 100 ms at a time'
              : `Activity ${shortTarget}, 100 ms at a time`,
          t0Label:
            target === ALL_TARGETS_VALUE
              ? 'first activity lifecycle slice'
              : `first ${shortTarget} slice`,
          nounPlural: 'activity apps',
          activeVerb: 'In Activity',
          activeShort: 'activity',
        };
      case 'proc_state':
        return {
          title:
            target === ALL_TARGETS_VALUE
              ? 'Android Process States, 100 ms at a time'
              : `Process State ${target}, 100 ms at a time`,
          t0Label:
            target === ALL_TARGETS_VALUE
              ? 'active trace start'
              : `first ${target} transition`,
          nounPlural: 'processes',
          activeVerb:
            target === ALL_TARGETS_VALUE ? 'Active / FG State' : `In ${target}`,
          activeShort:
            target === ALL_TARGETS_VALUE
              ? 'active'
              : target.toLowerCase().slice(0, 10),
        };
      case 'all':
        return {
          title: 'All Android Components, 100 ms at a time',
          t0Label: 'first component event',
          nounPlural: 'component apps',
          activeVerb: 'In Component',
          activeShort: 'active',
        };
    }
  }

  private async buildDataset(
    category: ComponentCategory,
    target: string,
  ): Promise<TimelineDataset> {
    const engine = this.trace.engine;
    const whereSql = this.getCategoryWhereSql(category, target);
    const labels = this.describeDatasetLabels(category, target);

    const eventsRes = await engine.query(`
      SELECT
        upid,
        pid,
        process_name,
        ts,
        dur,
        target
      FROM _android_component_timeline_events
      WHERE ${whereSql}
      ORDER BY ts ASC, upid ASC
    `);

    const intervalsByUpid = new Map<number, RawInterval[]>();
    const procMeta = new Map<
      number,
      {pid: number; name: string; firstEventTs: bigint}
    >();
    let minEventTs: bigint | null = null;

    for (
      const it = eventsRes.iter({
        upid: NUM,
        pid: NUM,
        process_name: STR,
        ts: LONG,
        dur: LONG,
        target: STR,
      });
      it.valid();
      it.next()
    ) {
      const {upid, pid, process_name: name, ts, dur} = it;
      if (
        category === 'proc_state' &&
        target === ALL_TARGETS_VALUE &&
        classifyProcStateFamily(it.target, 'R') === 'CACHED'
      ) {
        if (!procMeta.has(upid)) {
          procMeta.set(upid, {pid, name, firstEventTs: ts});
          intervalsByUpid.set(upid, []);
        }
        continue;
      }

      if (minEventTs === null || ts < minEventTs) {
        minEventTs = ts;
      }
      const existing = procMeta.get(upid);
      if (existing === undefined) {
        procMeta.set(upid, {pid, name, firstEventTs: ts});
        intervalsByUpid.set(upid, [{startNs: ts, endNs: ts + dur}]);
      } else {
        if (ts < existing.firstEventTs) {
          procMeta.set(upid, {pid, name, firstEventTs: ts});
        }
        intervalsByUpid.get(upid)!.push({startNs: ts, endNs: ts + dur});
      }
    }

    if (procMeta.size === 0) {
      return {
        category,
        target,
        ...labels,
        t0Ns: this.headerMeta.windowStartNs,
        bucketMs: BUCKET_MS,
        lingerMs: LINGER_MS,
        idleThresholdMs: IDLE_THRESHOLD_MS,
        b0: 0,
        nb: 1,
        preMs: 0,
        postMs: 0,
        cpuScaleMs: CPU_SCALE_MS,
        memScaleMb: DEFAULT_MEM_SCALE_MB,
        nslots: 0,
        rows: [],
        counts: [{S: 0, R: 0, C: 0, I: 0, K: 0}],
        procStateCounts: [
          {TOP: 0, FG_SVC: 0, RECEIVER: 0, SERVICE: 0, CACHED: 0, KILLED: 0},
        ],
        totAnonMb: [0],
        totSwapMb: [0],
        totRssMb: [0],
      };
    }

    const t0Ns =
      category === 'proc_state' && target === ALL_TARGETS_VALUE
        ? this.headerMeta.windowStartNs
        : (minEventTs ?? this.headerMeta.windowStartNs);

    const upids = Array.from(procMeta.keys());
    const upidsSql = upids.join(',');

    const lifecycleRes = await engine.query(`
      SELECT
        upid,
        start_ts,
        death_ts,
        is_lmk,
        oom_score
      FROM _android_component_proc_lifecycle
      WHERE upid IN (${upidsSql})
    `);

    const lifecycleByUpid = new Map<
      number,
      {
        startTs: bigint | null;
        deathTs: bigint | null;
        isLmk: boolean;
        oomScore: number | null;
      }
    >();
    for (
      const it = lifecycleRes.iter({
        upid: NUM,
        start_ts: LONG,
        death_ts: LONG_NULL,
        is_lmk: NUM,
        oom_score: NUM_NULL,
      });
      it.valid();
      it.next()
    ) {
      const hasStart = it.start_ts < 9000000000000000000n;
      lifecycleByUpid.set(it.upid, {
        startTs: hasStart ? it.start_ts : null,
        deathTs: it.death_ts,
        isLmk: it.is_lmk === 1,
        oomScore: it.oom_score,
      });
    }

    let minAppearMs = 0;
    const rawProcRows: Array<{
      upid: number;
      pid: number;
      name: string;
      appearMs: number;
      started: boolean;
      firstEventMs: number;
      firstEventTs: bigint;
      deathMs: number | null;
      goneMs: number | null;
      lmk: boolean;
      oom: number | null;
    }> = [];

    for (const [upid, meta] of procMeta.entries()) {
      const lc = lifecycleByUpid.get(upid);
      const firstEventMs = Math.round(Number(meta.firstEventTs - t0Ns) / 1e6);
      const hasValidStart =
        lc?.startTs !== undefined &&
        lc.startTs !== null &&
        lc.startTs <= meta.firstEventTs &&
        meta.firstEventTs - lc.startTs <= 60_000_000_000n;

      const started = Boolean(hasValidStart);
      const appearNs =
        hasValidStart && lc?.startTs !== null && lc?.startTs !== undefined
          ? lc.startTs
          : meta.firstEventTs;
      const appearMs = Math.round(Number(appearNs - t0Ns) / 1e6);
      if (appearMs < minAppearMs) {
        minAppearMs = appearMs;
      }

      const deathMs =
        lc?.deathTs !== undefined && lc.deathTs !== null
          ? Math.round(Number(lc.deathTs - t0Ns) / 1e6)
          : null;
      const goneMs = deathMs !== null ? deathMs + LINGER_MS : null;

      rawProcRows.push({
        upid,
        pid: meta.pid,
        name: meta.name,
        appearMs,
        started,
        firstEventMs,
        firstEventTs: meta.firstEventTs,
        deathMs,
        goneMs,
        lmk: lc?.isLmk ?? false,
        oom: lc?.oomScore ?? null,
      });
    }

    rawProcRows.sort((a, b) => {
      if (a.appearMs !== b.appearMs) return a.appearMs - b.appearMs;
      return a.pid - b.pid;
    });

    const slotFreeAtMs: number[] = [];
    const slotByUpid = new Map<number, number>();
    for (const r of rawProcRows) {
      let chosenSlot = -1;
      for (let s = 0; s < slotFreeAtMs.length; s++) {
        if (slotFreeAtMs[s] <= r.appearMs) {
          chosenSlot = s;
          break;
        }
      }
      if (chosenSlot === -1) {
        chosenSlot = slotFreeAtMs.length;
        slotFreeAtMs.push(
          r.goneMs !== null ? r.goneMs : Number.MAX_SAFE_INTEGER,
        );
      } else {
        slotFreeAtMs[chosenSlot] =
          r.goneMs !== null ? r.goneMs : Number.MAX_SAFE_INTEGER;
      }
      slotByUpid.set(r.upid, chosenSlot);
    }
    const nslots = slotFreeAtMs.length;

    const tracePreMs = Math.max(
      0,
      Math.round(Number(t0Ns - this.headerMeta.windowStartNs) / 1e6),
    );
    const postMs = Math.max(
      BUCKET_MS,
      Math.round(Number(this.headerMeta.windowEndNs - t0Ns) / 1e6),
    );
    const effectivePreMs = Math.min(
      tracePreMs,
      Math.max(300, -minAppearMs + 100),
    );
    const b0 = Math.min(0, -Math.ceil(effectivePreMs / BUCKET_MS));
    const bLast = Math.max(0, Math.floor(postMs / BUCKET_MS));
    const nb = Math.max(1, bLast - b0 + 1);

    const cpuByUpid = new Map<number, Float64Array>();
    for (const upid of upids) {
      cpuByUpid.set(upid, new Float64Array(nb));
    }

    const bucket0StartNs = t0Ns + BigInt(b0) * BUCKET_NS;
    const bucketEndNs = bucket0StartNs + BigInt(nb) * BUCKET_NS;

    const sameBucketCpuRes = await engine.query(`
      SELECT
        t.upid,
        CAST((s.ts - (${bucket0StartNs})) / 100000000 AS INT) AS b_idx,
        SUM(s.dur) AS dur_ns
      FROM sched s
      JOIN thread t USING (utid)
      WHERE s.dur > 0
        AND t.upid IN (${upidsSql})
        AND s.ts >= ${bucket0StartNs}
        AND s.ts + s.dur <= ${bucketEndNs}
        AND CAST((s.ts - (${bucket0StartNs})) / 100000000 AS INT) =
            CAST((s.ts + s.dur - 1 - (${bucket0StartNs})) / 100000000 AS INT)
      GROUP BY t.upid, b_idx
    `);

    for (
      const it = sameBucketCpuRes.iter({
        upid: NUM,
        b_idx: NUM,
        dur_ns: LONG,
      });
      it.valid();
      it.next()
    ) {
      const arr = cpuByUpid.get(it.upid);
      if (arr !== undefined && it.b_idx >= 0 && it.b_idx < nb) {
        arr[it.b_idx] += Number(it.dur_ns) / 1e6;
      }
    }

    const crossBucketCpuRes = await engine.query(`
      SELECT
        t.upid,
        s.ts,
        s.dur
      FROM sched s
      JOIN thread t USING (utid)
      WHERE s.dur > 0
        AND t.upid IN (${upidsSql})
        AND s.ts + s.dur > ${bucket0StartNs}
        AND s.ts < ${bucketEndNs}
        AND CAST((s.ts - (${bucket0StartNs})) / 100000000 AS INT) !=
            CAST((s.ts + s.dur - 1 - (${bucket0StartNs})) / 100000000 AS INT)
    `);

    for (
      const it = crossBucketCpuRes.iter({
        upid: NUM,
        ts: LONG,
        dur: LONG,
      });
      it.valid();
      it.next()
    ) {
      const arr = cpuByUpid.get(it.upid);
      if (arr === undefined) continue;
      const startRelNs = Number(it.ts - bucket0StartNs);
      const endRelNs = Number(it.ts + it.dur - bucket0StartNs);
      const firstIdx = Math.max(0, Math.floor(startRelNs / 1e8));
      const lastIdx = Math.min(nb - 1, Math.floor((endRelNs - 1) / 1e8));
      for (let idx = firstIdx; idx <= lastIdx; idx++) {
        const bLo = idx * 1e8;
        const bHi = (idx + 1) * 1e8;
        const overlapNs = Math.min(endRelNs, bHi) - Math.max(startRelNs, bLo);
        if (overlapNs > 0) {
          arr[idx] += overlapNs / 1e6;
        }
      }
    }

    const countersRes = await engine.query(`
      SELECT
        pct.upid,
        pct.name,
        c.ts,
        c.value
      FROM process_counter_track pct
      JOIN counter c ON c.track_id = pct.id
      WHERE pct.upid IN (${upidsSql})
        AND pct.name IN ('mem.rss.anon', 'mem.rss.file', 'mem.rss.shmem', 'mem.swap', 'oom_score_adj')
        AND c.ts <= ${bucketEndNs}
      ORDER BY pct.upid, c.ts
    `);

    const memSamplesByUpid = new Map<
      number,
      Array<{bIdx: number; name: string; value: number}>
    >();
    for (
      const it = countersRes.iter({
        upid: NUM,
        name: STR,
        ts: LONG,
        value: NUM,
      });
      it.valid();
      it.next()
    ) {
      const rawIdx = Math.floor(Number(it.ts - bucket0StartNs) / 1e8);
      const clampedIdx = Math.max(0, Math.min(nb - 1, rawIdx));
      let list = memSamplesByUpid.get(it.upid);
      if (list === undefined) {
        list = [];
        memSamplesByUpid.set(it.upid, list);
      }
      list.push({bIdx: clampedIdx, name: it.name, value: it.value});
    }

    const procStateByUpid = new Map<number, RawProcStateInterval[]>();
    if (this.headerMeta.hasFrameworkProcState) {
      const psRes = await engine.query(`
        SELECT
          upid,
          ts,
          iif(dur < 0, ${bucketEndNs} - ts, dur) AS dur,
          state
        FROM _android_process_state_intervals
        WHERE upid IN (${upidsSql})
          AND state NOT IN ('NONEXISTENT', 'EXITED')
          AND ts < ${bucketEndNs}
          AND ts + iif(dur < 0, ${bucketEndNs} - ts, dur) > ${bucket0StartNs}
        ORDER BY upid, ts
      `);
      for (
        const it = psRes.iter({
          upid: NUM,
          ts: LONG,
          dur: LONG,
          state: STR,
        });
        it.valid();
        it.next()
      ) {
        let list = procStateByUpid.get(it.upid);
        if (list === undefined) {
          list = [];
          procStateByUpid.set(it.upid, list);
        }
        list.push({
          startNs: it.ts,
          endNs: it.ts + it.dur,
          state: it.state,
          oomScore: null,
        });
      }
    }

    const oomRes = await engine.query(`
      SELECT
        upid,
        ts,
        dur,
        bucket,
        score
      FROM android_oom_adj_intervals
      WHERE upid IN (${upidsSql})
        AND ts < ${bucketEndNs}
        AND ts + dur > ${bucket0StartNs}
      ORDER BY upid, ts
    `);
    const oomIntervalsByUpid = new Map<number, RawProcStateInterval[]>();
    for (
      const it = oomRes.iter({
        upid: NUM,
        ts: LONG,
        dur: LONG,
        bucket: STR,
        score: NUM,
      });
      it.valid();
      it.next()
    ) {
      let list = oomIntervalsByUpid.get(it.upid);
      if (list === undefined) {
        list = [];
        oomIntervalsByUpid.set(it.upid, list);
      }
      list.push({
        startNs: it.ts,
        endNs: it.ts + it.dur,
        state: it.bucket,
        oomScore: it.score,
      });
    }

    let maxObservedMemMb = DEFAULT_MEM_SCALE_MB;
    const rows: ProcessTimelineRow[] = [];

    for (const r of rawProcRows) {
      const cpuArr = cpuByUpid.get(r.upid) ?? new Float64Array(nb);
      const compIntervals = intervalsByUpid.get(r.upid) ?? [];
      const psIntervals =
        procStateByUpid.get(r.upid) ?? oomIntervalsByUpid.get(r.upid) ?? [];

      const rssMb = new Array<number | null>(nb).fill(null);
      const anonMb = new Array<number | null>(nb).fill(null);
      const swapMb = new Array<number>(nb).fill(0);
      const oomScores = new Array<number | null>(nb).fill(null);
      const procStates = new Array<string | null>(nb).fill(null);

      const samples = memSamplesByUpid.get(r.upid) ?? [];
      let sPtr = 0;
      let curAnon: number | null = null;
      let curFile = 0;
      let curShmem = 0;
      let curSwap = 0;
      let curOom: number | null = null;

      for (let f = 0; f < nb; f++) {
        while (sPtr < samples.length && samples[sPtr].bIdx <= f) {
          const smp = samples[sPtr++];
          switch (smp.name) {
            case 'mem.rss.anon':
              curAnon = smp.value;
              break;
            case 'mem.rss.file':
              curFile = smp.value;
              break;
            case 'mem.rss.shmem':
              curShmem = smp.value;
              break;
            case 'mem.swap':
              curSwap = smp.value;
              break;
            case 'oom_score_adj':
              curOom = Math.round(smp.value);
              break;
          }
        }
        if (curAnon !== null) {
          const aMb = Math.round(curAnon / (1024 * 1024));
          const totalRssMb = Math.round(
            (curAnon + curFile + curShmem) / (1024 * 1024),
          );
          const swMb = Math.round(curSwap / (1024 * 1024));
          anonMb[f] = aMb;
          rssMb[f] = Math.max(aMb, totalRssMb);
          swapMb[f] = swMb;
          if (rssMb[f]! + swMb > maxObservedMemMb) {
            maxObservedMemMb = Math.ceil((rssMb[f]! + swMb) / 100) * 100;
          }
        }
        oomScores[f] = curOom;
      }

      const states = new Array<BucketState>(nb);
      const cpuMs = new Array<number>(nb);

      let cPtr = 0;
      let psPtr = 0;

      for (let f = 0; f < nb; f++) {
        const b = b0 + f;
        const loMs = b * BUCKET_MS;
        const hiMs = loMs + BUCKET_MS;
        const loNs = t0Ns + BigInt(b) * BUCKET_NS;
        const hiNs = loNs + BUCKET_NS;

        const cVal = Math.round(cpuArr[f] * 10) / 10;
        cpuMs[f] = cVal;

        while (psPtr < psIntervals.length && psIntervals[psPtr].endNs <= loNs) {
          psPtr++;
        }
        if (psPtr < psIntervals.length && psIntervals[psPtr].startNs < hiNs) {
          procStates[f] = psIntervals[psPtr].state;
          if (psIntervals[psPtr].oomScore !== null) {
            oomScores[f] = psIntervals[psPtr].oomScore;
          }
        }

        if (hiMs <= r.appearMs || (r.goneMs !== null && loMs >= r.goneMs)) {
          states[f] = '-';
          continue;
        }

        if (r.deathMs !== null && loMs >= r.deathMs && loMs < r.goneMs!) {
          states[f] = 'K';
          continue;
        }

        while (
          cPtr < compIntervals.length &&
          compIntervals[cPtr].endNs <= loNs
        ) {
          cPtr++;
        }
        const inComp =
          cPtr < compIntervals.length && compIntervals[cPtr].startNs < hiNs;

        if (inComp) {
          states[f] = 'R';
        } else if (hiMs <= r.firstEventMs) {
          states[f] = 'S';
        } else if (cVal >= IDLE_THRESHOLD_MS) {
          states[f] = 'C';
        } else {
          states[f] = 'I';
        }
      }

      rows.push({
        upid: r.upid,
        pid: r.pid,
        name: r.name,
        appearMs: r.appearMs,
        started: r.started,
        deathMs: r.deathMs,
        goneMs: r.goneMs,
        lmk: r.lmk,
        oom: r.oom,
        slot: slotByUpid.get(r.upid) ?? 0,
        firstEventTs: r.firstEventTs,
        states,
        cpuMs,
        rssMb,
        anonMb,
        swapMb,
        procStates,
        oomScores,
      });
    }

    const counts: Array<Record<ActiveBucketState, number>> = [];
    const procStateCounts: Array<Record<ProcStateFamily, number>> = [];
    const totAnonMb = new Array<number>(nb).fill(0);
    const totSwapMb = new Array<number>(nb).fill(0);
    const totRssMb = new Array<number>(nb).fill(0);

    for (let f = 0; f < nb; f++) {
      const cObj: Record<ActiveBucketState, number> = {
        S: 0,
        R: 0,
        C: 0,
        I: 0,
        K: 0,
      };
      const psObj: Record<ProcStateFamily, number> = {
        TOP: 0,
        FG_SVC: 0,
        RECEIVER: 0,
        SERVICE: 0,
        CACHED: 0,
        KILLED: 0,
      };
      let sumAnon = 0;
      let sumSwap = 0;
      let sumRss = 0;

      for (const r of rows) {
        const st = r.states[f];
        if (st === '-') continue;
        cObj[st]++;
        const fam = classifyProcStateFamily(r.procStates[f], st);
        psObj[fam]++;
        if (st !== 'K') {
          sumAnon += r.anonMb[f] ?? 0;
          sumSwap += r.swapMb[f] ?? 0;
          sumRss += r.rssMb[f] ?? 0;
        }
      }
      counts.push(cObj);
      procStateCounts.push(psObj);
      totAnonMb[f] = sumAnon;
      totSwapMb[f] = sumSwap;
      totRssMb[f] = sumRss;
    }

    return {
      category,
      target,
      ...labels,
      t0Ns,
      bucketMs: BUCKET_MS,
      lingerMs: LINGER_MS,
      idleThresholdMs: IDLE_THRESHOLD_MS,
      b0,
      nb,
      preMs: effectivePreMs,
      postMs,
      cpuScaleMs: CPU_SCALE_MS,
      memScaleMb: maxObservedMemMb,
      nslots,
      rows,
      counts,
      procStateCounts,
      totAnonMb,
      totSwapMb,
      totRssMb,
    };
  }

  goToFrame(f: number): void {
    const ds = this.dataset;
    if (ds === null) return;
    this.frame = Math.max(0, Math.min(ds.nb - 1, f));
    this.updateShownUpidsForCurrentFrame();
    if (this.syncTimeline) {
      const bucketIndex = ds.b0 + this.frame;
      const loNs = ds.t0Ns + BigInt(bucketIndex) * BUCKET_NS;
      const midNs = loNs + BUCKET_NS / 2n;
      this.trace.timeline.hoverCursorTimestamp = Time.fromRaw(midNs);
    }
    m.redraw();
  }

  private updateShownUpidsForCurrentFrame(): void {
    const ds = this.dataset;
    if (ds === null) return;
    const loMs = (ds.b0 + this.frame) * ds.bucketMs;
    this.newlyShownUpids.clear();
    for (const r of ds.rows) {
      const st = r.states[this.frame];
      const vis = st !== '-' && !(r.goneMs !== null && loMs >= r.goneMs);
      if (vis) {
        if (!this.shownUpids.has(r.upid)) {
          this.shownUpids.add(r.upid);
          this.newlyShownUpids.add(r.upid);
        }
      } else {
        this.shownUpids.delete(r.upid);
      }
    }
  }

  stop(): void {
    if (this.timerId !== null) {
      clearInterval(this.timerId);
      this.timerId = null;
      m.redraw();
    }
  }

  togglePlay(): void {
    if (this.timerId !== null) {
      this.stop();
      return;
    }
    const ds = this.dataset;
    if (ds === null) return;
    if (this.frame >= ds.nb - 1) {
      this.goToFrame(0);
    }
    this.timerId = setInterval(() => {
      const curDs = this.dataset;
      if (curDs === null || this.frame >= curDs.nb - 1) {
        this.stop();
        return;
      }
      this.goToFrame(this.frame + 1);
    }, this.speedMs);
    m.redraw();
  }

  setSpeedMs(speedMs: number): void {
    this.speedMs = speedMs;
    if (this.timerId !== null) {
      this.stop();
      this.togglePlay();
    }
  }

  pinProcessTracks(upid: number): void {
    const ws = this.trace.currentWorkspace;
    const procUri = `com.android.ComponentTimeline#proc.${upid}`;
    const procNode = ws.getTrackByUri(procUri);
    if (procNode !== undefined) {
      let cur: typeof procNode.parent = procNode;
      while (cur !== undefined) {
        if (cur.hasChildren) {
          cur.expand();
        }
        cur = cur.parent;
      }
      for (const child of procNode.children) {
        if (child.hasChildren) {
          child.expand();
        }
      }
      if (!procNode.isPinned) {
        procNode.pin();
      }
    }
    const schedNode = ws.getTrackByUri(`/process_${upid}`);
    if (schedNode !== undefined && !schedNode.isPinned) {
      schedNode.pin();
    }
    const psNode =
      ws.getTrackByUri(`com.android.ProcessState#process.${upid}`) ??
      ws.getTrackByUri(`com.android.ComponentTimeline#proc.${upid}.proc_state`);
    if (psNode !== undefined && !psNode.isPinned) {
      psNode.pin();
    }
    m.redraw();
  }

  focusProcessInTimeline(row: ProcessTimelineRow): void {
    const ds = this.dataset;
    const bucketIdx = ds !== null ? ds.b0 + this.frame : 0;
    const loNs =
      ds !== null ? ds.t0Ns + BigInt(bucketIdx) * BUCKET_NS : row.firstEventTs;
    const start = Time.fromRaw(loNs - 500_000_000n);
    const end = Time.fromRaw(loNs + 1_500_000_000n);

    this.pinProcessTracks(row.upid);

    const ws = this.trace.currentWorkspace;
    const catSuffix =
      ds !== null && ds.category !== 'all' ? `.${ds.category}` : '';
    const targetNode =
      (catSuffix !== ''
        ? ws.getTrackByUri(
            `com.android.ComponentTimeline#proc.${row.upid}${catSuffix}`,
          )
        : undefined) ??
      ws.getTrackByUri(`com.android.ComponentTimeline#proc.${row.upid}`) ??
      ws.getTrackByUri(`com.android.ProcessState#process.${row.upid}`) ??
      ws.getTrackByUri(`/process_${row.upid}`);

    this.trace.navigate('#!/viewer');
    this.trace.scrollTo({
      time: {start, end, behavior: 'focus'},
      track:
        targetNode?.uri !== undefined
          ? {uri: targetNode.uri, expandGroup: true}
          : undefined,
    });
  }

  pinActiveCategoryTrack(): void {
    const ds = this.dataset;
    if (ds === null) return;
    const ws = this.trace.currentWorkspace;
    const cat = ds.category === 'all' ? 'broadcast' : ds.category;
    const countNode = ws.getTrackByUri(
      `com.android.ComponentTimeline#count.${cat}`,
    );
    if (countNode !== undefined && !countNode.isPinned) {
      countNode.pin();
    }
    const catUri = `com.android.ComponentTimeline#category.${cat}`;
    const catNode = ws.getTrackByUri(catUri);
    if (ds.target !== ALL_TARGETS_VALUE) {
      const targetUri = `com.android.ComponentTimeline#target.${cat}.${ds.target}`;
      const targetNode = ws.getTrackByUri(targetUri);
      if (targetNode !== undefined && !targetNode.isPinned) {
        targetNode.pin();
      } else if (catNode !== undefined && !catNode.isPinned) {
        catNode.pin();
      }
    } else if (catNode !== undefined && !catNode.isPinned) {
      catNode.pin();
    }
    this.trace.navigate('#!/viewer');
  }
}
