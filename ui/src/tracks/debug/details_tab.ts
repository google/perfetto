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

import * as m from 'mithril';

import {ColumnType} from '../../common/query_result';
import {
  BottomTab,
  bottomTabRegistry,
  NewBottomTabArgs,
} from '../../frontend/bottom_tab';
import {globals} from '../../frontend/globals';
import {TPTimestamp} from '../../frontend/sql_types';
import {Duration} from '../../frontend/widgets/duration';
import {Timestamp} from '../../frontend/widgets/timestamp';
import {Tree, TreeNode} from '../../frontend/widgets/tree';
import {ARG_PREFIX} from './add_debug_track_menu';

interface DebugSliceDetalsTabConfig {
  sqlTableName: string;
  id: number;
}

function SqlValueToString(val: ColumnType) {
  if (val instanceof Uint8Array) {
    return `<blob length=${val.length}>`;
  }
  if (val === null) {
    return 'NULL';
  }
  return val.toString();
}

function dictToTree(dict: {[key: string]: m.Child}): m.Children {
  const children: m.Child[] = [];
  for (const key of Object.keys(dict)) {
    children.push(m(TreeNode, {
      left: key,
      right: dict[key],
    }));
  }
  return m(Tree, children);
}

export class DebugSliceDetailsTab extends BottomTab<DebugSliceDetalsTabConfig> {
  static readonly kind = 'org.perfetto.DebugSliceDetailsTab';

  data: {[key: string]: ColumnType}|undefined;

  static create(args: NewBottomTabArgs): DebugSliceDetailsTab {
    return new DebugSliceDetailsTab(args);
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
      'Name': this.data['name'] as string,
      'Start time': m(Timestamp, {ts: this.data['ts'] as TPTimestamp}),
      'Duration': m(Duration, {dur: this.data['dur'] as number}),
      'Debug slice id': `${this.config.sqlTableName}[${this.config.id}]`,
    });
    const args: {[key: string]: m.Child} = {};
    for (const key of Object.keys(this.data)) {
      if (key.startsWith(ARG_PREFIX)) {
        args[key.substr(ARG_PREFIX.length)] = SqlValueToString(this.data[key]);
      }
    }
    return m(
        'div.details-panel',
        m('header.overview', m('span', 'Debug Slice')),
        m('.details-table-multicolumn',
          {
            style: {
              'user-select': 'text',
            },
          },
          m('.half-width-panel', left),
          m('.half-width-panel', dictToTree(args))));
  }

  getTitle(): string {
    return `Current Selection`;
  }

  isLoading() {
    return this.data === undefined;
  }

  renderTabCanvas() {
    return;
  }
}

bottomTabRegistry.register(DebugSliceDetailsTab);
