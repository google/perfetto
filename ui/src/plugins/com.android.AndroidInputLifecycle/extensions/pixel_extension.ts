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

export class PixelInputLifecycleExtension implements InputLifecycleExtension {
  readonly id = 'com.google.PixelInputLifecycle';
  readonly requiredModules = ['pixel.input'];

  async isEligible(trace: Trace): Promise<boolean> {
    const result = await trace.engine.query(
      `SELECT 1 FROM slice WHERE name GLOB 'algo->processFrame:*' LIMIT 1`,
    );
    return result.numRows() > 0;
  }

  getSqlJoinSpec(): SqlJoinSpec {
    return {
      tableName: 'pixel_touch_events',
      tableAlias: 'pixel',
      joinOn:
        '((CAST(pixel.in_ts AS INT64) / 1000) * 1000) = ((CAST(a_evt.event_time AS INT64) / 1000) * 1000)',
    };
  }

  getStages(): StageDefinition[] {
    return [
      {
        key: 'pixel_touch_th',
        headerName: 'Pixel Touch Top Half',
        sequenceNumber: 100,
        idField: 'id_pixel_touch_th',
        trackField: 'track_pixel_touch_th',
        tsField: 'ts_pixel_touch_th',
        durField: 'dur_pixel_touch_th',
      },
      {
        key: 'pixel_touch_bh',
        headerName: 'Pixel Touch Bottom Half',
        sequenceNumber: 300,
        idField: 'id_pixel_touch_bh',
        trackField: 'track_pixel_touch_bh',
        tsField: 'ts_pixel_touch_bh',
        durField: 'dur_pixel_touch_bh',
      },
      {
        key: 'pixel_touch',
        headerName: 'Pixel Touch Twoshay',
        sequenceNumber: 500,
        idField: 'id_pixel_touch',
        trackField: 'track_pixel_touch',
        tsField: 'ts_pixel_touch',
        durField: 'dur_pixel_touch',
      },
    ];
  }

  async resolveInputId(
    trace: Trace,
    sliceId: number,
  ): Promise<string | undefined> {
    const stages = this.getStages();
    const whereClauses = stages
      .map((s) => `p.${s.idField} = ${sliceId}`)
      .join(' OR ');
    const query = `
      SELECT e.input_event_id AS input_id
      FROM android_input_events e
      JOIN pixel_touch_events p ON
        ((CAST(p.in_ts AS INT64) / 1000) * 1000) = ((CAST(e.event_time AS INT64) / 1000) * 1000)
      WHERE ${whereClauses}
      LIMIT 1
    `;
    const result = await trace.engine.query(query);
    const it = result.iter({input_id: STR_NULL});
    return it.valid() ? it.input_id ?? undefined : undefined;
  }
}
