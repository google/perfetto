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
    ];
  }

  async resolveInputId(
    trace: Trace,
    sliceId: number,
  ): Promise<string | undefined> {
    const query = `
      SELECT e.input_event_id AS input_id
      FROM android_input_events e
      JOIN android_frames_choreographer_do_frame chor 
        ON chor.frame_id = e.frame_id
        AND chor.upid = e.upid
      WHERE chor.id = ${sliceId}
      LIMIT 1
    `;
    const result = await trace.engine.query(query);
    const it = result.iter({input_id: STR_NULL});
    return it.valid() ? it.input_id ?? undefined : undefined;
  }
}
