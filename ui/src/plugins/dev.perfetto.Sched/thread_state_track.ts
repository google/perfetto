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

import {HSLColor} from '../../base/color';
import type {ColorScheme} from '../../base/color_scheme';
import {ColorVariant, SliceTrack} from '../../components/tracks/slice_track';
import type {Trace} from '../../public/trace';
import {SourceDataset} from '../../trace_processor/dataset';
import {LONG, NUM, NUM_NULL, STR} from '../../trace_processor/query_result';
import {colorForThreadState} from './common';
import {ThreadStateDetailsPanel} from './thread_state_details_panel';

const COLOR_TRANSPARENT = new HSLColor([0, 0, 0], 0.0);
const COLOR_SCHEME_SLEEPING_IDLE: ColorScheme = {
  base: COLOR_TRANSPARENT,
  variant: new HSLColor([0, 0, 50], 0.2),
  disabled: COLOR_TRANSPARENT,
  textBase: COLOR_TRANSPARENT,
  textVariant: COLOR_TRANSPARENT,
  textDisabled: COLOR_TRANSPARENT,
};

export function createThreadStateTrack(
  trace: Trace,
  uri: string,
  utid: number,
) {
  let hoveredSliceId: number | undefined;
  return SliceTrack.create({
    trace,
    uri,
    dataset: new SourceDataset({
      schema: {
        id: NUM,
        ts: LONG,
        dur: LONG,
        layer: NUM,
        ucpu: NUM_NULL,
        utid: NUM,
        state: STR,
        depth: NUM,
      },
      select: {
        id: 'id',
        ts: 'ts',
        dur: 'dur',
        ucpu: 'ucpu',
        utid: 'utid',
        state: 'sched_state_io_to_human_readable_string(state, io_wait)',
        depth: '0',
        // Move sleeping and idle slices to the back layer, others on top
        layer: "CASE WHEN state IN ('S', 'I') THEN 0 ELSE 1 END",
      },
      src: 'thread_state',
      filter: {
        col: 'utid',
        eq: utid,
      },
    }),
    // Make thread slice tracks a little shorter in height.
    sliceLayout: {
      sliceHeight: 12,
      titleSizePx: 10,
    },
    // The following set of callbacks work around base slice_track's behaviour
    // of globally highlighting all slices with the same title. Highlighting
    // all "running" or "sleeping" slices across all visible thread tracks is
    // both visually noisy, and distracts from the actual slice being hovered.
    // Corner case: different tracks still see the published title for
    // highlighting, but the alternative is polluting SliceTrackAttrs for a
    // case that only this track currently cares about.
    onSliceOver: ({slice}) => {
      hoveredSliceId = slice.id;
    },
    onSliceOut: () => {
      hoveredSliceId = undefined;
    },
    onUpdatedSlices: (slices) =>
      slices.map((s) =>
        s.id === hoveredSliceId ? ColorVariant.VARIANT : ColorVariant.BASE,
      ),
    sliceName: (row) => row.state || '[Unknown]',
    colorizer: (row): ColorScheme => {
      const colorForState = colorForThreadState(row.state || '[Unknown]');
      if (row.state.includes('Sleeping') || row.state.includes('Idle')) {
        // For sleeping/idle slices, return a transparent color scheme with
        // transparent text + a subtle gray variant displayed when hovering the
        // slice.
        return COLOR_SCHEME_SLEEPING_IDLE;
      } else {
        return colorForState;
      }
    },
    detailsPanel: (row) => new ThreadStateDetailsPanel(trace, row.id),
    rootTableName: 'thread_state',
  });
}
