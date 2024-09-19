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

import {globals} from '../../frontend/globals';
import {NamedRow} from '../../frontend/named_slice_track';
import {NewTrackArgs} from '../../frontend/track';
import {SCROLL_JANK_V3_TRACK_KIND} from '../../public/track_kinds';
import {Slice} from '../../public/track';
import {
  CustomSqlDetailsPanelConfig,
  CustomSqlTableDefConfig,
  CustomSqlTableSliceTrack,
} from '../../frontend/tracks/custom_sql_table_slice_track';
import {JANK_COLOR} from './jank_colors';
import {ScrollJankV3DetailsPanel} from './scroll_jank_v3_details_panel';
import {getColorForSlice} from '../../core/colorizer';
import {ScrollJankPluginState} from './common';

const UNKNOWN_SLICE_NAME = 'Unknown';
const JANK_SLICE_NAME = ' Jank';

export class ScrollJankV3Track extends CustomSqlTableSliceTrack {
  constructor(args: NewTrackArgs) {
    super(args);
    ScrollJankPluginState.getInstance().registerTrack({
      kind: SCROLL_JANK_V3_TRACK_KIND,
      trackUri: this.uri,
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

  async onDestroy(): Promise<void> {
    await super.onDestroy();
    ScrollJankPluginState.getInstance().unregisterTrack(
      SCROLL_JANK_V3_TRACK_KIND,
    );
  }

  rowToSlice(row: NamedRow): Slice {
    const slice = super.rowToSlice(row);

    let stage = slice.title.substring(0, slice.title.indexOf(JANK_SLICE_NAME));
    // Stage may include substage, in which case we use the substage for
    // color selection.
    const separator = '::';
    if (stage.indexOf(separator) != -1) {
      stage = stage.substring(stage.indexOf(separator) + separator.length);
    }

    if (stage == UNKNOWN_SLICE_NAME) {
      return {...slice, colorScheme: JANK_COLOR};
    } else {
      return {...slice, colorScheme: getColorForSlice(stage)};
    }
  }

  onUpdatedSlices(slices: Slice[]) {
    for (const slice of slices) {
      const currentSelection = globals.selectionManager.legacySelection;
      const isSelected =
        currentSelection &&
        currentSelection.kind === 'GENERIC_SLICE' &&
        currentSelection.id !== undefined &&
        currentSelection.id === slice.id;

      const highlighted = globals.state.highlightedSliceId === slice.id;
      const hasFocus = highlighted || isSelected;
      slice.isHighlighted = !!hasFocus;
    }
    super.onUpdatedSlices(slices);
  }
}
