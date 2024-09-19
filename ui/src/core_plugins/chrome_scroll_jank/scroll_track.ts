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

import {NewTrackArgs} from '../../frontend/track';
import {CHROME_TOPLEVEL_SCROLLS_KIND} from '../../public/track_kinds';
import {
  CustomSqlDetailsPanelConfig,
  CustomSqlTableDefConfig,
  CustomSqlTableSliceTrack,
} from '../../frontend/tracks/custom_sql_table_slice_track';
import {ScrollJankPluginState} from './common';
import {ScrollDetailsPanel} from './scroll_details_panel';

export class TopLevelScrollTrack extends CustomSqlTableSliceTrack {
  public static kind = CHROME_TOPLEVEL_SCROLLS_KIND;

  getSqlDataSource(): CustomSqlTableDefConfig {
    return {
      columns: [`printf("Scroll %s", CAST(id AS STRING)) AS name`, '*'],
      sqlTableName: 'chrome_scrolls',
    };
  }

  getDetailsPanel(): CustomSqlDetailsPanelConfig {
    return {
      kind: ScrollDetailsPanel.kind,
      config: {
        sqlTableName: this.tableName,
        title: 'Chrome Top Level Scrolls',
      },
    };
  }

  constructor(args: NewTrackArgs) {
    super(args);

    ScrollJankPluginState.getInstance().registerTrack({
      kind: TopLevelScrollTrack.kind,
      trackUri: this.uri,
      tableName: this.tableName,
      detailsPanelConfig: this.getDetailsPanel(),
    });
  }

  async onDestroy(): Promise<void> {
    await super.onDestroy();
    ScrollJankPluginState.getInstance().unregisterTrack(
      TopLevelScrollTrack.kind,
    );
  }
}
