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

import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {TrackNode} from '../../public/workspace';
import {SliceTrack} from '../../components/tracks/slice_track';
import {SourceDataset} from '../../trace_processor/dataset';
import {LONG, NUM, STR} from '../../trace_processor/query_result';

const METRIC_QUERY = `
INCLUDE PERFETTO MODULE chrome.loadline_2;

DROP TABLE IF EXISTS _chrome_loadline2_plugin_metric;
CREATE PERFETTO TABLE _chrome_loadline2_plugin_metric AS
WITH all_generated_slices AS (
  SELECT
    page,
    'visual' AS name,
    story_start AS ts,
    visual_presentation - story_start AS dur
  FROM chrome_loadline2_stages

  UNION ALL
  SELECT
    page,
    'interactive' AS name,
    story_start AS ts,
    interactive_presentation - story_start AS dur
  FROM chrome_loadline2_stages

  UNION ALL
  SELECT
    page,
    page AS name,
    story_start AS ts,
    story_finish - story_start AS dur
  FROM chrome_loadline2_stages
)
SELECT *
FROM all_generated_slices
ORDER BY page, ts, dur DESC;
`;

function generateBreakdownQuery(metric: string): string {
  return `
    DROP TABLE IF EXISTS _chrome_loadline2_plugin_${metric}_breakdown;
    CREATE PERFETTO TABLE _chrome_loadline2_plugin_${metric}_breakdown AS
    WITH all_generated_slices AS (
      SELECT
        page,
        '${metric}' AS name,
        story_start AS ts,
        ${metric}_presentation - story_start AS dur
      FROM chrome_loadline2_stages

      UNION ALL
      SELECT
        page,
        'browser' AS name,
        story_start AS ts,
        start_request - story_start AS dur
      FROM chrome_loadline2_stages

      UNION ALL
      SELECT
        page,
        'network' AS name,
        start_request AS ts,
        end_request - start_request AS dur
      FROM chrome_loadline2_stages

      UNION ALL
      SELECT
        page,
        'process_start' AS name,
        start_request AS ts,
        renderer_ready - start_request AS dur
      FROM chrome_loadline2_stages
      WHERE renderer_ready > start_request

      UNION ALL
      SELECT
        page,
        'renderer' AS name,
        MAX(renderer_ready, end_request) AS ts,
        ${metric}_mark - MAX(renderer_ready, end_request) AS dur
      FROM chrome_loadline2_stages

      UNION ALL
      SELECT
        page,
        '${metric}_raf' AS name,
        ${metric}_raf AS ts,
        0 AS dur
      FROM chrome_loadline2_stages
      WHERE ${metric}_raf IS NOT NULL

      UNION ALL
      SELECT
        page,
        'presentation' AS name,
        ${metric}_mark AS ts,
        ${metric}_presentation - ${metric}_mark AS dur
      FROM chrome_loadline2_stages
    )
    SELECT *
    FROM all_generated_slices
    ORDER BY page, ts, dur DESC;
  `;
}

const METRIC_TRACK_URI = 'org.chromium.LoadLine2#MetricTrack';
const VISUAL_TRACK_URI = 'org.chromium.LoadLine2#VisualBreakdownTrack';
const INTERACTIVE_TRACK_URI =
  'org.chromium.LoadLine2#InteractiveBreakdownTrack';

export default class implements PerfettoPlugin {
  static readonly id = 'org.chromium.LoadLine2';
  static readonly description =
    'Show breakdown of LoadLine 2 metrics on a separate track.';

  async onTraceLoad(trace: Trace): Promise<void> {
    if (!(await isLoadLineTrace(trace))) {
      return;
    }
    prepareTables(trace);
    registerTrack(trace, METRIC_TRACK_URI, '_chrome_loadline2_plugin_metric');
    registerTrack(
      trace,
      VISUAL_TRACK_URI,
      '_chrome_loadline2_plugin_visual_breakdown',
    );
    registerTrack(
      trace,
      INTERACTIVE_TRACK_URI,
      '_chrome_loadline2_plugin_interactive_breakdown',
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
  await trace.engine.query(METRIC_QUERY);
  await trace.engine.query(generateBreakdownQuery('visual'));
  await trace.engine.query(generateBreakdownQuery('interactive'));
}

function registerTrack(trace: Trace, uri: string, src: string): void {
  trace.tracks.registerTrack({
    uri,
    renderer: SliceTrack.create({
      trace,
      uri,
      dataset: new SourceDataset({
        src,
        schema: {
          ts: LONG,
          dur: LONG,
          name: STR,
        },
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

  trace.defaultWorkspace.addChildInOrder(metricTrack);
  metricTrack.addChildLast(visualBreakdownTrack);
  metricTrack.addChildLast(interactiveBreakdownTrack);
}
