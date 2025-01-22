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

import {colorForState} from '../../components/colorizer';
import {
  BASE_ROW,
  BaseSliceTrack,
} from '../../components/tracks/base_slice_track';
import {
  SLICE_LAYOUT_FLAT_DEFAULTS,
  SliceLayout,
} from '../../components/tracks/slice_layout';
import {LONG, NUM, NUM_NULL, STR} from '../../trace_processor/query_result';
import {Slice} from '../../public/track';
import {translateState} from '../../components/sql_utils/thread_state';
import {TrackEventDetails, TrackEventSelection} from '../../public/selection';
import {ThreadStateDetailsPanel} from './thread_state_details_panel';
import {Trace} from '../../public/trace';
import {SourceDataset} from '../../trace_processor/dataset';
import {Time} from '../../base/time';

export const THREAD_STATE_ROW = {
  ...BASE_ROW,
  state: STR,
  ioWait: NUM_NULL,
};

export type ThreadStateRow = typeof THREAD_STATE_ROW;

export class ThreadStateTrack extends BaseSliceTrack<Slice, ThreadStateRow> {
  protected sliceLayout: SliceLayout = {...SLICE_LAYOUT_FLAT_DEFAULTS};
  readonly rootTableName = 'thread_state';

  constructor(
    trace: Trace,
    uri: string,
    private utid: number,
  ) {
    super(trace, uri, THREAD_STATE_ROW);
  }

  getSqlSource(): string {
    // Do not display states: 'S' (sleeping), 'I' (idle kernel thread).
    return `
      select
        id,
        ts,
        dur,
        cpu,
        state,
        io_wait as ioWait,
        0 as depth
      from thread_state
      where
        utid = ${this.utid} and
        state not in ('S', 'I')
    `;
  }

  getDataset(): SourceDataset {
    return new SourceDataset({
      src: 'thread_state',
      schema: {
        id: NUM,
        ts: LONG,
        dur: LONG,
        cpu: NUM_NULL,
        state: STR,
        io_wait: NUM_NULL,
        utid: NUM,
      },
      filter: {
        col: 'utid',
        eq: this.utid,
      },
    });
  }

  rowToSlice(row: ThreadStateRow): Slice {
    const baseSlice = this.rowToSliceBase(row);
    const ioWait = row.ioWait === null ? undefined : !!row.ioWait;
    const title = translateState(row.state, ioWait);
    const color = colorForState(title);
    return {...baseSlice, title, colorScheme: color};
  }

  onUpdatedSlices(slices: Slice[]) {
    for (const slice of slices) {
      slice.isHighlighted = slice === this.hoveredSlice;
    }
  }

  override async getSelectionDetails(
    id: number,
  ): Promise<TrackEventDetails | undefined> {
    // We purposely don't call super.getSelectionDetails(id) here, as we want to
    // be able to return details for sleeping slices, but this super function
    // uses the query in getSqlSource() which omits sleeping slices.
    // See b/371972519.
    // TODO(stevegolton): Use dataset instead.
    const result = await this.trace.engine.query(`
      select
        ts,
        dur
      from thread_state
      where
        utid = ${this.utid}
        and id = ${id}
    `);

    const firstRow = result.maybeFirstRow({ts: LONG, dur: LONG});
    if (!firstRow) return undefined;

    return {
      ts: Time.fromRaw(firstRow.ts),
      dur: firstRow.dur,
      utid: this.utid,
    };
  }

  detailsPanel({eventId}: TrackEventSelection) {
    return new ThreadStateDetailsPanel(this.trace, eventId);
  }
}
