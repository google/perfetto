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

import {NamedSliceTrackTypes} from '../../frontend/named_slice_track';
import {Utid} from '../../frontend/sql_types';
import {
  CustomSqlDetailsPanelConfig,
  CustomSqlTableDefConfig,
  CustomSqlTableSliceTrack,
} from '../../frontend/tracks/custom_sql_table_slice_track';
import {Engine} from '../../public';

import {ChromeTasksDetailsTab} from './details';

export class ChromeTasksThreadTrack extends CustomSqlTableSliceTrack<NamedSliceTrackTypes> {
  constructor(engine: Engine, trackKey: string, private utid: Utid) {
    super({engine, trackKey});
  }

  getSqlDataSource(): CustomSqlTableDefConfig {
    return {
      columns: ['name', 'id', 'ts', 'dur'],
      sqlTableName: 'chrome_tasks',
      whereClause: `utid = ${this.utid}`,
    };
  }

  getDetailsPanel(): CustomSqlDetailsPanelConfig {
    return {
      kind: ChromeTasksDetailsTab.kind,
      config: {
        sqlTableName: 'chrome_tasks',
        title: 'Chrome Tasks',
      },
    };
  }
}
