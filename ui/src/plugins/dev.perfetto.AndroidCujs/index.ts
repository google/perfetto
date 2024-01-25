// Copyright (C) 2023 The Android Open Source Project
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

import {runQuery} from '../../common/queries';
import {addDebugSliceTrack} from '../../public';
import {
  Plugin,
  PluginContext,
  PluginContextTrace,
  PluginDescriptor,
} from '../../public';


const JANK_CUJ_QUERY_PRECONDITIONS = `
  SELECT RUN_METRIC('android/android_jank_cuj.sql');
  SELECT RUN_METRIC('android/jank/internal/counters.sql');
`;

const JANK_CUJ_QUERY = `
    SELECT
      CASE
        WHEN
          EXISTS(
              SELECT 1
              FROM slice AS cuj_state_marker
                     JOIN track marker_track
                          ON marker_track.id = cuj_state_marker.track_id
              WHERE
                cuj_state_marker.ts >= cuj.ts
                AND cuj_state_marker.ts + cuj_state_marker.dur <= cuj.ts + cuj.dur
                AND
                ( /* e.g. J<CUJ_NAME>#FT#cancel#0 this for backward compatibility */
                      cuj_state_marker.name GLOB(cuj.name || '#FT#cancel*')
                    OR (marker_track.name = cuj.name AND cuj_state_marker.name GLOB 'FT#cancel*')
                  )
            )
          THEN ' ❌ '
        WHEN
          EXISTS(
              SELECT 1
              FROM slice AS cuj_state_marker
                     JOIN track marker_track
                          ON marker_track.id = cuj_state_marker.track_id
              WHERE
                cuj_state_marker.ts >= cuj.ts
                AND cuj_state_marker.ts + cuj_state_marker.dur <= cuj.ts + cuj.dur
                AND
                ( /* e.g. J<CUJ_NAME>#FT#end#0 this for backward compatibility */
                      cuj_state_marker.name GLOB(cuj.name || '#FT#end*')
                    OR (marker_track.name = cuj.name AND cuj_state_marker.name GLOB 'FT#end*')
                  )
            )
          THEN ' ✅ '
        ELSE NULL
        END || cuj.name AS name,
      total_frames,
      missed_app_frames,
      missed_sf_frames,
      sf_callback_missed_frames,
      hwui_callback_missed_frames,
      cuj_layer.layer_name,
      cuj.ts,
      cuj.dur,
      cuj.track_id,
      cuj.slice_id
    FROM slice AS cuj
           JOIN process_track AS pt
                ON cuj.track_id = pt.id
           LEFT JOIN android_jank_cuj jc
                     ON pt.upid = jc.upid AND cuj.name = jc.cuj_slice_name AND cuj.ts = jc.ts
           LEFT JOIN android_jank_cuj_layer_name cuj_layer USING (cuj_id)
           LEFT JOIN android_jank_cuj_counter_metrics USING (cuj_id)
    WHERE cuj.name GLOB 'J<*>'
      AND cuj.dur > 0
`;

const JANK_COLUMNS = [
  'name',
  'total_frames',
  'missed_app_frames',
  'missed_sf_frames',
  'sf_callback_missed_frames',
  'hwui_callback_missed_frames',
  'layer_name',
  'ts',
  'dur',
  'track_id',
  'slice_id',
];

const LATENCY_CUJ_QUERY = `
    SELECT
      CASE
        WHEN
          EXISTS(
              SELECT 1
              FROM slice AS cuj_state_marker
                     JOIN track marker_track
                          ON marker_track.id = cuj_state_marker.track_id
              WHERE
                cuj_state_marker.ts >= cuj.ts
                AND cuj_state_marker.ts + cuj_state_marker.dur <= cuj.ts + cuj.dur
                AND marker_track.name = cuj.name AND (
                    cuj_state_marker.name GLOB 'cancel' 
                    OR cuj_state_marker.name GLOB 'timeout')
            )
          THEN ' ❌ '
        ELSE ' ✅ '
        END || cuj.name AS name,
      cuj.dur / 1e6 as dur_ms,
      cuj.ts,
      cuj.dur,
      cuj.track_id,
      cuj.slice_id
    FROM slice AS cuj
           JOIN process_track AS pt
                ON cuj.track_id = pt.id
    WHERE cuj.name GLOB 'L<*>'
      AND cuj.dur > 0
`;

const LATENCY_COLUMNS = ['name', 'dur_ms', 'ts', 'dur', 'track_id', 'slice_id'];
class AndroidCujs implements Plugin {
  onActivate(_ctx: PluginContext): void {}

  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    ctx.registerCommand({
      id: 'dev.perfetto.AndroidCujs#PinJankCUJs',
      name: 'Pin: Android Jank CUJs',
      callback: () => {
        runQuery(JANK_CUJ_QUERY_PRECONDITIONS, ctx.engine).then(() => {
          addDebugSliceTrack(
            ctx.engine,
            {
              sqlSource: JANK_CUJ_QUERY,
              columns: JANK_COLUMNS,
            },
            'Jank CUJs',
            {ts: 'ts', dur: 'dur', name: 'name'},
            []);
        });
      },
    });

    ctx.registerCommand({
      id: 'dev.perfetto.AndroidCujs#ListJankCUJs',
      name: 'Run query: Android Jank CUJs',
      callback: () => {
        runQuery(JANK_CUJ_QUERY_PRECONDITIONS, ctx.engine)
          .then(
            () => ctx.tabs.openQuery(JANK_CUJ_QUERY, 'Android Jank CUJs'));
      },
    });

    ctx.registerCommand({
      id: 'dev.perfetto.AndroidCujs#PinLatencyCUJs',
      name: 'Pin: Android Latency CUJs',
      callback: () => {
        addDebugSliceTrack(
          ctx.engine,
          {
            sqlSource: LATENCY_CUJ_QUERY,
            columns: LATENCY_COLUMNS,
          },
          'Latency CUJs',
          {ts: 'ts', dur: 'dur', name: 'name'},
          []);
      },
    });

    ctx.registerCommand({
      id: 'dev.perfetto.AndroidCujs#ListLatencyCUJs',
      name: 'Run query: Android Latency CUJs',
      callback: () =>
        ctx.tabs.openQuery(LATENCY_CUJ_QUERY, 'Android Latency CUJs'),
    });
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'dev.perfetto.AndroidCujs',
  plugin: AndroidCujs,
};
