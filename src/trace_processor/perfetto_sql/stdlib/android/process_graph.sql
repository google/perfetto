--
-- Copyright 2026 The Android Open Source Project
--
-- Licensed under the Apache License, Version 2.0 (the "License");
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     https://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.
--

-- Active Android service binding intervals derived from framework
-- service_state track events.
-- One row per (bind_id, host process epoch) where both client and host
-- processes are resolved.
CREATE PERFETTO TABLE _android_service_connections AS
WITH
  bounds AS (
    SELECT
      min(
        coalesce((SELECT min(ts) FROM clock_snapshot), trace_start()),
        trace_start()
      ) AS start_ts,
      9223372036854775807 AS inf_ts
  ),
  raw_slices AS (
    SELECT s.id AS slice_id, s.ts, s.name, s.arg_set_id
    FROM slice AS s
    WHERE
      s.category = 'service_state'
      AND s.name IN (
        'service_binding',
        'service_connection_added',
        'service_connection_removed',
        'service_destroy'
      )
  ),
  decoded_events AS (
    SELECT
      s.slice_id,
      s.ts,
      s.name,
      max(iif(a.key = 'service_state_changed_event.bind_id', a.int_value, NULL)) AS bind_id,
      max(
        iif(
          a.key = 'service_state_changed_event.caller_upid'
          AND a.int_value > 0,
          a.int_value,
          NULL
        )
      ) AS caller_upid,
      max(
        iif(
          a.key = 'service_state_changed_event.upid'
          AND a.int_value > 0,
          a.int_value,
          NULL
        )
      ) AS host_upid,
      max(
        iif(
          a.key = 'service_state_changed_event.component_name',
          a.string_value,
          NULL
        )
      ) AS component_name,
      max(
        iif(
          a.key = 'service_state_changed_event.intent_action'
          AND a.string_value != '',
          a.string_value,
          NULL
        )
      ) AS intent_action,
      group_concat(
        iif(
          a.flat_key IN (
            'service_state_changed_event.bind_flags_32',
            'service_state_changed_event.bind_flags_32_to_64'
          )
          AND a.string_value IS NOT NULL,
          replace(a.string_value, 'SERVICE_BIND_FLAG_', ''),
          NULL
        ),
        ','
      ) AS bind_flags
    FROM raw_slices AS s
    JOIN args AS a USING (arg_set_id)
    GROUP BY
      s.slice_id,
      s.ts,
      s.name
  ),
  bind_summary AS (
    SELECT
      e.bind_id,
      min(iif(e.name = 'service_binding', e.ts, NULL)) AS bind_ts,
      min(iif(e.name = 'service_connection_removed', e.ts, NULL)) AS remove_ts,
      max(e.caller_upid) AS client_upid,
      max(iif(e.name = 'service_connection_removed', e.host_upid, NULL)) AS removed_host_upid,
      max(e.component_name) AS component_name,
      max(e.intent_action) AS intent_action,
      max(iif(e.name = 'service_connection_added', e.bind_flags, NULL)) AS added_flags,
      max(e.bind_flags) AS any_flags
    FROM decoded_events AS e
    WHERE
      e.bind_id IS NOT NULL
      AND e.name != 'service_destroy'
    GROUP BY
      e.bind_id
  ),
  raw_host_attaches AS (
    SELECT
      e.bind_id,
      e.ts,
      e.slice_id,
      e.host_upid,
      lag(e.host_upid) OVER (PARTITION BY e.bind_id ORDER BY e.ts, e.slice_id) AS prev_host_upid,
      row_number() OVER (PARTITION BY e.bind_id ORDER BY e.ts, e.slice_id) AS attach_ord
    FROM decoded_events AS e
    WHERE
      e.bind_id IS NOT NULL
      AND e.name IN ('service_binding', 'service_connection_added')
      AND e.host_upid IS NOT NULL
  ),
  dedup_attaches AS (
    SELECT
      a.bind_id,
      iif(
        a.attach_ord = 1,
        coalesce(bs.bind_ts, (SELECT start_ts FROM bounds)),
        a.ts
      ) AS epoch_ts,
      a.slice_id,
      a.host_upid
    FROM raw_host_attaches AS a
    JOIN bind_summary AS bs USING (bind_id)
    WHERE
      a.prev_host_upid IS NULL
      OR a.prev_host_upid != a.host_upid
  ),
  all_epochs AS (
    SELECT
      d.bind_id,
      d.epoch_ts,
      lead(d.epoch_ts) OVER (
        PARTITION BY
          d.bind_id
        ORDER BY d.epoch_ts, d.slice_id
      ) AS next_epoch_ts,
      d.host_upid
    FROM dedup_attaches AS d
    UNION ALL
    -- Pre-trace bindings that only emitted service_connection_removed.
    SELECT
      bs.bind_id,
      (SELECT start_ts FROM bounds) AS epoch_ts,
      NULL AS next_epoch_ts,
      bs.removed_host_upid AS host_upid
    FROM bind_summary AS bs
    WHERE
      bs.removed_host_upid IS NOT NULL
      AND NOT EXISTS (
        SELECT 1 FROM dedup_attaches AS d WHERE d.bind_id = bs.bind_id
      )
  ),
  epochs_and_destroys AS (
    SELECT
      0 AS is_destroy,
      ep.bind_id,
      ep.epoch_ts AS ts,
      ep.next_epoch_ts,
      ep.host_upid,
      bs.component_name,
      NULL AS destroy_ts
    FROM all_epochs AS ep
    JOIN bind_summary AS bs USING (bind_id)
    WHERE
      bs.client_upid IS NOT NULL
    UNION ALL
    SELECT
      1 AS is_destroy,
      NULL AS bind_id,
      e.ts,
      NULL AS next_epoch_ts,
      NULL AS host_upid,
      e.component_name,
      e.ts AS destroy_ts
    FROM decoded_events AS e
    WHERE
      e.name = 'service_destroy'
      AND e.component_name IS NOT NULL
  ),
  epochs_with_destroy AS (
    SELECT
      x.bind_id,
      x.ts AS epoch_ts,
      x.next_epoch_ts,
      x.host_upid,
      min(x.destroy_ts) OVER (
        PARTITION BY
          x.component_name
        ORDER BY x.ts DESC, x.is_destroy DESC
        ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
      ) AS next_destroy_ts
    FROM epochs_and_destroys AS x
  ),
  resolved_intervals AS (
    SELECT
      ep.bind_id,
      ep.epoch_ts AS ts,
      min(
        coalesce(ep.next_epoch_ts, b.inf_ts),
        coalesce(bs.remove_ts, b.inf_ts),
        coalesce(ep.next_destroy_ts, b.inf_ts),
        coalesce(cp.end_ts, b.inf_ts),
        coalesce(hp.end_ts, b.inf_ts)
      ) AS raw_end_ts,
      b.inf_ts,
      bs.client_upid,
      ep.host_upid AS server_upid,
      bs.component_name,
      bs.intent_action,
      coalesce(bs.added_flags, bs.any_flags) AS bind_flags
    FROM epochs_with_destroy AS ep
    JOIN bind_summary AS bs USING (bind_id)
    JOIN process AS cp ON cp.upid = bs.client_upid
    JOIN process AS hp ON hp.upid = ep.host_upid
    CROSS JOIN bounds AS b
    WHERE
      ep.bind_id IS NOT NULL
  )
