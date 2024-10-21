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

import {Utid} from '../../trace_processor/sql_utils/core_types';
import {
  CustomSqlTableDefConfig,
  CustomSqlTableSliceTrack,
} from '../../frontend/tracks/custom_sql_table_slice_track';
import {Trace} from '../../public/trace';
import {TrackEventSelection} from '../../public/selection';
import {ChromeTasksDetailsPanel} from './details';

export class ChromeTasksThreadTrack extends CustomSqlTableSliceTrack {
  constructor(
    trace: Trace,
    uri: string,
    private utid: Utid,
  ) {
    super({trace, uri});
  }

  getSqlDataSource(): CustomSqlTableDefConfig {
    return {
      columns: ['name', 'id', 'ts', 'dur'],
      sqlTableName: 'chrome_tasks',
      whereClause: `utid = ${this.utid}`,
    };
  }

  override detailsPanel(sel: TrackEventSelection) {
    return new ChromeTasksDetailsPanel(this.trace, sel.eventId);
  }
}
