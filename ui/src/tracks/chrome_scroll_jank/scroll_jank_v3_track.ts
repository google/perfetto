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

import {
  getColorForSlice,
} from '../../common/colorizer';
import {globals} from '../../frontend/globals';
import {NamedSliceTrackTypes} from '../../frontend/named_slice_track';
import {NewTrackArgs, TrackBase} from '../../frontend/track';
import {PrimaryTrackSortKey} from '../../public';
import {
  CustomSqlDetailsPanelConfig,
  CustomSqlTableDefConfig,
  CustomSqlTableSliceTrack,
} from '../custom_sql_table_slices';

import {EventLatencyTrackTypes} from './event_latency_track';
import {
  SCROLL_JANK_GROUP_ID,
  ScrollJankPluginState,
  ScrollJankTracks as DecideTracksResult,
} from './index';
import {DEEP_RED_COLOR, RED_COLOR} from './jank_colors';
import {ScrollJankV3DetailsPanel} from './scroll_jank_v3_details_panel';

const UNKNOWN_SLICE_NAME = 'Unknown';
const JANK_SLICE_NAME = ' Jank';

export class ScrollJankV3Track extends
    CustomSqlTableSliceTrack<NamedSliceTrackTypes> {
  static readonly kind = 'org.chromium.ScrollJank.scroll_jank_v3_track';

  static create(args: NewTrackArgs): TrackBase {
    return new ScrollJankV3Track(args);
  }

  constructor(args: NewTrackArgs) {
    super(args);
    ScrollJankPluginState.getInstance().registerTrack({
      kind: ScrollJankV3Track.kind,
      trackKey: this.trackKey,
      tableName: this.tableName,
      detailsPanelConfig: this.getDetailsPanel(),
    });
  }

  getSqlDataSource(): CustomSqlTableDefConfig {
    return {
      columns: [
        `IIF(
          cause_of_jank IS NOT NULL,
          cause_of_jank || IIF(
            sub_cause_of_jank IS NOT NULL, "::" || sub_cause_of_jank, ""
            ), "${UNKNOWN_SLICE_NAME}") || "${JANK_SLICE_NAME}" AS name`,
        'id',
        'ts',
        'dur',
        'event_latency_id',
      ],
      sqlTableName: 'chrome_janky_frame_presentation_intervals',
    };
  }

  getDetailsPanel(): CustomSqlDetailsPanelConfig {
    return {
      kind: ScrollJankV3DetailsPanel.kind,
      config: {
        sqlTableName: 'chrome_janky_frame_presentation_intervals',
        title: 'Chrome Scroll Janks',
      },
    };
  }

  onDestroy() {
    super.onDestroy();
    ScrollJankPluginState.getInstance().unregisterTrack(ScrollJankV3Track.kind);
  }

  onUpdatedSlices(slices: EventLatencyTrackTypes['slice'][]) {
    for (const slice of slices) {
      const currentSelection = globals.state.currentSelection;
      const isSelected = currentSelection &&
          currentSelection.kind === 'GENERIC_SLICE' &&
          currentSelection.id !== undefined && currentSelection.id === slice.id;

      const highlighted = globals.state.highlightedSliceId === slice.id;
      const hasFocus = highlighted || isSelected;

      let stage =
          slice.title.substring(0, slice.title.indexOf(JANK_SLICE_NAME));
      // Stage may include substage, in which case we use the substage for
      // color selection.
      const separator = '::';
      if (stage.indexOf(separator) != -1) {
        stage = stage.substring(stage.indexOf(separator) + separator.length);
      }

      if (stage == UNKNOWN_SLICE_NAME) {
        if (hasFocus) {
          slice.baseColor = DEEP_RED_COLOR;
        } else {
          slice.baseColor = RED_COLOR;
        }
      } else {
        slice.baseColor = getColorForSlice(stage, hasFocus);
      }
    }
    super.onUpdatedSlices(slices);
  }
}

export async function addScrollJankV3ScrollTrack():
    Promise<DecideTracksResult> {
  const result: DecideTracksResult = {
    tracksToAdd: [],
  };

  result.tracksToAdd.push({
    uri: 'perfetto.ChromeScrollJank#scrollJankV3',
    trackSortKey: PrimaryTrackSortKey.ASYNC_SLICE_TRACK,
    name: 'Chrome Scroll Janks',
    trackGroup: SCROLL_JANK_GROUP_ID,
  });

  return result;
}
