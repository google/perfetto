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
import {makeColorScheme} from '../../components/colorizer';
import {ColorScheme} from '../../base/color_scheme';
import {LONG, NUM, STR, STR_NULL} from '../../trace_processor/query_result';
import {Trace} from '../../public/trace';
import {SourceDataset} from '../../trace_processor/dataset';
import {SliceTrack} from '../../components/tracks/slice_track';
import {ThreadSliceDetailsPanel} from '../../components/details/thread_slice_details_tab';

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
const WHITE_200 = makeColorScheme(new HSLColor('#F5F5F5'));

export function createActualFramesTrack(
  trace: Trace,
  uri: string,
  maxDepth: number,
  trackIds: ReadonlyArray<number>,
  useExperimentalJankForClassification: boolean,
) {
  return SliceTrack.create({
    trace,
    uri,
    dataset: new SourceDataset({
      src: 'actual_frame_timeline_slice',
      schema: {
        id: NUM,
        name: STR,
        ts: LONG,
        dur: LONG,
        jank_type: STR,
        jank_tag: STR_NULL,
        jank_tag_experimental: STR_NULL,
        jank_severity_type: STR_NULL,
        arg_set_id: NUM,
        track_id: NUM,
      },
      filter: {
        col: 'track_id',
        in: trackIds,
      },
    }),
    colorizer: (row) => {
      return getColorSchemeForJank(
        useExperimentalJankForClassification
          ? row.jank_tag_experimental
          : row.jank_tag,
        row.jank_severity_type,
      );
    },
    initialMaxDepth: maxDepth,
    rootTableName: 'slice',
    detailsPanel: () => new ThreadSliceDetailsPanel(trace),
  });
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
      case 'Non Animating': // should not happen
      case 'Display not ON':
        return WHITE_200;
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
      case 'Non Animating':
      case 'Display not ON':
        return WHITE_200;
      default:
        return PINK_500;
    }
  }
}
