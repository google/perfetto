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

import {NamedRow} from '../../components/tracks/named_slice_track';
import {Slice} from '../../public/track';
import {
  CustomSqlTableDefConfig,
  CustomSqlTableSliceTrack,
} from '../../components/tracks/custom_sql_table_slice_track';
import {JANK_COLOR} from './jank_colors';
import {TrackEventSelection} from '../../public/selection';
import {EventLatencySliceDetailsPanel} from './event_latency_details_panel';
import {Trace} from '../../public/trace';

export const JANKY_LATENCY_NAME = 'Janky EventLatency';

export class EventLatencyTrack extends CustomSqlTableSliceTrack {
  constructor(
    trace: Trace,
    uri: string,
    private baseTable: string,
  ) {
    super(trace, uri);
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

  override detailsPanel(sel: TrackEventSelection) {
    return new EventLatencySliceDetailsPanel(this.trace, sel.eventId);
  }
}
