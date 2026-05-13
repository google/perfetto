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
import {ColorScheme} from '../../base/color_scheme';
import {JANK_COLOR} from './jank_colors';
import {getColorForSlice, makeColorScheme} from '../../components/colorizer';
import {HSLColor} from '../../base/color';
import {SliceTrack} from '../../components/tracks/slice_track';
import {SourceDataset} from '../../trace_processor/dataset';
import {ScrollFrameClassification} from './scroll_timeline_v4_model';
import {ScrollTimelineV4DetailsPanel} from './scroll_timeline_v4_details_panel';
import {SCROLL_TIMELINE_V4_TRACK} from './tracks';

const INDIGO = makeColorScheme(new HSLColor([231, 48, 48]));
const GOLD = makeColorScheme(new HSLColor([48, 95, 55]));
const TANGERINE = makeColorScheme(new HSLColor([32, 100, 50]));
const DARK_GREEN = makeColorScheme(new HSLColor([120, 44, 34]));
const TEAL = makeColorScheme(new HSLColor([187, 90, 42]));

function toColorScheme(
  classification: ScrollFrameClassification,
): ColorScheme | undefined {
  switch (classification) {
    case ScrollFrameClassification.DEFAULT:
      return INDIGO;
    case ScrollFrameClassification.JANKY:
      return JANK_COLOR;
    case ScrollFrameClassification.NON_DAMAGING:
      return GOLD;
    case ScrollFrameClassification.SYNTHETIC:
      return TANGERINE;
    case ScrollFrameClassification.FIRST_FRAME_IN_SCROLL:
      return DARK_GREEN;
    case ScrollFrameClassification.INERTIAL:
      return TEAL;
    case ScrollFrameClassification.DESCENDANT_SLICE:
      return undefined;
  }
}

export function createScrollTimelineV4Track(trace: Trace) {
  return SliceTrack.create({
    trace,
    uri: SCROLL_TIMELINE_V4_TRACK.uri,
    rootTableName: SCROLL_TIMELINE_V4_TRACK.tableName,
    dataset: new SourceDataset({
      src: SCROLL_TIMELINE_V4_TRACK.tableName,
      schema: {
        id: NUM,
        ts: LONG,
        dur: LONG,
        depth: NUM,
        name: STR,
        classification: NUM,
      },
    }),
    colorizer: (row) => {
      return toColorScheme(row.classification) ?? getColorForSlice(row.name);
    },
    detailsPanel: (row) => new ScrollTimelineV4DetailsPanel(trace, row.id),
  });
}
