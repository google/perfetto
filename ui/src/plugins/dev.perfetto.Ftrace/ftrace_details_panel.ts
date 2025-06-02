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
import {assertUnreachable} from '../../base/logging';
import {Time} from '../../base/time';
import {renderArguments} from '../../components/details/args';
import {Arg, ArgValue, ArgValueType} from '../../components/sql_utils/args';
import {asArgId} from '../../components/sql_utils/core_types';
import {Timestamp} from '../../components/widgets/timestamp';
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {Trace} from '../../public/trace';
import {
  LONG_NULL,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout, GridLayoutColumn} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';

export class FtraceEventDetailsPanel implements TrackEventDetailsPanel {
  private args?: ReadonlyArray<Arg>;

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
                right: m(Timestamp, {ts: Time.fromRaw(this.row.ts)}),
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
      SELECT
        args.id as id,
        flat_key as flatKey,
        key,
        int_value as intValue,
        string_value as stringValue,
        real_value as realValue,
        value_type as valueType,
        display_value as displayValue
      FROM ftrace_event
      JOIN args USING(arg_set_id)
      WHERE ftrace_event.id = ${this.row.id}
    `);

    const it = queryRes.iter({
      id: NUM,
      flatKey: STR,
      key: STR,
      intValue: LONG_NULL,
      stringValue: STR_NULL,
      realValue: NUM_NULL,
      valueType: STR,
      displayValue: STR_NULL,
    });

    const args: Arg[] = [];
    for (; it.valid(); it.next()) {
      const value = parseArgValue(it);
      args.push({
        id: asArgId(it.id),
        flatKey: it.flatKey,
        key: it.key,
        value,
        displayValue: it.displayValue ?? 'NULL',
      });
    }
    this.args = args;
  }
}

function parseArgValue(it: {
  valueType: string;
  intValue: bigint | null;
  stringValue: string | null;
  realValue: number | null;
}): ArgValue {
  const valueType = it.valueType as ArgValueType;
  switch (valueType) {
    case 'int':
    case 'uint':
      return it.intValue;
    case 'pointer':
      return it.intValue === null ? null : `0x${it.intValue.toString(16)}`;
    case 'string':
      return it.stringValue;
    case 'bool':
      return Boolean(it.intValue);
    case 'real':
      return it.realValue;
    case 'null':
      return null;
    default:
      assertUnreachable(valueType);
  }
}
