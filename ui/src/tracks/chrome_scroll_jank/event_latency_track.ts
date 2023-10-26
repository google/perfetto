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
import {
  NamedSliceTrackTypes,
} from '../../frontend/named_slice_track';
import {NewTrackArgs, TrackBase} from '../../frontend/track';
import {PrimaryTrackSortKey} from '../../public';
import {
  CustomSqlDetailsPanelConfig,
  CustomSqlTableDefConfig,
  CustomSqlTableSliceTrack,
} from '../custom_sql_table_slices';

import {EventLatencySliceDetailsPanel} from './event_latency_details_panel';
import {
  SCROLL_JANK_GROUP_ID,
  ScrollJankPluginState,
  ScrollJankTracks as DecideTracksResult,
} from './index';
import {DEEP_RED_COLOR, RED_COLOR} from './jank_colors';

export const JANKY_LATENCY_NAME = 'Janky EventLatency';

export interface EventLatencyTrackTypes extends NamedSliceTrackTypes {
  config: {baseTable: string;}
}

const CHROME_EVENT_LATENCY_TRACK_KIND =
    'org.chromium.ScrollJank.event_latencies';

export class EventLatencyTrack extends
    CustomSqlTableSliceTrack<EventLatencyTrackTypes> {
  static readonly kind = CHROME_EVENT_LATENCY_TRACK_KIND;

  static create(args: NewTrackArgs): TrackBase {
    return new EventLatencyTrack(args);
  }

  constructor(args: NewTrackArgs) {
    super(args);
    ScrollJankPluginState.getInstance().registerTrack({
      kind: EventLatencyTrack.kind,
      trackKey: this.trackKey,
      tableName: this.tableName,
      detailsPanelConfig: this.getDetailsPanel(),
    });
  }

  onDestroy() {
    super.onDestroy();
    ScrollJankPluginState.getInstance().unregisterTrack(EventLatencyTrack.kind);
  }

  getSqlSource(): string {
    return `SELECT * FROM ${this.config.baseTable}`;
  }

  getDetailsPanel(): CustomSqlDetailsPanelConfig {
    return {
      kind: EventLatencySliceDetailsPanel.kind,
      config: {title: '', sqlTableName: this.tableName},
    };
  }

  getSqlDataSource(): CustomSqlTableDefConfig {
    return {
      sqlTableName: this.config.baseTable,
    };
  }

  onUpdatedSlices(slices: EventLatencyTrackTypes['slice'][]) {
    for (const slice of slices) {
      const currentSelection = globals.state.currentSelection;
      const isSelected = currentSelection &&
          currentSelection.kind === 'GENERIC_SLICE' &&
          currentSelection.id !== undefined && currentSelection.id === slice.id;

      const highlighted = globals.state.highlightedSliceId === slice.id;
      const hasFocus = highlighted || isSelected;

      if (slice.title === JANKY_LATENCY_NAME) {
        if (hasFocus) {
          slice.baseColor = DEEP_RED_COLOR;
        } else {
          slice.baseColor = RED_COLOR;
        }
      } else {
        slice.baseColor = getColorForSlice(slice.title, hasFocus);
      }
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
