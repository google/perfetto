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

import {Trace} from '../../public/trace';
import {LONG, NUM, STR} from '../../trace_processor/query_result';
import {ColorScheme} from '../../base/color_scheme';
import {JANK_COLOR} from './jank_colors';
import {getColorForSlice, makeColorScheme} from '../../components/colorizer';
import {HSLColor} from '../../base/color';
import {ScrollTimelineDetailsPanel} from './scroll_timeline_details_panel';
import {
  ScrollTimelineModel,
  ScrollUpdateClassification,
} from './scroll_timeline_model';
import {SliceTrack} from '../../components/tracks/slice_track';
import {SourceDataset} from '../../trace_processor/dataset';

const INDIGO = makeColorScheme(new HSLColor([231, 48, 48]));
const GRAY = makeColorScheme(new HSLColor([0, 0, 62]));
const DARK_GREEN = makeColorScheme(new HSLColor([120, 44, 34]));
const TEAL = makeColorScheme(new HSLColor([187, 90, 42]));

function toColorScheme(
  classification: ScrollUpdateClassification,
): ColorScheme | undefined {
  switch (classification) {
    case ScrollUpdateClassification.DEFAULT:
      return INDIGO;
    case ScrollUpdateClassification.JANKY:
      return JANK_COLOR;
    case ScrollUpdateClassification.COALESCED:
      return GRAY;
    case ScrollUpdateClassification.FIRST_SCROLL_UPDATE_IN_FRAME:
      return DARK_GREEN;
    case ScrollUpdateClassification.INERTIAL:
      return TEAL;
    case ScrollUpdateClassification.STEP:
      return undefined;
  }
}

export function createScrollTimelineTrack(
  trace: Trace,
  model: ScrollTimelineModel,
) {
  return SliceTrack.create({
    trace,
    uri: model.trackUri,
    dataset: new SourceDataset({
      src: model.tableName,
      schema: {
        id: NUM,
        ts: LONG,
        dur: LONG,
        name: STR,
        classification: NUM,
        depth: NUM,
      },
    }),
    colorizer: (row) => {
      return toColorScheme(row.classification) ?? getColorForSlice(row.name);
    },
    detailsPanel: (row) => {
      return new ScrollTimelineDetailsPanel(trace, model, row.id);
    },
  });
}
