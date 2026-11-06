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

export const METRIC_QUERY = `
DROP TABLE IF EXISTS _chrome_loadline2_plugin_metric;
CREATE PERFETTO TABLE _chrome_loadline2_plugin_metric AS
WITH all_generated_slices AS (
  SELECT
    page,
    page AS name,
    story_start AS ts,
    story_finish - story_start AS dur,
    0 AS depth
  FROM chrome_loadline2_stages

  UNION ALL
  SELECT
    page,
    'interactive' AS name,
    story_start AS ts,
    interactive_presentation - story_start AS dur,
    1 AS depth
  FROM chrome_loadline2_stages

  UNION ALL
  SELECT
    page,
    'visual' AS name,
    story_start AS ts,
    visual_presentation - story_start AS dur,
    2 AS depth
  FROM chrome_loadline2_stages
)
SELECT
  ROW_NUMBER() OVER (ORDER BY ts, depth, dur DESC) AS id,
  *
FROM all_generated_slices
WHERE ts IS NOT NULL AND dur IS NOT NULL
ORDER BY ts, depth, dur DESC;
`;

export function generateBreakdownQuery(
  metric: 'visual' | 'interactive',
): string {
  return `
    DROP TABLE IF EXISTS _chrome_loadline2_plugin_${metric}_breakdown;
    CREATE PERFETTO TABLE _chrome_loadline2_plugin_${metric}_breakdown AS
    WITH
      stages AS (
        SELECT
          *,
          MAX(
            COALESCE(renderer_ready, end_request),
            COALESCE(end_request, renderer_ready)
          ) AS renderer_start
        FROM chrome_loadline2_stages
      ),
      renderer_summary AS (
        SELECT
          page,
          group_concat(
            subsystem || ': ' || printf('%.1fms (%.1f%%)', dur / 1e6, percentage),
            char(10)
            ORDER BY dur DESC, subsystem
          ) AS summary
        FROM chrome_loadline2_renderer_breakdown
        WHERE metric = '${metric}'
        GROUP BY page
      ),
      presentation_summary AS (
        SELECT
          page,
          group_concat(
            stage || ': ' || printf('%.2fms', dur / 1e6),
            char(10)
            ORDER BY ts, id
          ) AS summary
        FROM chrome_loadline2_presentation_breakdown
        WHERE metric = '${metric}' AND stage_depth = 1
        GROUP BY page
      ),
      all_generated_slices AS (
        SELECT
          s.page,
          '${metric}' AS name,
          s.story_start AS ts,
          s.${metric}_presentation - s.story_start AS dur,
          0 AS depth,
          'metric' AS stage_category,
          printf(
            'Total ${metric} metric: %.2f ms',
            (s.${metric}_presentation - s.story_start) / 1e6
          ) AS details
        FROM stages s

        UNION ALL
        SELECT
          s.page,
          'browser' AS name,
          s.story_start AS ts,
          s.start_request - s.story_start AS dur,
          1 AS depth,
          'top_stage' AS stage_category,
          IIF(
            s.${metric}_presentation > s.story_start,
            printf(
              '%.1f%% of ${metric} metric',
              100.0 * (s.start_request - s.story_start) /
                (s.${metric}_presentation - s.story_start)
            ),
            'n/a (${metric} metric not reached)'
          ) AS details
        FROM stages s

        UNION ALL
        SELECT
          s.page,
          'network' AS name,
          s.start_request AS ts,
          s.end_request - s.start_request AS dur,
          1 AS depth,
          'top_stage' AS stage_category,
          IIF(
            s.${metric}_presentation > s.story_start,
            printf(
              '%.1f%% of ${metric} metric',
              100.0 * (s.end_request - s.start_request) /
                (s.${metric}_presentation - s.story_start)
            ),
            'n/a (${metric} metric not reached)'
          ) AS details
        FROM stages s

        UNION ALL
        SELECT
          s.page,
          'renderer' AS name,
          s.renderer_start AS ts,
          s.${metric}_mark - s.renderer_start AS dur,
          1 AS depth,
          'top_stage' AS stage_category,
          COALESCE(rs.summary, 'Renderer stage') AS details
        FROM stages s
        LEFT JOIN renderer_summary rs USING (page)

        UNION ALL
        SELECT
          s.page,
          'presentation' AS name,
          s.${metric}_mark AS ts,
          s.${metric}_presentation - s.${metric}_mark AS dur,
          1 AS depth,
          'top_stage' AS stage_category,
          COALESCE(ps.summary, 'Presentation stage') AS details
        FROM stages s
        LEFT JOIN presentation_summary ps USING (page)

        UNION ALL
        SELECT
          s.page,
          'process_start' AS name,
          s.start_request AS ts,
          s.renderer_ready - s.start_request AS dur,
          2 AS depth,
          'process_start' AS stage_category,
          'Renderer process startup overlapping network request' AS details
        FROM stages s
        WHERE s.renderer_ready > s.start_request

        UNION ALL
        SELECT
          rs.page,
          rs.subsystem AS name,
          MAX(rs.ts, s.renderer_start) AS ts,
          MIN(rs.ts + rs.dur, s.${metric}_mark) -
            MAX(rs.ts, s.renderer_start) AS dur,
          2 AS depth,
          'renderer_subsystem' AS stage_category,
          printf(
            'Subsystem total in ${metric} renderer stage: %.2f ms (%.1f%%)',
            rb.dur / 1e6,
            rb.percentage
          ) AS details
        FROM chrome_loadline2_renderer_slices rs
        JOIN stages s USING (page)
        LEFT JOIN chrome_loadline2_renderer_breakdown rb
          ON rb.page = rs.page
          AND rb.metric = '${metric}'
          AND rb.subsystem = rs.subsystem
        WHERE
          rs.dur > 0
          AND s.renderer_start IS NOT NULL
          AND rs.ts < s.${metric}_mark
          AND rs.ts + rs.dur > s.renderer_start

        UNION ALL
        SELECT
          pb.page,
          pb.stage AS name,
          pb.ts,
          pb.dur,
          pb.stage_depth + 1 AS depth,
          IIF(
            pb.stage_depth = 1,
            'presentation_stage',
            'presentation_substage'
          ) AS stage_category,
          IIF(
            s.${metric}_presentation > s.${metric}_mark,
            printf(
              '%.1f%% of ${metric} presentation stage',
              100.0 * pb.dur / (s.${metric}_presentation - s.${metric}_mark)
            ),
            'n/a'
          ) AS details
        FROM chrome_loadline2_presentation_breakdown pb
        JOIN stages s USING (page)
        WHERE pb.metric = '${metric}'

        UNION ALL
        SELECT
          s.page,
          '${metric}_raf' AS name,
          s.${metric}_raf AS ts,
          0 AS dur,
          4 AS depth,
          'milestone_raf' AS stage_category,
          'requestAnimationFrame callback timestamp' AS details
        FROM stages s
        WHERE s.${metric}_raf IS NOT NULL
      )
    SELECT
      ROW_NUMBER() OVER (ORDER BY ts, depth, dur DESC) AS id,
      *
    FROM all_generated_slices
    WHERE ts IS NOT NULL AND dur IS NOT NULL
    ORDER BY ts, depth, dur DESC;
  `;
}

