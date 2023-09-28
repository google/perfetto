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

import m from 'mithril';

import {ColumnType} from '../common/query_result';
import {raf} from '../core/raf_scheduler';
import {DetailsShell} from '../widgets/details_shell';
import {GridLayout} from '../widgets/grid_layout';
import {Section} from '../widgets/section';
import {SqlRef} from '../widgets/sql_ref';
import {dictToTree, Tree, TreeNode} from '../widgets/tree';

import {BottomTab, bottomTabRegistry, NewBottomTabArgs} from './bottom_tab';
import {sqlValueToString} from './sql_utils';

export interface ColumnConfig {
  displayName?: string;
}

export type Columns = {
  [columnName: string]: ColumnConfig
}

export interface GenericSliceDetailsTabConfigBase {
  sqlTableName: string;
  title: string;
  // All columns are rendered if |columns| is undefined.
  columns?: Columns;
}

export type GenericSliceDetailsTabConfig = GenericSliceDetailsTabConfigBase&{
  id: number;
}

// A details tab, which fetches slice-like object from a given SQL table by id
// and renders it according to the provided config, specifying which columns
// need to be rendered and how.
export class GenericSliceDetailsTab extends
    BottomTab<GenericSliceDetailsTabConfig> {
  static readonly kind = 'dev.perfetto.GenericSliceDetailsTab';

  data: {[key: string]: ColumnType}|undefined;

  static create(args: NewBottomTabArgs): GenericSliceDetailsTab {
    return new GenericSliceDetailsTab(args);
  }

  constructor(args: NewBottomTabArgs) {
    super(args);

    this.engine
        .query(`select * from ${this.config.sqlTableName} where id = ${
            this.config.id}`)
        .then((queryResult) => {
          this.data = queryResult.firstRow({});
          raf.scheduleFullRedraw();
        });
  }

  viewTab() {
    if (this.data === undefined) {
      return m('h2', 'Loading');
    }

    const args: {[key: string]: m.Child} = {};
    if (this.config.columns !== undefined) {
      for (const key of Object.keys(this.config.columns)) {
        let argKey = key;
        if (this.config.columns[key].displayName !== undefined) {
          argKey = this.config.columns[key].displayName!;
        }
        args[argKey] = sqlValueToString(this.data[key]);
      }
    } else {
      for (const key of Object.keys(this.data)) {
        args[key] = sqlValueToString(this.data[key]);
      }
    }

    const details = dictToTree(args);

    return m(
        DetailsShell,
        {
          title: this.config.title,
        },
        m(
            GridLayout,
            m(
                Section,
                {title: 'Details'},
                m(Tree, details),
            ),
            m(
                Section,
                {title: 'Metadata'},
                m(Tree, [m(TreeNode, {
                  left: 'SQL ID',
                  right: m(SqlRef, {
                    table: this.config.sqlTableName,
                    id: this.config.id}),
                })]),
            ),
        ),
    );
  }

  getTitle(): string {
    return this.config.title;
  }

  isLoading() {
    return this.data === undefined;
  }
}

bottomTabRegistry.register(GenericSliceDetailsTab);
