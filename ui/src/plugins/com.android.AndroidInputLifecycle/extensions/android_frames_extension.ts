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

import type {Trace} from '../../../public/trace';
import type {
  InputLifecycleExtension,
  StageDefinition,
  SqlJoinSpec,
} from './interface';
import {STR_NULL} from '../../../trace_processor/query_result';

export class AndroidFramesInputLifecycleExtension
  implements InputLifecycleExtension
{
  readonly id = 'android.InputLifecycleAndroidFrames';
  readonly name = 'Android Frame Pipeline';

  async isEligible(trace: Trace): Promise<boolean> {
    const result = await trace.engine.query(
      `SELECT 1 FROM slice WHERE name GLOB 'Choreographer#doFrame *' LIMIT 1`,
    );
    return result.numRows() > 0;
  }

  getSqlJoinSpec(): SqlJoinSpec {
    return {
      tableName: '_android_input_frames',
      tableAlias: 'frames',
      joinOn: 'frames.frame_id = a_evt.frame_id AND frames.upid = a_evt.upid',
    };
  }

  getStages(): StageDefinition[] {
    return [
      {
        key: 'choreographer_do_frame',
        headerName: 'Choreographer#doFrame',
        sequenceNumber: 5000,
        idField: 'id_do_frame',
        trackField: 'track_do_frame',
        tsField: 'ts_do_frame',
        durField: 'dur_do_frame',
      },
      {
        key: 'draw_frames',
        headerName: 'DrawFrames',
        sequenceNumber: 6000,
        idField: 'id_draw_frames',
        trackField: 'track_draw_frames',
        tsField: 'ts_draw_frames',
        durField: 'dur_draw_frames',
      },
      {
        key: 'sf',
        headerName: 'SurfaceFlinger',
        sequenceNumber: 7000,
        idField: 'id_sf',
        trackField: 'track_sf',
        tsField: 'ts_sf',
        durField: 'dur_sf',
      },
    ];
  }

  async resolveInputId(
    trace: Trace,
    sliceId: number,
  ): Promise<string | undefined> {
    const query = `
      WITH selected_vsync AS (
        -- Case 1: Selected Choreographer slice
        SELECT chor.frame_id AS vsync_id, chor.upid
        FROM android_frames_choreographer_do_frame chor
        WHERE chor.id = ${sliceId}
        UNION ALL
        -- Case 2: Selected DrawFrames slice
        SELECT d.frame_id AS vsync_id, d.upid
        FROM android_frames_draw_frame d
        WHERE d.id = ${sliceId}
        UNION ALL
        -- Case 3: Selected SurfaceFlinger composite slice
        SELECT sf.app_vsync AS vsync_id, sf.app_upid AS upid
        FROM _input_sf_resolved sf
        WHERE sf.id = ${sliceId}
      )
      SELECT e.input_event_id AS input_id
      FROM android_input_events e
      JOIN selected_vsync cv ON e.frame_id = cv.vsync_id AND e.upid = cv.upid
      LIMIT 1
    `;
    const result = await trace.engine.query(query);
    const it = result.iter({input_id: STR_NULL});
    return it.valid() ? it.input_id ?? undefined : undefined;
  }
}