export const JS_SCRIPT_LIFECYCLE_QUERY = `
DROP TABLE IF EXISTS _chrome_loadline2_plugin_js_script_lifecycle;
CREATE PERFETTO TABLE _chrome_loadline2_plugin_js_script_lifecycle AS
SELECT
  s.id,
  s.ts,
  s.dur,
  internal_layout(s.ts, s.dur) OVER (
    ORDER BY s.ts, s.dur DESC, s.id
    ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
  ) AS depth,
  s.stage || ': ' || s.script_name AS name,
  s.page,
  s.script_id,
  s.stage,
  s.script_name,
  s.url,
  s.script_kind,
  s.milestone_status,
  s.is_streamed,
  s.not_streamed_reason,
  s.thread_name,
  j.request_dur,
  j.response_dur,
  j.bg_parse_dur,
  j.bg_parse_cpu_dur,
  j.bg_parse_wait_dur,
  j.compile_dur,
  j.execute_dur,
  j.end_to_end_dur,
  CASE
    WHEN s.stage = 'bg_parse' AND s.dur > 0 AND j.bg_parse_cpu_dur IS NOT NULL
      THEN MIN(1.0, MAX(0.0, CAST(j.bg_parse_cpu_dur AS DOUBLE) / s.dur))
    ELSE 1.0
  END AS bg_parse_cpu_ratio
FROM chrome_loadline2_js_script_slices s
JOIN chrome_loadline2_js_scripts j USING (page, script_id)
ORDER BY s.ts, s.dur DESC, s.id;
`;
