// Copyright (C) 2025 The Android Open Source Project
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
import {exists} from '../../base/utils';
import {getColorForSlice} from '../../components/colorizer';
import {formatDuration} from '../../components/time_utils';
import {SliceTrack, renderTooltip} from '../../components/tracks/slice_track';
import {DurationWidget} from '../../components/widgets/duration';
import {Timestamp} from '../../components/widgets/timestamp';
import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import {SourceDataset} from '../../trace_processor/dataset';
import {
  LONG,
  LONG_NULL,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';

const METRIC_QUERY = `
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
SELECT *
FROM all_generated_slices
WHERE ts IS NOT NULL AND dur IS NOT NULL
ORDER BY ts, depth, dur DESC;
`;

function generateBreakdownQuery(metric: string): string {
  return `
    DROP TABLE IF EXISTS _chrome_loadline2_plugin_${metric}_breakdown;
    CREATE PERFETTO TABLE _chrome_loadline2_plugin_${metric}_breakdown AS
    WITH
      renderer_summary AS (
        SELECT
          page,
          group_concat(
            subsystem || ': ' || printf('%.1fms (%.1f%%)', dur / 1e6, percentage),
            char(10)
          ) AS summary
        FROM (
          SELECT page, subsystem, dur, percentage
          FROM chrome_loadline2_renderer_breakdown
          WHERE metric = '${metric}'
          ORDER BY page, dur DESC
        )
        GROUP BY page
      ),
      presentation_summary AS (
        SELECT
          page,
          group_concat(
            stage || ': ' || printf('%.2fms', dur / 1e6),
            char(10)
          ) AS summary
        FROM (
          SELECT page, stage, dur
          FROM chrome_loadline2_presentation_breakdown
          WHERE metric = '${metric}' AND stage_depth = 1
          ORDER BY page, ts
        )
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
        FROM chrome_loadline2_stages s

        UNION ALL
        SELECT
          s.page,
          'browser' AS name,
          s.story_start AS ts,
          s.start_request - s.story_start AS dur,
          1 AS depth,
          'top_stage' AS stage_category,
          printf(
            '%.1f%% of ${metric} metric',
            100.0 * (s.start_request - s.story_start) /
              NULLIF(s.${metric}_presentation - s.story_start, 0)
          ) AS details
        FROM chrome_loadline2_stages s

        UNION ALL
        SELECT
          s.page,
          'network' AS name,
          s.start_request AS ts,
          s.end_request - s.start_request AS dur,
          1 AS depth,
          'top_stage' AS stage_category,
          printf(
            '%.1f%% of ${metric} metric',
            100.0 * (s.end_request - s.start_request) /
              NULLIF(s.${metric}_presentation - s.story_start, 0)
          ) AS details
        FROM chrome_loadline2_stages s

        UNION ALL
        SELECT
          s.page,
          'renderer' AS name,
          MAX(s.renderer_ready, s.end_request) AS ts,
          s.${metric}_mark - MAX(s.renderer_ready, s.end_request) AS dur,
          1 AS depth,
          'top_stage' AS stage_category,
          COALESCE(rs.summary, 'Renderer stage') AS details
        FROM chrome_loadline2_stages s
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
        FROM chrome_loadline2_stages s
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
        FROM chrome_loadline2_stages s
        WHERE s.renderer_ready > s.start_request

        UNION ALL
        SELECT
          rs.page,
          rs.subsystem AS name,
          MAX(rs.ts, MAX(s.renderer_ready, s.end_request)) AS ts,
          MIN(rs.ts + rs.dur, s.${metric}_mark) -
            MAX(rs.ts, MAX(s.renderer_ready, s.end_request)) AS dur,
          2 AS depth,
          'renderer_subsystem' AS stage_category,
          printf(
            'Subsystem total in ${metric} renderer stage: %.2f ms (%.1f%%)',
            rb.dur / 1e6,
            rb.percentage
          ) AS details
        FROM chrome_loadline2_renderer_slices rs
        JOIN chrome_loadline2_stages s USING (page)
        LEFT JOIN chrome_loadline2_renderer_breakdown rb
          ON rb.page = rs.page
          AND rb.metric = '${metric}'
          AND rb.subsystem = rs.subsystem
        WHERE
          rs.dur > 0
          AND rs.ts < s.${metric}_mark
          AND rs.ts + rs.dur > MAX(s.renderer_ready, s.end_request)

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
          printf(
            '%.1f%% of ${metric} presentation stage',
            100.0 * pb.dur / NULLIF(s.${metric}_presentation - s.${metric}_mark, 0)
          ) AS details
        FROM chrome_loadline2_presentation_breakdown pb
        JOIN chrome_loadline2_stages s USING (page)
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
        FROM chrome_loadline2_stages s
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

const JS_SCRIPT_LIFECYCLE_QUERY = `
DROP TABLE IF EXISTS _chrome_loadline2_plugin_js_script_lifecycle;
CREATE PERFETTO TABLE _chrome_loadline2_plugin_js_script_lifecycle AS
SELECT
  s.id,
  s.ts,
  s.dur,
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
JOIN chrome_loadline2_js_scripts j USING (script_id)
ORDER BY s.ts, s.dur DESC, s.id;
`;

const METRIC_TRACK_URI = 'org.chromium.LoadLine2#MetricTrack';
const VISUAL_TRACK_URI = 'org.chromium.LoadLine2#VisualBreakdownTrack';
const INTERACTIVE_TRACK_URI =
  'org.chromium.LoadLine2#InteractiveBreakdownTrack';
const JS_SCRIPT_LIFECYCLE_TRACK_URI =
  'org.chromium.LoadLine2#JsScriptLifecycleTrack';

export default class LoadLine2Plugin implements PerfettoPlugin {
  static readonly id = 'org.chromium.LoadLine2';
  static readonly description =
    'Show breakdown of LoadLine 2 metrics on a separate track.';

  async onTraceLoad(trace: Trace): Promise<void> {
    if (!(await isLoadLineTrace(trace))) {
      return;
    }
    await prepareTables(trace);
    registerMetricTrack(
      trace,
      METRIC_TRACK_URI,
      '_chrome_loadline2_plugin_metric',
    );
    registerBreakdownTrack(
      trace,
      VISUAL_TRACK_URI,
      '_chrome_loadline2_plugin_visual_breakdown',
      'Breakdown of LoadLine 2 visual metric stages and subsystems.',
    );
    registerBreakdownTrack(
      trace,
      INTERACTIVE_TRACK_URI,
      '_chrome_loadline2_plugin_interactive_breakdown',
      'Breakdown of LoadLine 2 interactive metric stages and subsystems.',
    );
    registerJsScriptLifecycleTrack(
      trace,
      JS_SCRIPT_LIFECYCLE_TRACK_URI,
      '_chrome_loadline2_plugin_js_script_lifecycle',
    );
    addTracks(trace);
  }
}

async function isLoadLineTrace(trace: Trace): Promise<boolean> {
  const queryRes = await trace.engine.query(`
      SELECT EXISTS(SELECT * FROM slice WHERE name GLOB 'LoadLine2/*') AS res;
    `);
  const it = queryRes.iter({res: NUM});
  return it.res > 0;
}

async function prepareTables(trace: Trace): Promise<void> {
  await trace.engine.query('INCLUDE PERFETTO MODULE chrome.loadline_2;');
  await trace.engine.query(METRIC_QUERY);
  await trace.engine.query(generateBreakdownQuery('visual'));
  await trace.engine.query(generateBreakdownQuery('interactive'));
  await trace.engine.query(JS_SCRIPT_LIFECYCLE_QUERY);
}

function registerMetricTrack(trace: Trace, uri: string, src: string): void {
  trace.tracks.registerTrack({
    uri,
    description: 'High-level story, interactive, and visual durations.',
    renderer: SliceTrack.create({
      trace,
      uri,
      dataset: new SourceDataset({
        src,
        schema: {
          ts: LONG,
          dur: LONG,
          depth: NUM,
          name: STR,
          page: STR,
        },
      }),
    }),
  });
}

function registerBreakdownTrack(
  trace: Trace,
  uri: string,
  src: string,
  description: string,
): void {
  trace.tracks.registerTrack({
    uri,
    description,
    renderer: SliceTrack.create({
      trace,
      uri,
      initialMaxDepth: 4,
      dataset: new SourceDataset({
        src,
        schema: {
          id: NUM,
          ts: LONG,
          dur: LONG,
          depth: NUM,
          name: STR,
          page: STR,
          stage_category: STR,
          details: STR,
        },
      }),
      tooltip: (slice) =>
        renderTooltip(trace, slice, {
          extras: m('div', slice.row.details.replaceAll('\n', ', ')),
        }),
      detailsPanel: (row) => ({
        render: () => {
          const isMultiLineStageSummary =
            row.name === 'renderer' || row.name === 'presentation';
          const breakdownItems = isMultiLineStageSummary
            ? row.details.split('\n').map((entry) => {
                const sepIdx = entry.indexOf(': ');
                if (sepIdx !== -1) {
                  return m(TreeNode, {
                    left: entry.slice(0, sepIdx),
                    right: entry.slice(sepIdx + 2),
                  });
                }
                return m(TreeNode, {left: entry});
              })
            : [m(TreeNode, {left: 'Summary', right: row.details})];

          return m(
            DetailsShell,
            {
              title: 'LoadLine 2 Breakdown Slice',
              description: `${row.page} • ${row.name}`,
            },
            m(
              GridLayout,
              m(
                Section,
                {title: 'Stage Details'},
                m(Tree, [
                  m(TreeNode, {left: 'Name', right: row.name}),
                  m(TreeNode, {left: 'Page', right: row.page}),
                  m(TreeNode, {left: 'Category', right: row.stage_category}),
                  m(TreeNode, {
                    left: 'Start time',
                    right: m(Timestamp, {trace, ts: Time.fromRaw(row.ts)}),
                  }),
                  m(TreeNode, {
                    left: 'Duration',
                    right: m(DurationWidget, {trace, dur: row.dur}),
                  }),
                  m(TreeNode, {left: 'Depth', right: row.depth}),
                ]),
              ),
              m(
                Section,
                {title: 'Critical Path Breakdown'},
                m(Tree, breakdownItems),
              ),
            ),
          );
        },
      }),
    }),
  });
}

function renderNullableDuration(trace: Trace, dur: bigint | null): m.Children {
  return dur !== null ? m(DurationWidget, {trace, dur}) : 'N/A';
}

function registerJsScriptLifecycleTrack(
  trace: Trace,
  uri: string,
  src: string,
): void {
  trace.tracks.registerTrack({
    uri,
    description:
      'End-to-end JavaScript request, response, background parse, compile, and execute stages.',
    renderer: SliceTrack.create({
      trace,
      uri,
      dataset: new SourceDataset({
        src,
        schema: {
          id: NUM,
          ts: LONG,
          dur: LONG,
          name: STR,
          page: STR,
          script_id: NUM,
          stage: STR,
          script_name: STR,
          url: STR_NULL,
          script_kind: STR,
          milestone_status: STR,
          is_streamed: NUM,
          not_streamed_reason: STR_NULL,
          thread_name: STR_NULL,
          request_dur: LONG_NULL,
          response_dur: LONG_NULL,
          bg_parse_dur: LONG_NULL,
          bg_parse_cpu_dur: LONG_NULL,
          bg_parse_wait_dur: LONG_NULL,
          compile_dur: LONG_NULL,
          execute_dur: LONG_NULL,
          end_to_end_dur: LONG_NULL,
          bg_parse_cpu_ratio: NUM_NULL,
        },
      }),
      sliceSubtitle: (row) => `${row.script_kind} • ${row.milestone_status}`,
      colorizer: (row) =>
        getColorForSlice(`${row.page}:${row.script_id}:${row.script_name}`),
      fillRatio: (row) => row.bg_parse_cpu_ratio ?? 1,
      tooltip: (slice) =>
        renderTooltip(trace, slice, {
          extras: [
            m(
              'div',
              `${slice.row.script_kind} • ${slice.row.milestone_status}`,
            ),
            slice.row.end_to_end_dur !== null &&
              m(
                'div',
                `End-to-end: ${formatDuration(trace, slice.row.end_to_end_dur)}`,
              ),
            slice.row.bg_parse_dur !== null &&
              m(
                'div',
                `BG parse: ${formatDuration(trace, slice.row.bg_parse_dur)}`,
              ),
          ],
        }),
      detailsPanel: (row) => ({
        render: () =>
          m(
            DetailsShell,
            {
              title: 'JS Script Lifecycle',
              description: `${row.page} • ${row.script_name} (${row.stage})`,
            },
            m(
              GridLayout,
              m(
                Section,
                {title: 'Script & Slice Details'},
                m(Tree, [
                  m(TreeNode, {left: 'Script name', right: row.script_name}),
                  m(TreeNode, {left: 'Active stage', right: row.stage}),
                  m(TreeNode, {left: 'Page', right: row.page}),
                  m(TreeNode, {left: 'Script kind', right: row.script_kind}),
                  m(TreeNode, {
                    left: 'Milestone cutoff',
                    right: row.milestone_status,
                  }),
                  m(TreeNode, {
                    left: 'Start time',
                    right: m(Timestamp, {trace, ts: Time.fromRaw(row.ts)}),
                  }),
                  m(TreeNode, {
                    left: 'Stage duration',
                    right: m(DurationWidget, {trace, dur: row.dur}),
                  }),
                  m(TreeNode, {
                    left: 'Thread',
                    right: row.thread_name ?? 'N/A',
                  }),
                  m(TreeNode, {
                    left: 'Streamed',
                    right:
                      row.is_streamed !== 0
                        ? 'Yes'
                        : `No (${row.not_streamed_reason ?? 'n/a'})`,
                  }),
                  exists(row.url) && m(TreeNode, {left: 'URL', right: row.url}),
                ]),
              ),
              m(
                Section,
                {title: 'End-to-End Lifecycle Timings'},
                m(Tree, [
                  m(TreeNode, {
                    left: 'End-to-end duration',
                    right: renderNullableDuration(trace, row.end_to_end_dur),
                  }),
                  m(TreeNode, {
                    left: '1. Network request',
                    right: renderNullableDuration(trace, row.request_dur),
                  }),
                  m(TreeNode, {
                    left: '2. Network response',
                    right: renderNullableDuration(trace, row.response_dur),
                  }),
                  m(TreeNode, {
                    left: '3. Background parse',
                    right:
                      row.bg_parse_dur !== null
                        ? m(
                            'span',
                            m(DurationWidget, {trace, dur: row.bg_parse_dur}),
                            ' (Parsing: ',
                            m(DurationWidget, {
                              trace,
                              dur: row.bg_parse_cpu_dur ?? 0n,
                            }),
                            ', Waiting: ',
                            m(DurationWidget, {
                              trace,
                              dur: row.bg_parse_wait_dur ?? 0n,
                            }),
                            ')',
                          )
                        : 'N/A',
                  }),
                  m(TreeNode, {
                    left: '4. Compile',
                    right: renderNullableDuration(trace, row.compile_dur),
                  }),
                  m(TreeNode, {
                    left: '5. Execute',
                    right: renderNullableDuration(trace, row.execute_dur),
                  }),
                  m(TreeNode, {left: 'Script ID', right: row.script_id}),
                ]),
              ),
            ),
          ),
      }),
    }),
  });
}

function addTracks(trace: Trace): void {
  const metricTrack = new TrackNode({
    uri: METRIC_TRACK_URI,
    name: 'LoadLine 2 metrics',
  });
  const visualBreakdownTrack = new TrackNode({
    uri: VISUAL_TRACK_URI,
    name: 'Visual metric breakdown',
  });
  const interactiveBreakdownTrack = new TrackNode({
    uri: INTERACTIVE_TRACK_URI,
    name: 'Interactive metric breakdown',
  });
  const jsScriptLifecycleTrack = new TrackNode({
    uri: JS_SCRIPT_LIFECYCLE_TRACK_URI,
    name: 'JS script lifecycle',
  });

  trace.defaultWorkspace.addChildInOrder(metricTrack);
  metricTrack.addChildLast(visualBreakdownTrack);
  metricTrack.addChildLast(interactiveBreakdownTrack);
  metricTrack.addChildLast(jsScriptLifecycleTrack);
}