SELECT
  row_number() OVER (ORDER BY r.ts, r.bind_id) AS id,
  r.ts,
  iif(r.raw_end_ts = r.inf_ts, -1, r.raw_end_ts - r.ts) AS dur,
  r.bind_id,
  r.client_upid,
  r.server_upid,
  r.component_name,
  r.intent_action,
  r.bind_flags
FROM resolved_intervals AS r
WHERE
  r.raw_end_ts = r.inf_ts
  OR r.raw_end_ts > r.ts;

-- Active Android ContentProvider connection intervals derived from framework
-- provider_state track events.
CREATE PERFETTO TABLE _android_content_provider_connections AS
WITH
  bounds AS (
    SELECT
      min(
        coalesce((SELECT min(ts) FROM clock_snapshot), trace_start()),
        trace_start()
      ) AS start_ts,
      9223372036854775807 AS inf_ts
  ),
  raw_slices AS (
    SELECT s.id AS slice_id, s.ts, s.name, s.arg_set_id
    FROM slice AS s
    WHERE
      s.category = 'provider_state'
      AND s.name IN (
        'provider_acquired',
        'provider_released',
        'provider_published',
        'provider_died'
      )
  ),
  decoded_events AS (
    SELECT
      s.slice_id,
      s.ts,
      s.name,
      max(
        iif(
          a.key = 'provider_state_changed_event.caller_upid'
          AND a.int_value > 0,
          a.int_value,
          NULL
        )
      ) AS caller_upid,
      max(
        iif(
          a.key = 'provider_state_changed_event.upid'
          AND a.int_value > 0,
          a.int_value,
          NULL
        )
      ) AS host_upid,
      max(
        iif(
          a.key = 'provider_state_changed_event.authority',
          a.string_value,
          NULL
        )
      ) AS authority,
      max(
        iif(a.key = 'provider_state_changed_event.is_stable', a.int_value, NULL)
      ) AS is_stable
    FROM raw_slices AS s
    JOIN args AS a USING (arg_set_id)
    GROUP BY
      s.slice_id,
      s.ts,
      s.name
  ),
  pubs_indexed AS (
    SELECT
      e.authority,
      e.ts AS pub_ts,
      e.host_upid AS pub_host_upid,
      row_number() OVER (PARTITION BY e.authority ORDER BY e.ts, e.slice_id) AS pub_ord
    FROM decoded_events AS e
    WHERE
      e.name = 'provider_published'
      AND e.authority IS NOT NULL
      AND e.host_upid IS NOT NULL
  ),
  acq_and_pub AS (
    SELECT
      0 AS is_pub,
      e.slice_id,
      e.ts,
      e.caller_upid,
      e.host_upid,
      e.authority,
      e.is_stable
    FROM decoded_events AS e
    WHERE
      e.name = 'provider_acquired'
      AND e.caller_upid IS NOT NULL
      AND e.authority IS NOT NULL
    UNION ALL
    SELECT
      1 AS is_pub,
      p.pub_ord AS slice_id,
      p.pub_ts AS ts,
      NULL AS caller_upid,
      p.pub_host_upid AS host_upid,
      p.authority,
      NULL AS is_stable
    FROM pubs_indexed AS p
  ),
  acq_with_pub_idx AS (
    SELECT
      x.slice_id,
      x.ts,
      x.caller_upid,
      x.host_upid,
      x.authority,
      x.is_stable,
      sum(x.is_pub) OVER (
        PARTITION BY
          x.authority
        ORDER BY x.ts, x.is_pub, x.slice_id
        ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
      ) AS pubs_before
    FROM acq_and_pub AS x
  ),
  resolved_acquires AS (
    SELECT
      a.slice_id,
      a.ts,
      a.caller_upid,
      coalesce(a.host_upid, p.pub_host_upid) AS host_upid,
      a.authority,
      a.is_stable
    FROM acq_with_pub_idx AS a
    LEFT JOIN pubs_indexed AS p
      ON a.host_upid IS NULL
      AND p.authority = a.authority
      AND p.pub_ord = a.pubs_before + 1
      AND p.pub_ts - a.ts <= 10000000000
    WHERE
      a.caller_upid IS NOT NULL
  ),
  depth_stream AS (
    SELECT
      a.ts,
      a.slice_id,
      1 AS delta,
      a.caller_upid,
      a.host_upid,
      a.authority,
      a.is_stable
    FROM resolved_acquires AS a
    WHERE
      a.host_upid IS NOT NULL
    UNION ALL
    SELECT
      e.ts,
      e.slice_id,
      -1 AS delta,
      e.caller_upid,
      e.host_upid,
      e.authority,
      e.is_stable
    FROM decoded_events AS e
    WHERE
      e.name = 'provider_released'
      AND e.caller_upid IS NOT NULL
      AND e.authority IS NOT NULL
  ),
  depth_calc AS (
    SELECT
      d.*,
      sum(d.delta) OVER (
        PARTITION BY
          d.caller_upid,
          d.authority
        ORDER BY d.ts, d.slice_id
        ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
      ) AS raw_depth
    FROM depth_stream AS d
  ),
  pre_trace_counts AS (
    SELECT caller_upid, authority, max(0, -min(raw_depth)) AS pre_cnt
    FROM depth_calc
    GROUP BY
      caller_upid,
      authority
  ),
  numbered_releases AS (
    SELECT
      d.caller_upid,
      d.authority,
      d.ts AS rel_ts,
      d.host_upid AS rel_host_upid,
      d.is_stable AS rel_is_stable,
      row_number() OVER (
        PARTITION BY
          d.caller_upid,
          d.authority
        ORDER BY d.ts, d.slice_id
      ) AS ord
    FROM depth_calc AS d
    WHERE
      d.delta = -1
  ),
  numbered_acquires AS (
    SELECT
      d.caller_upid,
      d.authority,
      d.ts AS acq_ts,
      d.host_upid AS acq_host_upid,
      d.is_stable AS acq_is_stable,
      p.pre_cnt
      + row_number() OVER (
        PARTITION BY
          d.caller_upid,
          d.authority
        ORDER BY d.ts, d.slice_id
      ) AS ord
    FROM depth_calc AS d
    JOIN pre_trace_counts AS p USING (caller_upid, authority)
    WHERE
      d.delta = 1
    UNION ALL
    -- Pre-trace acquisitions proven by early releases before matching acquires.
    SELECT
      r.caller_upid,
      r.authority,
      (SELECT start_ts FROM bounds) AS acq_ts,
      r.rel_host_upid AS acq_host_upid,
      r.rel_is_stable AS acq_is_stable,
      r.ord
    FROM numbered_releases AS r
    JOIN pre_trace_counts AS p USING (caller_upid, authority)
    WHERE
      r.ord <= p.pre_cnt
      AND r.rel_host_upid IS NOT NULL
  ),
  paired_fifo AS (
    SELECT
      a.acq_ts AS ts,
      r.rel_ts,
      a.caller_upid AS client_upid,
      a.acq_host_upid AS server_upid,
      a.authority,
      a.acq_is_stable AS is_stable
    FROM numbered_acquires AS a
    LEFT JOIN numbered_releases AS r
      ON r.caller_upid = a.caller_upid
      AND r.authority = a.authority
      AND r.ord = a.ord
  ),
  fifo_and_dies AS (
    SELECT
      0 AS is_die,
      f.ts,
      f.rel_ts,
      f.client_upid,
      f.server_upid,
      f.authority,
      f.is_stable,
      NULL AS die_ts
    FROM paired_fifo AS f
    UNION ALL
    SELECT
      1 AS is_die,
      e.ts,
      NULL AS rel_ts,
      NULL AS client_upid,
      NULL AS server_upid,
      e.authority,
      NULL AS is_stable,
      e.ts AS die_ts
    FROM decoded_events AS e
    WHERE
      e.name = 'provider_died'
      AND e.authority IS NOT NULL
  ),
  fifo_with_die AS (
    SELECT
      x.ts,
      x.rel_ts,
      x.client_upid,
      x.server_upid,
      x.authority,
      x.is_stable,
      min(x.die_ts) OVER (
        PARTITION BY
          x.authority
        ORDER BY x.ts DESC, x.is_die DESC
        ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
      ) AS next_die_ts
    FROM fifo_and_dies AS x
  ),
  resolved_intervals AS (
    SELECT
      f.ts,
      min(
        coalesce(f.rel_ts, b.inf_ts),
        coalesce(f.next_die_ts, b.inf_ts),
        coalesce(cp.end_ts, b.inf_ts),
        coalesce(hp.end_ts, b.inf_ts)
      ) AS raw_end_ts,
      b.inf_ts,
      f.client_upid,
      f.server_upid,
      f.authority,
      f.is_stable
    FROM fifo_with_die AS f
    JOIN process AS cp ON cp.upid = f.client_upid
    JOIN process AS hp ON hp.upid = f.server_upid
    CROSS JOIN bounds AS b
    WHERE
      f.client_upid IS NOT NULL
  )
SELECT
  row_number() OVER (ORDER BY r.ts, r.client_upid, r.server_upid) AS id,
  r.ts,
  iif(r.raw_end_ts = r.inf_ts, -1, r.raw_end_ts - r.ts) AS dur,
  r.client_upid,
  r.server_upid,
  r.authority,
  r.is_stable
FROM resolved_intervals AS r
WHERE
  r.raw_end_ts = r.inf_ts
  OR r.raw_end_ts > r.ts;
