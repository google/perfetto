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

// Panel for the top-level scrolls. For now, just show the scroll id, but we
// can add things like scroll event count, janks, etc. as needed.

import m from 'mithril';

import {ColumnType} from '../../common/query_result';
import {
  BottomTab,
  bottomTabRegistry,
  NewBottomTabArgs,
} from '../../frontend/bottom_tab';
import {globals} from '../../frontend/globals';
import {timestampFromSqlNanos} from '../../frontend/sql_types';
import {Duration} from '../../frontend/widgets/duration';
import {Timestamp} from '../../frontend/widgets/timestamp';
import {dictToTree} from '../../frontend/widgets/tree';

interface TopLevelScrollTabConfig {
  sqlTableName: string;
  id: number;
}

export class TopLevelScrollDetailsTab extends
    BottomTab<TopLevelScrollTabConfig> {
  static readonly kind = 'org.perfetto.TopLevelScrollDetailsTab';

  data: {[key: string]: ColumnType}|undefined;

  static create(args: NewBottomTabArgs): TopLevelScrollDetailsTab {
    return new TopLevelScrollDetailsTab(args);
  }

  constructor(args: NewBottomTabArgs) {
    super(args);

    this.engine
        .query(`select * from ${this.config.sqlTableName} where id = ${
            this.config.id}`)
        .then((queryResult) => {
          this.data = queryResult.firstRow({});
          globals.rafScheduler.scheduleFullRedraw();
        });
  }

  viewTab() {
    if (this.data === undefined) {
      return m('h2', 'Loading');
    }

    const left = dictToTree({
      'Scroll Id (gesture_scroll_id)': `${this.data['id']}`,
      'Start time': m(Timestamp, {ts: timestampFromSqlNanos(this.data['ts'])}),
      'Duration': m(Duration, {dur: Number(this.data['dur'])}),
    });
    return m(
        '.details-panel',
        m('header.overview', m('span', `${this.data['name']}`)),
        m('.details-table-multicolumn', m('.half-width-panel', left)));
  }

  getTitle(): string {
    return `Current Chrome Scroll`;
  }

  isLoading() {
    return this.data === undefined;
  }

  renderTabCanvas() {
    return;
  }
}

bottomTabRegistry.register(TopLevelScrollDetailsTab);
