// Copyright (C) 2024 The Android Open Source Project
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
  CounterTrack,
  type CounterTrackAttrs,
} from '../../components/tracks/counter_track';
import {SourceDataset} from '../../trace_processor/dataset';
import {LONG, NUM, NUM_NULL} from '../../trace_processor/query_result';

export interface TraceProcessorCounterTrackAttrs extends Omit<
  CounterTrackAttrs,
  'sqlSource' | 'selectable'
> {
  /** The trace processor track id. */
  readonly trackId: number;

  /** The display name of the track. */
  readonly trackName: string;

  /** The root table to query. Defaults to 'counter'. */
  readonly rootTable?: string;

  /**
   * SQL source selecting the necessary data.
   * If omitted, defaults to selecting from rootTable where track_id = trackId.
   */
  readonly sqlSource?: string;
}

export class TraceProcessorCounterTrack extends CounterTrack {
  protected readonly trackName: string;
  private readonly dataset: SourceDataset;

  constructor(attrs: TraceProcessorCounterTrackAttrs) {
    const {trackId, trackName, rootTable = 'counter', sqlSource} = attrs;
    const resolvedSqlSource =
      sqlSource ??
      `
      select id, ts, value, arg_set_id
      from ${rootTable}
      where track_id = ${trackId}
    `;
    super({...attrs, sqlSource: resolvedSqlSource});
    this.trackName = trackName;
    const schema = {
      id: NUM,
      ts: LONG,
      value: NUM,
      arg_set_id: NUM_NULL,
    };
    this.dataset =
      sqlSource === undefined
        ? new SourceDataset({
            schema,
            src: rootTable,
            filter: {col: 'track_id', eq: trackId},
          })
        : new SourceDataset({schema, src: sqlSource});
  }

  override getDataset(): SourceDataset {
    return this.dataset;
  }

  protected override getTrackName(): string {
    return this.trackName;
  }
}
