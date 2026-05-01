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

import {Time, duration, time} from '../../base/time';
import {Engine} from '../../trace_processor/engine';
import {Trace} from '../../public/trace';
import {
  LONG,
  LONG_NULL,
  NUM,
  NUM_NULL,
} from '../../trace_processor/query_result';
import {TrackEventDetailsPanel} from '../../public/details_panel';
import m from 'mithril';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';
import {Timestamp} from '../../components/widgets/timestamp';
import {DurationWidget} from '../../components/widgets/duration';
import {TrackEventSelection} from '../../public/selection';
import {hasArgs, renderArguments} from '../../components/details/args';
import {asArgSetId} from '../../components/sql_utils/core_types';
import {ArgsDict, getArgs} from '../../components/sql_utils/args';
import {
  YMode,
  counterDisplayUnit,
  counterValueExpression,
} from '../../components/tracks/counter_track';
import {assertUnreachable} from '../../base/assert';

interface CounterDetails {
  // The "left" timestamp of the counter sample T(N)
  ts: time;

  // The delta between this sample and the next one's timestamps T(N+1) - T(N)
  duration: duration;

  // The raw value F(N)
  value: number;

  // The delta: F(N+1) - F(N)
  delta: number;

  // The rate: (F(N+1) - F(N)) / dt
  rate: number;

  args?: ArgsDict;
}

export class CounterDetailsPanel implements TrackEventDetailsPanel {
  private readonly trace: Trace;
  private readonly engine: Engine;
  private readonly sqlSource: string;
  private readonly trackName: string;
  private readonly getMode: () => YMode;
  private readonly unit: string;
  private readonly rateUnit: string;
  private counterDetails?: CounterDetails;

  constructor(
    trace: Trace,
    trackName: string,
    getMode: () => YMode,
    unit: string,
    rateUnit: string,
    sqlSource: string,
  ) {
    this.trace = trace;
    this.engine = trace.engine;
    this.trackName = trackName;
    this.getMode = getMode;
    this.unit = unit;
    this.rateUnit = rateUnit;
    this.sqlSource = sqlSource;
  }

  async load({eventId}: TrackEventSelection) {
    this.counterDetails = await loadCounterDetails(
      this.engine,
      eventId,
      this.sqlSource,
    );
  }

  private formatWithUnit(value: number, mode: YMode): string {
    const unitLabel = counterDisplayUnit(mode, this.unit, this.rateUnit);
    return unitLabel
      ? `${value.toLocaleString()} ${unitLabel}`
      : value.toLocaleString();
  }

  private renderValueNodes(info: CounterDetails): m.Children {
    const mode = this.getMode();
    switch (mode) {
      case 'value':
        return [
          m(TreeNode, {
            left: 'Value',
            right: this.formatWithUnit(info.value, 'value'),
          }),
          m(TreeNode, {
            left: 'Delta',
            right: this.formatWithUnit(info.delta, 'delta'),
          }),
          m(TreeNode, {
            left: 'Rate',
            right: this.formatWithUnit(info.rate, 'rate'),
          }),
        ];
      case 'delta':
        return [
          m(TreeNode, {
            left: 'Value',
            right: this.formatWithUnit(info.value, 'value'),
          }),
          m(TreeNode, {
            left: 'Delta',
            right: this.formatWithUnit(info.delta, 'delta'),
          }),
        ];
      case 'rate':
        return m(TreeNode, {
          left: 'Rate',
          right: this.formatWithUnit(info.rate, 'rate'),
        });
      default:
        assertUnreachable(mode);
    }
  }

  render() {
    const counterInfo = this.counterDetails;
    if (counterInfo) {
      const args =
        hasArgs(counterInfo.args) &&
        m(
          Section,
          {title: 'Arguments'},
          m(Tree, renderArguments(this.trace, counterInfo.args)),
        );

      return m(
        DetailsShell,
        {title: 'Counter', description: `${this.trackName}`},
        m(
          GridLayout,
          m(
            Section,
            {title: 'Properties'},
            m(
              Tree,
              m(TreeNode, {left: 'Name', right: `${this.trackName}`}),
              m(TreeNode, {
                left: 'Start time',
                right: m(Timestamp, {trace: this.trace, ts: counterInfo.ts}),
              }),
              this.renderValueNodes(counterInfo),
              m(TreeNode, {
                left: 'Duration',
                right: m(DurationWidget, {
                  trace: this.trace,
                  dur: counterInfo.duration,
                }),
              }),
            ),
          ),
          args,
        ),
      );
    } else {
      return m(DetailsShell, {title: 'Counter', description: 'Loading...'});
    }
  }

  isLoading(): boolean {
    return this.counterDetails === undefined;
  }
}

async function loadCounterDetails(
  engine: Engine,
  id: number,
  sqlSource: string,
): Promise<CounterDetails> {
  const deltaExpr = counterValueExpression('delta');
  const rateExpr = counterValueExpression('rate');
  const query = `
    WITH src AS (
      SELECT
        id,
        ts,
        value,
        ${deltaExpr} as delta,
        ${rateExpr} as rate,
        arg_set_id
      FROM (${sqlSource})
    ),
    CURRENT AS (
      SELECT * FROM src WHERE id = ${id}
    ),
    NEXT as (
      SELECT
        ts
      FROM (${sqlSource})
      WHERE ts > (select ts from CURRENT)
      ORDER BY ts ASC
      LIMIT 1
    )
    SELECT
      ts as leftTs,
      value,
      delta,
      rate,
      arg_set_id as argSetId,
      (SELECT ts FROM NEXT) as rightTs
    FROM CURRENT
  `;

  const counter = await engine.query(query);
  const row = counter.iter({
    value: NUM,
    delta: NUM,
    rate: NUM,
    leftTs: LONG,
    rightTs: LONG_NULL,
    argSetId: NUM_NULL,
  });
  const leftTs = Time.fromRaw(row.leftTs);
  const rightTs = row.rightTs !== null ? Time.fromRaw(row.rightTs) : leftTs;
  const duration = rightTs - leftTs;
  const argSetId = row.argSetId;
  const args =
    argSetId == null ? undefined : await getArgs(engine, asArgSetId(argSetId));
  return {
    ts: leftTs,
    value: row.value,
    delta: row.delta,
    rate: row.rate,
    duration,
    args,
  };
}
