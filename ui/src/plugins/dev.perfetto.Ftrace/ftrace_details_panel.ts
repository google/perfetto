// Copyright (C) 2025 The Android Open Source Project
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
import {Time} from '../../base/time';
import {renderArguments} from '../../components/details/args';
import {ArgsDict, getArgs} from '../../components/sql_utils/args';
import {asArgSetId} from '../../components/sql_utils/core_types';
import {Timestamp} from '../../components/widgets/timestamp';
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {Trace} from '../../public/trace';
import {NUM_NULL} from '../../trace_processor/query_result';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout, GridLayoutColumn} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';

export class FtraceEventDetailsPanel implements TrackEventDetailsPanel {
  private args?: ArgsDict;

  constructor(
    readonly trace: Trace,
    readonly row: Readonly<{
      id: number;
      ts: bigint;
      name: string;
      cpu: number;
    }>,
  ) {}

  async load() {
    await this.loadArgs();
  }

  render() {
    return m(
      DetailsShell,
      {
        title: `Ftrace Event`,
        description: this.row.name,
      },
      m(
        GridLayout,
        m(
          GridLayoutColumn,
          m(
            Section,
            {title: 'Details'},
            m(
              Tree,
              m(TreeNode, {
                left: 'ID',
                right: this.row.id,
              }),
              m(TreeNode, {
                left: 'Name',
                right: this.row.name,
              }),
              m(TreeNode, {
                left: 'Timestamp',
                right: m(Timestamp, {
                  trace: this.trace,
                  ts: Time.fromRaw(this.row.ts),
                }),
              }),
              m(TreeNode, {
                left: 'CPU',
                right: this.row.cpu,
              }),
            ),
          ),
        ),
        m(
          GridLayoutColumn,
          m(
            Section,
            {title: 'Arguments'},
            m(Tree, this.args && renderArguments(this.trace, this.args)),
          ),
        ),
      ),
    );
  }

  private async loadArgs() {
    const queryRes = await this.trace.engine.query(`
      SELECT arg_set_id
      FROM ftrace_event
      WHERE ftrace_event.id = ${this.row.id}
    `);

    const it = queryRes.iter({
      arg_set_id: NUM_NULL,
    });

    if (it.valid() && it.arg_set_id !== null) {
      this.args = await getArgs(this.trace.engine, asArgSetId(it.arg_set_id));
    }
  }
}
