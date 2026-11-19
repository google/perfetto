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

import {SliceTrack} from '../../components/tracks/slice_track';
import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import {SourceDataset} from '../../trace_processor/dataset';
import {LONG, NUM, STR} from '../../trace_processor/query_result';
import {registerBreakdownTrack} from './breakdown_track';
import {registerJsScriptLifecycleTrack} from './js_script_track';
import {
  JS_SCRIPT_LIFECYCLE_QUERY,
  METRIC_QUERY,
  generateBreakdownQuery,
} from './queries';

const METRIC_TRACK_URI = 'org.chromium.LoadLine2#MetricTrack';
const VISUAL_TRACK_URI = 'org.chromium.LoadLine2#VisualBreakdownTrack';
const INTERACTIVE_TRACK_URI =
  'org.chromium.LoadLine2#InteractiveBreakdownTrack';
const JS_SCRIPT_LIFECYCLE_TRACK_URI =
  'org.chromium.LoadLine2#JsScriptLifecycleTrack';

export default class LoadLine2Plugin implements PerfettoPlugin {
  static readonly id = 'org.chromium.LoadLine2';
  static readonly description =
    'Breakdown of LoadLine 2 metrics, renderer subsystems, frame presentation, and JS scripts.';

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
      'visual',
      'Breakdown of LoadLine 2 visual metric stages and subsystems.',
    );
    registerBreakdownTrack(
      trace,
      INTERACTIVE_TRACK_URI,
      '_chrome_loadline2_plugin_interactive_breakdown',
      'interactive',
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
      SELECT EXISTS(
        SELECT 1 FROM slice
        WHERE category = 'blink.user_timing' AND name GLOB 'LoadLine2/*/*'
      ) AS res;
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
          id: NUM,
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

function addTracks(trace: Trace): void {
  const metricTrack = new TrackNode({
    uri: METRIC_TRACK_URI,
    name: 'LoadLine 2 metrics',
  });
  trace.defaultWorkspace.addChildInOrder(metricTrack);

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

  metricTrack.addChildLast(visualBreakdownTrack);
  metricTrack.addChildLast(interactiveBreakdownTrack);
  metricTrack.addChildLast(jsScriptLifecycleTrack);
}
