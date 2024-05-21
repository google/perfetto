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
import {NamedRow, NamedSliceTrackTypes} from '../../frontend/named_slice_track';
import {NewTrackArgs} from '../../frontend/track';
import {PrimaryTrackSortKey, Slice} from '../../public';
import {
  CustomSqlDetailsPanelConfig,
  CustomSqlTableDefConfig,
  CustomSqlTableSliceTrack,
} from '../../frontend/tracks/custom_sql_table_slice_track';

import {EventLatencySliceDetailsPanel} from './event_latency_details_panel';
import {JANK_COLOR} from './jank_colors';
import {getLegacySelection} from '../../common/state';
import {
  CHROME_EVENT_LATENCY_TRACK_KIND,
  DecideTracksResult,
  SCROLL_JANK_GROUP_ID,
  ScrollJankPluginState,
} from './common';

export const JANKY_LATENCY_NAME = 'Janky EventLatency';

export interface EventLatencyTrackTypes extends NamedSliceTrackTypes {
  config: {baseTable: string};
}

export class EventLatencyTrack extends CustomSqlTableSliceTrack<EventLatencyTrackTypes> {
  constructor(args: NewTrackArgs, private baseTable: string) {
    super(args);
    ScrollJankPluginState.getInstance().registerTrack({
      kind: CHROME_EVENT_LATENCY_TRACK_KIND,
      trackKey: this.trackKey,
      tableName: this.tableName,
      detailsPanelConfig: this.getDetailsPanel(),
    });
  }

  async onDestroy(): Promise<void> {
    await super.onDestroy();
    ScrollJankPluginState.getInstance().unregisterTrack(
      CHROME_EVENT_LATENCY_TRACK_KIND,
    );
  }

  getSqlSource(): string {
    return `SELECT * FROM ${this.baseTable}`;
  }

  getDetailsPanel(): CustomSqlDetailsPanelConfig {
    return {
      kind: EventLatencySliceDetailsPanel.kind,
      config: {title: '', sqlTableName: this.tableName},
    };
  }

  getSqlDataSource(): CustomSqlTableDefConfig {
    return {
      sqlTableName: this.baseTable,
    };
  }

  rowToSlice(row: NamedRow): Slice {
    const baseSlice = super.rowToSlice(row);
    if (baseSlice.title === JANKY_LATENCY_NAME) {
      return {...baseSlice, colorScheme: JANK_COLOR};
    } else {
      return baseSlice;
    }
  }

  onUpdatedSlices(slices: EventLatencyTrackTypes['slice'][]) {
    for (const slice of slices) {
      const currentSelection = getLegacySelection(globals.state);
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

  // At the moment we will just display the slice details. However, on select,
  // this behavior should be customized to show jank-related data.
}

export async function addLatencyTracks(): Promise<DecideTracksResult> {
  const result: DecideTracksResult = {
    tracksToAdd: [],
  };

  result.tracksToAdd.push({
    uri: 'perfetto.ChromeScrollJank#eventLatency',
    trackSortKey: PrimaryTrackSortKey.ASYNC_SLICE_TRACK,
    name: 'Chrome Scroll Input Latencies',
    trackGroup: SCROLL_JANK_GROUP_ID,
  });

  return result;
}
