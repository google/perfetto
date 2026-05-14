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

import {Trace} from '../../public/trace';
import {LONG, NUM, STR} from '../../trace_processor/query_result';
import {SliceTrack} from '../../components/tracks/slice_track';
import {SourceDataset} from '../../trace_processor/dataset';
import {getColorForSlice, makeColorScheme} from '../../components/colorizer';
import {FrameTimelineSliceClassification} from './frame_timeline_model';
import {ColorScheme} from '../../base/color_scheme';
import {HSLColor} from '../../base/color';

const COLOR_LIGHTER_SLATE = makeColorScheme(new HSLColor([215, 16, 47]));
const COLOR_VIBRANT_MINT = makeColorScheme(new HSLColor([160, 84, 39]));
const COLOR_SOFT_LAVENDER = makeColorScheme(new HSLColor([254, 49, 77]));
const COLOR_ELECTRIC_SKY_BLUE = makeColorScheme(new HSLColor([217, 91, 60]));

function toColorScheme(
  classification: FrameTimelineSliceClassification,
): ColorScheme | undefined {
  switch (classification) {
    case FrameTimelineSliceClassification.HEADER:
      return COLOR_LIGHTER_SLATE;
    case FrameTimelineSliceClassification.FRAME_TIME:
      return COLOR_ELECTRIC_SKY_BLUE;
    case FrameTimelineSliceClassification.EXTEND_VSYNC:
      // Don't override the color because we want it to match the default color
      // of 'Extend_VSync' slices on the other track.
      return undefined;
    case FrameTimelineSliceClassification.PREFERRED_TIMELINE:
      return COLOR_VIBRANT_MINT;
    case FrameTimelineSliceClassification.NON_PREFERRED_TIMELINE:
      return COLOR_SOFT_LAVENDER;
  }
}

export function createFrameTimelineTrack(
  trace: Trace,
  uri: string,
  tableName: string,
) {
  return SliceTrack.create({
    trace,
    uri: uri,
    rootTableName: tableName,
    dataset: new SourceDataset({
      src: tableName,
      schema: {
        id: NUM,
        ts: LONG,
        dur: LONG,
        name: STR,
        type: NUM,
        depth: NUM,
      },
    }),
    colorizer: (row) => {
      return toColorScheme(row.type) ?? getColorForSlice(row.name);
    },
  });
}
