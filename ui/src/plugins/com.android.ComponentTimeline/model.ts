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

const DEFAULT_BUCKET_MS = 100;
const LINGER_MS = 5000;
const IDLE_THRESHOLD_MS = 1.0;
const DEFAULT_MEM_SCALE_MB = 500;
const MAX_BUCKETS = 1200;

const BUCKET_CANDIDATES_MS = [
  100, 200, 500, 1000, 2000, 5000, 10000, 30000, 60000, 120000,
] as const;

export function chooseBucketMs(spanMs: number): number {
  for (const cand of BUCKET_CANDIDATES_MS) {
    if (Math.ceil(spanMs / cand) <= MAX_BUCKETS) {
      return cand;
    }
  }
  return Math.max(120000, Math.ceil(spanMs / MAX_BUCKETS / 1000) * 1000);
}

export function formatBucketLabel(bucketMs: number): string {
  if (bucketMs >= 60000 && bucketMs % 60000 === 0) {
    return `${bucketMs / 60000} min`;
  }
  if (bucketMs >= 1000) {
    const sec = bucketMs / 1000;
    return Number.isInteger(sec) ? `${sec} s` : `${sec.toFixed(1)} s`;
  }
  return `${bucketMs} ms`;
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
        INCLUDE PERFETTO MODULE intervals.intersect;
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
          trace_start() AS trace_start_ns,
          trace_end() AS trace_end_ns,
          (SELECT count() FROM cpu) AS ncpu,
          (
            (SELECT count() FROM android_lmk_events) +
            (
              SELECT count()
              FROM slice
              WHERE name = 'process_died'
                AND arg_set_id IS NOT NULL
                AND (
                  extract_arg(arg_set_id, 'process_died_event.reason') = 'APP_EXIT_REASON_LOW_MEMORY'
                  OR extract_arg(arg_set_id, 'process_died_event.sub_reason') = 'APP_EXIT_SUBREASON_OOM_KILL'
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
        trace_start_ns: LONG,
        trace_end_ns: LONG,
        ncpu: NUM,
        nlmk: NUM,
        fingerprint: STR_NULL,
        trace_uuid: STR_NULL,
        trace_trigger: STR_NULL,
      });

      const windowStartNs = metaRow.trace_start_ns;
      const windowEndNs =
        metaRow.trace_end_ns > windowStartNs
          ? metaRow.trace_end_ns
          : windowStartNs + 1_000_000_000n;

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
      WITH pid_to_upid AS (
        SELECT pid, max(upid) AS upid
        FROM process
        WHERE pid IS NOT NULL AND pid > 0
        GROUP BY pid
      ),
      fw_slices AS (
        SELECT
          s.id AS slice_id,
          s.ts,
          s.dur AS slice_dur,
          s.name AS raw_name,
          s.arg_set_id
        FROM slice s
        WHERE s.arg_set_id IS NOT NULL
          AND s.name IN (
            'broadcast_delivered',
            'self_broadcast_delivered',
            'service_start',
            'service_stop',
            'service_binding',
            'service_published',
            'service_unbinding',
            'service_connection_removed',
            'service_restart_scheduled',
            'fgs_start',
            'fgs_stop',
            'provider_published',
            'provider_acquired',
            'provider_released',
            'provider_died'
          )
      ),
      broadcast_raw AS (
        SELECT
          s.slice_id,
          s.ts AS finish_ts,
          s.slice_dur,
          s.raw_name,
          extract_arg(s.arg_set_id, 'broadcast_event.action_name') AS target,
          extract_arg(s.arg_set_id, 'broadcast_event.receiver_upid') AS arg_upid,
          extract_arg(s.arg_set_id, 'broadcast_event.receiver_pid') AS arg_pid,
          COALESCE(extract_arg(s.arg_set_id, 'broadcast_event.receive_delay_ms'), 0) AS receive_delay_ms,
          COALESCE(extract_arg(s.arg_set_id, 'broadcast_event.finish_delay_ms'), 0) AS finish_delay_ms
        FROM fw_slices s
        WHERE s.raw_name = 'broadcast_delivered'
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
        LEFT JOIN pid_to_upid pu ON pu.pid = b.arg_pid
        JOIN process p ON p.upid = COALESCE(b.arg_upid, pu.upid)
        WHERE b.target IS NOT NULL
          AND p.name IS NOT NULL
          AND p.name != 'system_server'
          AND b.finish_ts >= ${windowStartNs}
          AND b.finish_ts <= ${windowEndNs}

        UNION ALL

        SELECT
          s.slice_id,
          'broadcast' AS category,
          extract_arg(s.arg_set_id, 'self_broadcast_event.action_name') AS target,
          p.upid,
          p.pid,
          p.name AS process_name,
          s.ts,
          iif(s.slice_dur > 0, s.slice_dur, 100000000) AS dur,
          s.raw_name
        FROM fw_slices s
        LEFT JOIN pid_to_upid pu
          ON pu.pid = extract_arg(s.arg_set_id, 'self_broadcast_event.pid')
        JOIN process p
          ON p.upid = COALESCE(
            extract_arg(s.arg_set_id, 'self_broadcast_event.upid'),
            pu.upid
          )
        WHERE s.raw_name = 'self_broadcast_delivered'
          AND extract_arg(s.arg_set_id, 'self_broadcast_event.action_name') IS NOT NULL
          AND p.name IS NOT NULL
          AND p.name != 'system_server'
          AND s.ts >= ${windowStartNs}
          AND s.ts <= ${windowEndNs}
      ),
      service_raw AS (
        SELECT
          s.slice_id,
          s.ts,
          s.slice_dur,
          s.raw_name,
          COALESCE(
            extract_arg(s.arg_set_id, 'service_state_changed_event.component_name'),
            extract_arg(s.arg_set_id, 'fg_service_state_changed_event.component_name')
          ) AS target,
          COALESCE(
            extract_arg(s.arg_set_id, 'service_state_changed_event.upid'),
            extract_arg(s.arg_set_id, 'fg_service_state_changed_event.upid'),
            pu.upid
          ) AS upid
        FROM fw_slices s
        LEFT JOIN pid_to_upid pu
          ON pu.pid = COALESCE(
            extract_arg(s.arg_set_id, 'service_state_changed_event.pid'),
            extract_arg(s.arg_set_id, 'fg_service_state_changed_event.pid')
          )
        WHERE s.raw_name IN (
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
        SELECT
          s.slice_id,
          s.ts,
          s.slice_dur,
          s.raw_name,
          extract_arg(s.arg_set_id, 'provider_state_changed_event.authority') AS target,
          COALESCE(
            extract_arg(s.arg_set_id, 'provider_state_changed_event.upid'),
            pu.upid
          ) AS upid
        FROM fw_slices s
        LEFT JOIN pid_to_upid pu
          ON pu.pid = extract_arg(s.arg_set_id, 'provider_state_changed_event.pid')
        WHERE s.raw_name IN (
          'provider_published',
          'provider_acquired',
          'provider_released',
          'provider_died'
        )
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
        SELECT id, ts, dur, track_id, name
        FROM slice
        WHERE (
          name GLOB 'performCreate:*'
          OR name GLOB 'performStart:*'
          OR name GLOB 'performResume:*'
          OR name GLOB 'performPause:*'
          OR name GLOB 'performStop:*'
          OR name GLOB 'performDestroy:*'
          OR name IN ('activityStart', 'activityResume', 'activityPause', 'activityStop', 'activityDestroy')
        )
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

      -- Merged non-overlapping component intervals per (upid, category, target)
      -- via interval_merge_overlapping_partitioned!
      DROP TABLE IF EXISTS _android_component_merged_intervals;
      CREATE PERFETTO TABLE _android_component_merged_intervals AS
      SELECT
        row_number() OVER (ORDER BY m.ts, m.upid, m.category, m.target) AS id,
        m.ts,
        m.dur,
        m.upid,
        m.process_name,
        m.category,
        m.target
      FROM interval_merge_overlapping_partitioned!(
        (
          SELECT ts, dur, upid, process_name, category, target
          FROM _android_component_timeline_events
          WHERE category != 'proc_state' AND dur > 0
        ),
        (upid, process_name, category, target)
      ) AS m
      WHERE m.dur > 0;

      -- Total intersecting component overlap across all processes/categories
      -- via intervals_overlap_count!
      DROP TABLE IF EXISTS _android_component_total_overlap;
      CREATE PERFETTO TABLE _android_component_total_overlap AS
      WITH active_events AS (
        SELECT ts, dur
        FROM _android_component_timeline_events
        WHERE category != 'proc_state' AND dur > 0
      )
      SELECT
        c.ts,
        coalesce(
          lead(c.ts) OVER (ORDER BY c.ts) - c.ts,
          max(trace_end() - c.ts, 0)
        ) AS dur,
        c.value AS concurrency
      FROM intervals_overlap_count!(active_events, ts, dur) AS c;

      -- Per-category intersecting component overlap via intervals_overlap_count_by_group!
      DROP TABLE IF EXISTS _android_component_concurrency;
      CREATE PERFETTO TABLE _android_component_concurrency AS
      WITH active_events AS (
        SELECT ts, dur, category
        FROM _android_component_timeline_events
        WHERE category != 'proc_state' AND dur > 0
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

      -- Per-target intersecting component overlap via intervals_overlap_count_by_group!
      DROP TABLE IF EXISTS _android_component_target_concurrency;
      CREATE PERFETTO TABLE _android_component_target_concurrency AS
      WITH active_events AS (
        SELECT ts, dur, cat_target_key
        FROM _android_component_timeline_events
        WHERE category != 'proc_state' AND dur > 0
      )
      SELECT
        c.ts,
        coalesce(
          lead(c.ts) OVER (PARTITION BY c.group_name ORDER BY c.ts) - c.ts,
          max(trace_end() - c.ts, 0)
        ) AS dur,
        substr(c.group_name, 1, instr(c.group_name, ':') - 1) AS category,
        substr(c.group_name, instr(c.group_name, ':') + 1) AS target,
        c.group_name AS cat_target_key,
        c.value AS concurrency
      FROM intervals_overlap_count_by_group!(
        active_events, ts, dur, cat_target_key
      ) AS c;

      -- Per-process intersecting component overlap via intervals_overlap_count_by_group!
      DROP TABLE IF EXISTS _android_component_process_concurrency;
      CREATE PERFETTO TABLE _android_component_process_concurrency AS
      WITH active_events AS (
        SELECT ts, dur, upid
        FROM _android_component_timeline_events
        WHERE category != 'proc_state' AND dur > 0
      )
      SELECT
        c.ts,
        coalesce(
          lead(c.ts) OVER (PARTITION BY c.group_name ORDER BY c.ts) - c.ts,
          max(trace_end() - c.ts, 0)
        ) AS dur,
        c.group_name AS upid,
        c.value AS concurrency
      FROM intervals_overlap_count_by_group!(
        active_events, ts, dur, upid
      ) AS c;

      -- Atomic self-intersection segments where >= 2 components overlap simultaneously
      -- via interval_self_intersect!
      DROP TABLE IF EXISTS _android_component_self_intersect;
      CREATE PERFETTO TABLE _android_component_self_intersect AS
      WITH bounded_merged AS (
        SELECT id, ts, dur, upid, process_name, category, target
        FROM _android_component_merged_intervals
        ORDER BY dur DESC, ts ASC
        LIMIT 20000
      ),
      raw_si AS (
        SELECT
          si.ts,
          si.dur,
          si.group_id,
          count() AS overlap_count,
          count(DISTINCT m.upid) AS proc_count,
          GROUP_CONCAT(DISTINCT m.process_name) AS processes,
          GROUP_CONCAT(DISTINCT m.category || ':' || m.target) AS components
        FROM interval_self_intersect!(bounded_merged) si
        JOIN bounded_merged m ON m.id = si.id
        WHERE si.interval_ends_at_ts = FALSE
          AND si.dur > 0
        GROUP BY si.group_id, si.ts, si.dur
        HAVING overlap_count >= 2
      )
      SELECT
        row_number() OVER (ORDER BY ts) AS id,
        ts,
        dur,
        group_id,
        overlap_count,
        proc_count,
        coalesce(processes, '') AS processes,
        coalesce(components, '') AS components
      FROM raw_si;

      DROP TABLE IF EXISTS _android_component_proc_lifecycle;
      CREATE PERFETTO TABLE _android_component_proc_lifecycle AS
      WITH pid_to_upid AS (
        SELECT pid, max(upid) AS upid
        FROM process
        WHERE pid IS NOT NULL AND pid > 0
        GROUP BY pid
      ),
      track_event_proc AS (
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
            pu.upid
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
        LEFT JOIN pid_to_upid pu
          ON pu.pid = extract_arg(s.arg_set_id, 'process_died_event.pid')
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
    bucketMs: number,
  ): {
    title: string;
    t0Label: string;
    nounPlural: string;
    activeVerb: string;
    activeShort: string;
  } {
    const bLabel = formatBucketLabel(bucketMs);
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
              ? `All Broadcasts fan-out, ${bLabel} at a time`
              : `${shortTarget} fan-out, ${bLabel} at a time`,
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
              ? `Services timeline, ${bLabel} at a time`
              : `Service ${shortTarget}, ${bLabel} at a time`,
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
              ? `Content Providers timeline, ${bLabel} at a time`
              : `Provider ${shortTarget}, ${bLabel} at a time`,
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
              ? `JobScheduler timeline, ${bLabel} at a time`
              : `Job ${shortTarget}, ${bLabel} at a time`,
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
              ? `Activities lifecycle, ${bLabel} at a time`
              : `Activity ${shortTarget}, ${bLabel} at a time`,
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
              ? `Android Process States, ${bLabel} at a time`
              : `Process State ${target}, ${bLabel} at a time`,
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
          title: `All Android Components, ${bLabel} at a time`,
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

    const activeWhereExtra =
      category === 'proc_state' && target === ALL_TARGETS_VALUE
        ? `AND target NOT GLOB '*CACHED*' AND target NOT IN ('HOME', 'LAST_ACTIVITY', 'NONEXISTENT', 'EXITED')`
        : '';

    const procSummaryRes = await engine.query(`
      SELECT
        upid,
        pid,
        process_name,
        min(ts) AS min_ts,
        max(ts + dur) AS max_end_ts,
        min(iif(1 = 1 ${activeWhereExtra}, ts, NULL)) AS first_active_ts,
        max(iif(1 = 1 ${activeWhereExtra}, ts + dur, NULL)) AS last_active_end_ts
      FROM _android_component_timeline_events
      WHERE ${whereSql}
      GROUP BY upid, pid, process_name
      ORDER BY min_ts ASC, upid ASC
    `);

    const procMeta = new Map<
      number,
      {
        pid: number;
        name: string;
        firstEventTs: bigint;
        lastEventEndTs: bigint;
      }
    >();
    let minEventTs: bigint | null = null;
    let maxEventEndTs: bigint | null = null;

    for (
      const it = procSummaryRes.iter({
        upid: NUM,
        pid: NUM,
        process_name: STR,
        min_ts: LONG,
        max_end_ts: LONG,
        first_active_ts: LONG_NULL,
        last_active_end_ts: LONG_NULL,
      });
      it.valid();
      it.next()
    ) {
      const firstTs = it.first_active_ts ?? it.min_ts;
      const lastEndTs = it.last_active_end_ts ?? it.max_end_ts;
      if (it.first_active_ts !== null) {
        if (minEventTs === null || firstTs < minEventTs) {
          minEventTs = firstTs;
        }
        if (maxEventEndTs === null || lastEndTs > maxEventEndTs) {
          maxEventEndTs = lastEndTs;
        }
      }
      procMeta.set(it.upid, {
        pid: it.pid,
        name: it.process_name,
        firstEventTs: firstTs,
        lastEventEndTs: lastEndTs,
      });
    }

    if (procMeta.size === 0) {
      const emptyLabels = this.describeDatasetLabels(
        category,
        target,
        DEFAULT_BUCKET_MS,
      );
      return {
        category,
        target,
        ...emptyLabels,
        t0Ns: this.headerMeta.windowStartNs,
        bucketMs: DEFAULT_BUCKET_MS,
        lingerMs: LINGER_MS,
        idleThresholdMs: IDLE_THRESHOLD_MS,
        b0: 0,
        nb: 1,
        preMs: 0,
        postMs: 0,
        cpuScaleMs: DEFAULT_BUCKET_MS,
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
    const effectivePreMs = Math.min(
      tracePreMs,
      Math.max(300, -minAppearMs + 100),
    );

    const maxEndNs = maxEventEndTs ?? this.headerMeta.windowEndNs;
    const effectiveEndNs =
      this.headerMeta.windowEndNs - maxEndNs <= 60_000_000_000n
        ? this.headerMeta.windowEndNs
        : maxEndNs + 15_000_000_000n < this.headerMeta.windowEndNs
          ? maxEndNs + 15_000_000_000n
          : this.headerMeta.windowEndNs;

    const rawPostMs = Math.max(
      DEFAULT_BUCKET_MS,
      Math.round(Number(effectiveEndNs - t0Ns) / 1e6),
    );
    const totalSpanMs = effectivePreMs + rawPostMs;
    const bucketMs = chooseBucketMs(totalSpanMs);
    const bucketNs = BigInt(bucketMs) * 1_000_000n;
    const labels = this.describeDatasetLabels(category, target, bucketMs);

    const postMs = Math.max(bucketMs, rawPostMs);
    const b0 = Math.min(0, -Math.ceil(effectivePreMs / bucketMs));
    const bLast = Math.max(0, Math.floor(postMs / bucketMs));
    const nb = Math.max(1, Math.min(4096, bLast - b0 + 1));

    const bucket0StartNs = t0Ns + BigInt(b0) * bucketNs;
    const bucketEndNs = bucket0StartNs + BigInt(nb) * bucketNs;

    // Generate fixed-size buckets in SQL WITHOUT any recursive CTEs (using 12-bit cross-join)
    await engine.query(`
      DROP TABLE IF EXISTS _android_component_buckets;
      CREATE PERFETTO TABLE _android_component_buckets AS
      WITH b0(v) AS (VALUES (0), (1)),
           b1(v) AS (SELECT a.v * 2 + b.v FROM b0 a CROSS JOIN b0 b),
           b2(v) AS (SELECT a.v * 4 + b.v FROM b1 a CROSS JOIN b1 b),
           b3(v) AS (SELECT a.v * 16 + b.v FROM b2 a CROSS JOIN b2 b),
           b4(v) AS (SELECT a.v * 16 + b.v FROM b3 a CROSS JOIN b2 b)
      SELECT
        v + 1 AS id,
        v AS b_idx,
        ${bucket0StartNs} + v * ${bucketNs} AS ts,
        ${bucketNs} AS dur
      FROM b4
      WHERE v < ${nb};
    `);

    // 1. Intersect merged component intervals with buckets via C++ _interval_intersect!
    const activeByUpid = new Map<number, Uint8Array>();
    for (const upid of upids) {
      activeByUpid.set(upid, new Uint8Array(nb));
    }

    const compBucketsRes = await engine.query(`
      WITH proc_comp AS (
        SELECT
          m.upid AS id,
          m.ts,
          m.dur
        FROM interval_merge_overlapping_partitioned!(
          (
            SELECT ts, dur, upid
            FROM _android_component_timeline_events
            WHERE (${whereSql})
              ${activeWhereExtra}
              AND dur > 0
              AND ts < ${bucketEndNs}
              AND ts + dur > ${bucket0StartNs}
          ),
          (upid)
        ) m
        WHERE m.dur > 0
      )
      SELECT
        ii.id_1 AS upid,
        ii.id_0 - 1 AS b_idx
      FROM _interval_intersect!((_android_component_buckets, proc_comp), ()) ii
      WHERE ii.dur > 0
      GROUP BY ii.id_1, b_idx
    `);

    for (
      const it = compBucketsRes.iter({upid: NUM, b_idx: NUM});
      it.valid();
      it.next()
    ) {
      const arr = activeByUpid.get(it.upid);
      if (arr !== undefined && it.b_idx >= 0 && it.b_idx < nb) {
        arr[it.b_idx] = 1;
      }
    }

    // 2. Intersect sched slices with buckets via C++ _interval_intersect!
    const cpuByUpid = new Map<number, Float64Array>();
    for (const upid of upids) {
      cpuByUpid.set(upid, new Float64Array(nb));
    }

    const cpuBucketsRes = await engine.query(`
      WITH proc_sched AS (
        SELECT
          t.upid AS id,
          s.ts,
          s.dur
        FROM sched s
        JOIN thread t USING (utid)
        WHERE s.dur > 0
          AND t.upid IN (${upidsSql})
          AND s.ts < ${bucketEndNs}
          AND s.ts + s.dur > ${bucket0StartNs}
      )
      SELECT
        ii.id_1 AS upid,
        ii.id_0 - 1 AS b_idx,
        sum(ii.dur) AS dur_ns
      FROM _interval_intersect!((_android_component_buckets, proc_sched), ()) ii
      WHERE ii.dur > 0
      GROUP BY ii.id_1, b_idx
    `);

    for (
      const it = cpuBucketsRes.iter({
        upid: NUM,
        b_idx: NUM,
        dur_ns: LONG,
      });
      it.valid();
      it.next()
    ) {
      const arr = cpuByUpid.get(it.upid);
      if (arr !== undefined && it.b_idx >= 0 && it.b_idx < nb) {
        arr[it.b_idx] = Number(it.dur_ns) / 1e6;
      }
    }

    // 3. Bucketed memory & oom_score_adj counter samples aggregated in SQL per bucket
    const countersRes = await engine.query(`
      SELECT
        pct.upid,
        pct.name,
        max(0, min(${nb - 1}, CAST((c.ts - (${bucket0StartNs})) / ${bucketNs} AS INT))) AS b_idx,
        c.value
      FROM process_counter_track pct
      JOIN counter c ON c.track_id = pct.id
      WHERE pct.upid IN (${upidsSql})
        AND pct.name IN ('mem.rss.anon', 'mem.rss.file', 'mem.rss.shmem', 'mem.swap', 'oom_score_adj')
        AND c.ts <= ${bucketEndNs}
      GROUP BY pct.upid, pct.name, b_idx
      HAVING c.ts = max(c.ts)
      ORDER BY pct.upid, b_idx
    `);

    const memSamplesByUpid = new Map<
      number,
      Array<{bIdx: number; name: string; value: number}>
    >();
    for (
      const it = countersRes.iter({
        upid: NUM,
        name: STR,
        b_idx: NUM,
        value: NUM,
      });
      it.valid();
      it.next()
    ) {
      let list = memSamplesByUpid.get(it.upid);
      if (list === undefined) {
        list = [];
        memSamplesByUpid.set(it.upid, list);
      }
      list.push({bIdx: it.b_idx, name: it.name, value: it.value});
    }

    // 4. Intersect process state & OOM intervals with buckets via C++ _interval_intersect!
    const procStatesByUpid = new Map<number, Array<string | null>>();
    const oomScoresByUpid = new Map<number, Array<number | null>>();
    for (const upid of upids) {
      procStatesByUpid.set(upid, new Array<string | null>(nb).fill(null));
      oomScoresByUpid.set(upid, new Array<number | null>(nb).fill(null));
    }

    if (this.headerMeta.hasFrameworkProcState) {
      const psBucketsRes = await engine.query(`
        WITH proc_ps AS (
          SELECT
            id,
            ts,
            iif(dur < 0, ${bucketEndNs} - ts, dur) AS dur
          FROM _android_process_state_intervals
          WHERE upid IN (${upidsSql})
            AND state NOT IN ('NONEXISTENT', 'EXITED')
            AND ts < ${bucketEndNs}
            AND ts + iif(dur < 0, ${bucketEndNs} - ts, dur) > ${bucket0StartNs}
        ),
        ii_ps AS (
          SELECT
            psi.upid,
            ii.id_0 - 1 AS b_idx,
            psi.state,
            sum(ii.dur) AS overlap_dur
          FROM _interval_intersect!((_android_component_buckets, proc_ps), ()) ii
          JOIN _android_process_state_intervals psi ON psi.id = ii.id_1
          WHERE ii.dur > 0
          GROUP BY psi.upid, b_idx, psi.state
        ),
        ranked_ps AS (
          SELECT
            upid,
            b_idx,
            state,
            row_number() OVER (PARTITION BY upid, b_idx ORDER BY overlap_dur DESC) AS rn
          FROM ii_ps
        )
        SELECT upid, b_idx, state
        FROM ranked_ps
        WHERE rn = 1
      `);

      for (
        const it = psBucketsRes.iter({
          upid: NUM,
          b_idx: NUM,
          state: STR,
        });
        it.valid();
        it.next()
      ) {
        const arr = procStatesByUpid.get(it.upid);
        if (arr !== undefined && it.b_idx >= 0 && it.b_idx < nb) {
          arr[it.b_idx] = it.state;
        }
      }
    } else {
      const oomBucketsRes = await engine.query(`
        WITH proc_oom AS (
          SELECT
            row_number() OVER (ORDER BY ts, upid) AS id,
            upid,
            ts,
            dur,
            bucket,
            score
          FROM android_oom_adj_intervals
          WHERE upid IN (${upidsSql})
            AND dur > 0
            AND ts < ${bucketEndNs}
            AND ts + dur > ${bucket0StartNs}
        ),
        ii_oom AS (
          SELECT
            po.upid,
            ii.id_0 - 1 AS b_idx,
            po.bucket AS state,
            po.score AS oom_score,
            sum(ii.dur) AS overlap_dur
          FROM _interval_intersect!((_android_component_buckets, proc_oom), ()) ii
          JOIN proc_oom po ON po.id = ii.id_1
          WHERE ii.dur > 0
          GROUP BY po.upid, b_idx, po.bucket, po.score
        ),
        ranked_oom AS (
          SELECT
            upid,
            b_idx,
            state,
            oom_score,
            row_number() OVER (PARTITION BY upid, b_idx ORDER BY overlap_dur DESC) AS rn
          FROM ii_oom
        )
        SELECT upid, b_idx, state, oom_score
        FROM ranked_oom
        WHERE rn = 1
      `);

      for (
        const it = oomBucketsRes.iter({
          upid: NUM,
          b_idx: NUM,
          state: STR,
          oom_score: NUM_NULL,
        });
        it.valid();
        it.next()
      ) {
        const psArr = procStatesByUpid.get(it.upid);
        const oomArr = oomScoresByUpid.get(it.upid);
        if (it.b_idx >= 0 && it.b_idx < nb) {
          if (psArr !== undefined) psArr[it.b_idx] = it.state;
          if (oomArr !== undefined && it.oom_score !== null) {
            oomArr[it.b_idx] = it.oom_score;
          }
        }
      }
    }

    let maxObservedMemMb = DEFAULT_MEM_SCALE_MB;
    const rows: ProcessTimelineRow[] = [];

    for (const r of rawProcRows) {
      const cpuArr = cpuByUpid.get(r.upid) ?? new Float64Array(nb);
      const activeArr = activeByUpid.get(r.upid) ?? new Uint8Array(nb);
      const procStates =
        procStatesByUpid.get(r.upid) ?? new Array<string | null>(nb).fill(null);
      const oomScores =
        oomScoresByUpid.get(r.upid) ?? new Array<number | null>(nb).fill(null);

      const rssMb = new Array<number | null>(nb).fill(null);
      const anonMb = new Array<number | null>(nb).fill(null);
      const swapMb = new Array<number>(nb).fill(0);

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
        if (oomScores[f] === null && curOom !== null) {
          oomScores[f] = curOom;
        }
      }

      const states = new Array<BucketState>(nb);
      const cpuMs = new Array<number>(nb);

      for (let f = 0; f < nb; f++) {
        const b = b0 + f;
        const loMs = b * bucketMs;
        const hiMs = loMs + bucketMs;

        const cVal = Math.round(cpuArr[f] * 10) / 10;
        cpuMs[f] = cVal;

        if (hiMs <= r.appearMs || (r.goneMs !== null && loMs >= r.goneMs)) {
          states[f] = '-';
          continue;
        }

        if (r.deathMs !== null && loMs >= r.deathMs && loMs < r.goneMs!) {
          states[f] = 'K';
          continue;
        }

        if (activeArr[f] === 1) {
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
      bucketMs,
      lingerMs: LINGER_MS,
      idleThresholdMs: IDLE_THRESHOLD_MS,
      b0,
      nb,
      preMs: effectivePreMs,
      postMs,
      cpuScaleMs: bucketMs,
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
      const bucketNs = BigInt(ds.bucketMs) * 1_000_000n;
      const loNs = ds.t0Ns + BigInt(bucketIndex) * bucketNs;
      const midNs = loNs + bucketNs / 2n;
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
    const overlapNode = ws.getTrackByUri(
      `com.android.ComponentTimeline#proc.${upid}.overlap`,
    );
    if (overlapNode !== undefined && !overlapNode.isPinned) {
      overlapNode.pin();
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
    const bucketNs =
      ds !== null ? BigInt(ds.bucketMs) * 1_000_000n : 100_000_000n;
    const loNs =
      ds !== null ? ds.t0Ns + BigInt(bucketIdx) * bucketNs : row.firstEventTs;
    const start = Time.fromRaw(loNs - 5n * bucketNs);
    const end = Time.fromRaw(loNs + 15n * bucketNs);

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
