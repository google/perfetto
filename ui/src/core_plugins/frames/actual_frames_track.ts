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

import {HSLColor} from '../../public/color';
import {makeColorScheme} from '../../core/colorizer';
import {ColorScheme} from '../../public/color_scheme';
import {NAMED_ROW, NamedSliceTrack} from '../../frontend/named_slice_track';
import {SLICE_LAYOUT_FIT_CONTENT_DEFAULTS} from '../../frontend/slice_layout';
import {STR_NULL} from '../../trace_processor/query_result';
import {Slice} from '../../public/track';
import {Trace} from '../../public/trace';

// color named and defined based on Material Design color palettes
// 500 colors indicate a timeline slice is not a partial jank (not a jank or
// full jank)
const BLUE_500 = makeColorScheme(new HSLColor('#03A9F4'));
const BLUE_200 = makeColorScheme(new HSLColor('#90CAF9'));
const GREEN_500 = makeColorScheme(new HSLColor('#4CAF50'));
const GREEN_200 = makeColorScheme(new HSLColor('#A5D6A7'));
const YELLOW_500 = makeColorScheme(new HSLColor('#FFEB3B'));
const YELLOW_100 = makeColorScheme(new HSLColor('#FFF9C4'));
const RED_500 = makeColorScheme(new HSLColor('#FF5722'));
const RED_200 = makeColorScheme(new HSLColor('#EF9A9A'));
const LIGHT_GREEN_500 = makeColorScheme(new HSLColor('#C0D588'));
const LIGHT_GREEN_100 = makeColorScheme(new HSLColor('#DCEDC8'));
const PINK_500 = makeColorScheme(new HSLColor('#F515E0'));
const PINK_200 = makeColorScheme(new HSLColor('#F48FB1'));

export const ACTUAL_FRAME_ROW = {
  // Base columns (tsq, ts, dur, id, depth).
  ...NAMED_ROW,

  // Jank-specific columns.
  jankTag: STR_NULL,
  jankSeverityType: STR_NULL,
};
export type ActualFrameRow = typeof ACTUAL_FRAME_ROW;

export class ActualFramesTrack extends NamedSliceTrack<Slice, ActualFrameRow> {
  constructor(
    trace: Trace,
    maxDepth: number,
    uri: string,
    private trackIds: number[],
  ) {
    super({trace, uri});
    this.sliceLayout = {
      ...SLICE_LAYOUT_FIT_CONTENT_DEFAULTS,
      depthGuess: maxDepth,
    };
  }

  // This is used by the base class to call iter().
  protected getRowSpec() {
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
        afs.jank_tag as jankTag,
        afs.jank_severity_type as jankSeverityType
      from experimental_slice_layout s
      join actual_frame_timeline_slice afs using(id)
      where
        filter_track_ids = '${this.trackIds.join(',')}'
    `;
  }

  rowToSlice(row: ActualFrameRow): Slice {
    const baseSlice = this.rowToSliceBase(row);
    return {
      ...baseSlice,
      colorScheme: getColorSchemeForJank(row.jankTag, row.jankSeverityType),
    };
  }
}

function getColorSchemeForJank(
  jankTag: string | null,
  jankSeverityType: string | null,
): ColorScheme {
  if (jankSeverityType === 'Partial') {
    switch (jankTag) {
      case 'Self Jank':
        return RED_200;
      case 'Other Jank':
        return YELLOW_100;
      case 'Dropped Frame':
        return BLUE_200;
      case 'Buffer Stuffing':
      case 'SurfaceFlinger Stuffing':
        return LIGHT_GREEN_100;
      case 'No Jank': // should not happen
        return GREEN_200;
      default:
        return PINK_200;
    }
  } else {
    switch (jankTag) {
      case 'Self Jank':
        return RED_500;
      case 'Other Jank':
        return YELLOW_500;
      case 'Dropped Frame':
        return BLUE_500;
      case 'Buffer Stuffing':
      case 'SurfaceFlinger Stuffing':
        return LIGHT_GREEN_500;
      case 'No Jank':
        return GREEN_500;
      default:
        return PINK_500;
    }
  }
}
