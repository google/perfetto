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

import {
  Plugin,
  PluginContext,
  PluginDescriptor,
} from '../../public';

class AndroidCujs implements Plugin {
  onActivate(ctx: PluginContext): void {
    ctx.addCommand({
      id: 'dev.perfetto.AndroidCujs#ListJankCUJs',
      name: 'Run query: Android Jank CUJs',
      callback: () => ctx.viewer.tabs.openQuery(
          `
            SELECT RUN_METRIC('android/android_jank_cuj.sql');
            SELECT RUN_METRIC('android/jank/internal/counters.sql');

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
                  THEN '❌ CANCELED'
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
                  THEN '✅ completed'
                ELSE NULL
                END AS state,
              cuj.name,
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
            ORDER BY state asc, cuj.ts desc;
          `,
          'Android Jank CUJs'),
    });

    ctx.addCommand({
      id: 'dev.perfetto.AndroidCujs#ListLatencyCUJs',
      name: 'Run query: Android Latency CUJs',
      callback: () => ctx.viewer.tabs.openQuery(
          `
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
                  THEN '❌ CANCELED'
                ELSE '✅ completed'
                END AS state,
              cuj.name,
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
            ORDER BY state asc, ts desc;
          `,
          'Android Latency CUJs'),
    });
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'dev.perfetto.AndroidCujs',
  plugin: AndroidCujs,
};
