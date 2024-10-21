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
import {TrackEventDetailsPanel} from '../public/details_panel';
import {ColumnType} from '../trace_processor/query_result';
import {sqlValueToReadableString} from '../trace_processor/sql_utils';
import {DetailsShell} from '../widgets/details_shell';
import {GridLayout} from '../widgets/grid_layout';
import {Section} from '../widgets/section';
import {SqlRef} from '../widgets/sql_ref';
import {dictToTree, Tree, TreeNode} from '../widgets/tree';
import {Trace} from '../public/trace';

export interface ColumnConfig {
  readonly displayName?: string;
}

export type Columns = {
  readonly [columnName: string]: ColumnConfig;
};

// A details tab, which fetches slice-like object from a given SQL table by id
// and renders it according to the provided config, specifying which columns
// need to be rendered and how.
export class GenericSliceDetailsTab implements TrackEventDetailsPanel {
  private data?: {[key: string]: ColumnType};

  constructor(
    private readonly trace: Trace,
    private readonly sqlTableName: string,
    private readonly id: number,
    private readonly title: string,
    private readonly columns?: Columns,
  ) {}

  async load() {
    const result = await this.trace.engine.query(
      `select * from ${this.sqlTableName} where id = ${this.id}`,
    );

    this.data = result.firstRow({});
  }

  render() {
    if (!this.data) {
      return m('h2', 'Loading');
    }

    const args: {[key: string]: m.Child} = {};
    if (this.columns !== undefined) {
      for (const key of Object.keys(this.columns)) {
        let argKey = key;
        if (this.columns[key].displayName !== undefined) {
          argKey = this.columns[key].displayName!;
        }
        args[argKey] = sqlValueToReadableString(this.data[key]);
      }
    } else {
      for (const key of Object.keys(this.data)) {
        args[key] = sqlValueToReadableString(this.data[key]);
      }
    }

    const details = dictToTree(args);

    return m(
      DetailsShell,
      {
        title: this.title,
      },
      m(
        GridLayout,
        m(Section, {title: 'Details'}, m(Tree, details)),
        m(
          Section,
          {title: 'Metadata'},
          m(Tree, [
            m(TreeNode, {
              left: 'SQL ID',
              right: m(SqlRef, {
                table: this.sqlTableName,
                id: this.id,
              }),
            }),
          ]),
        ),
      ),
    );
  }
}
