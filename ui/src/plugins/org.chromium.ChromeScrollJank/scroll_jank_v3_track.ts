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

import {SliceTrack} from '../../components/tracks/slice_track';
import {JANK_COLOR} from './jank_colors';
import {getColorForSlice} from '../../components/colorizer';
import {ScrollJankV3DetailsPanel} from './scroll_jank_v3_details_panel';
import {SourceDataset} from '../../trace_processor/dataset';
import {LONG, NUM, STR} from '../../trace_processor/query_result';
import {Trace} from '../../public/trace';

const UNKNOWN_SLICE_NAME = 'Unknown';
const JANK_SLICE_NAME = ' Jank';

export function createScrollJankV3Track(trace: Trace, uri: string) {
  return SliceTrack.create({
    trace,
    uri,
    dataset: new SourceDataset({
      schema: {
        id: NUM,
        ts: LONG,
        dur: LONG,
        name: STR,
      },
      src: `
        SELECT
          IIF(
            cause_of_jank IS NOT NULL,
            cause_of_jank || IIF(
              sub_cause_of_jank IS NOT NULL, "::" || sub_cause_of_jank, ""
              ), "${UNKNOWN_SLICE_NAME}") || "${JANK_SLICE_NAME}" AS name,
          id,
          ts,
          dur,
          event_latency_id
        FROM chrome_janky_frame_presentation_intervals
      `,
    }),
    colorizer: (row) => {
      let stage = row.name.substring(0, row.name.indexOf(JANK_SLICE_NAME));
      // Stage may include substage, in which case we use the substage for
      // color selection.
      const separator = '::';
      if (stage.indexOf(separator) != -1) {
        stage = stage.substring(stage.indexOf(separator) + separator.length);
      }

      if (stage == UNKNOWN_SLICE_NAME) {
        return JANK_COLOR;
      } else {
        return getColorForSlice(stage);
      }
    },
    detailsPanel: (row) => new ScrollJankV3DetailsPanel(trace, row.id),
  });
}
