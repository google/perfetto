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

import {HSLColor} from '../../common/color';
import {ColorScheme, makeColorScheme} from '../../common/colorizer';
import {
  NAMED_ROW,
  NamedSliceTrack,
  NamedSliceTrackTypes,
} from '../../frontend/named_slice_track';
import {EngineProxy, Slice, STR_NULL} from '../../public';

const BLUE = makeColorScheme(new HSLColor('#03A9F4'));    // Blue 500
const GREEN = makeColorScheme(new HSLColor('#4CAF50'));   // Green 500
const YELLOW = makeColorScheme(new HSLColor('#FFEB3B'));  // Yellow 500
const RED = makeColorScheme(new HSLColor('#FF5722'));     // Red 500
const LIGHT_GREEN =
    makeColorScheme(new HSLColor('#C0D588'));           // Light Green 500
const PINK = makeColorScheme(new HSLColor('#F515E0'));  // Pink 500

export const ACTUAL_FRAME_ROW = {
  // Base columns (tsq, ts, dur, id, depth).
  ...NAMED_ROW,

  // Chrome-specific columns.
  jankTag: STR_NULL,
};
export type ActualFrameRow = typeof ACTUAL_FRAME_ROW;

export interface ActualFrameTrackTypes extends NamedSliceTrackTypes {
  row: ActualFrameRow;
}

export class ActualFramesTrack extends NamedSliceTrack<ActualFrameTrackTypes> {
  constructor(
      engine: EngineProxy, maxDepth: number, trackKey: string,
      private trackIds: number[]) {
    super({engine, trackKey});
    this.sliceLayout.maxDepth = maxDepth + 1;
  }

  // This is used by the base class to call iter().
  getRowSpec() {
    return ACTUAL_FRAME_ROW;
  }

  getSqlSource(): string {
    return `
      SELECT
        s.ts as ts,
        s.dur as dur,
        s.layout_depth as depth,
        s.name as name,
        s.id as id,
        afs.jank_tag as jankTag
      from experimental_slice_layout s
      join actual_frame_timeline_slice afs using(id)
      where
        filter_track_ids = '${this.trackIds.join(',')}'
    `;
  }

  rowToSlice(row: ActualFrameRow): Slice {
    const baseSlice = super.rowToSlice(row);
    return {...baseSlice, colorScheme: getColorSchemeForJank(row.jankTag)};
  }
}

function getColorSchemeForJank(jankTag: string|null): ColorScheme {
  switch (jankTag) {
    case 'Self Jank':
      return RED;
    case 'Other Jank':
      return YELLOW;
    case 'Dropped Frame':
      return BLUE;
    case 'Buffer Stuffing':
    case 'SurfaceFlinger Stuffing':
      return LIGHT_GREEN;
    case 'No Jank':
      return GREEN;
    default:
      return PINK;
  }
}
