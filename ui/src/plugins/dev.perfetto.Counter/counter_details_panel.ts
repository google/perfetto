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
import {Timestamp} from '../../frontend/widgets/timestamp';
import {DurationWidget} from '../../frontend/widgets/duration';
import {TrackEventSelection} from '../../public/selection';
import {hasArgs, renderArguments} from '../../frontend/slice_args';
import {asArgSetId} from '../../trace_processor/sql_utils/core_types';
import {Arg, getArgs} from '../../trace_processor/sql_utils/args';

interface CounterDetails {
  // The "left" timestamp of the counter sample T(N)
  ts: time;

  // The delta between this sample and the next one's timestamps T(N+1) - T(N)
  duration: duration;

  // The value of the counter sample F(N)
  value: number;

  // The delta between this sample's value and the previous one F(N) - F(N-1)
  delta: number;

  args?: Arg[];
}

export class CounterDetailsPanel implements TrackEventDetailsPanel {
  private readonly trace: Trace;
  private readonly engine: Engine;
  private readonly trackId: number;
  private readonly rootTable: string;
  private readonly trackName: string;
  private counterDetails?: CounterDetails;

  constructor(
    trace: Trace,
    trackId: number,
    trackName: string,
    rootTable = 'counter',
  ) {
    this.trace = trace;
    this.engine = trace.engine;
    this.trackId = trackId;
    this.trackName = trackName;
    this.rootTable = rootTable;
  }

  async load({eventId}: TrackEventSelection) {
    this.counterDetails = await loadCounterDetails(
      this.engine,
      this.trackId,
      eventId,
      this.rootTable,
    );
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
                right: m(Timestamp, {ts: counterInfo.ts}),
              }),
              m(TreeNode, {
                left: 'Value',
                right: `${counterInfo.value.toLocaleString()}`,
              }),
              m(TreeNode, {
                left: 'Delta',
                right: `${counterInfo.delta.toLocaleString()}`,
              }),
              m(TreeNode, {
                left: 'Duration',
                right: m(DurationWidget, {dur: counterInfo.duration}),
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
  trackId: number,
  id: number,
  rootTable: string,
): Promise<CounterDetails> {
  const query = `
    WITH CTE AS (
      SELECT
        id,
        ts as leftTs,
        value,
        LAG(value) OVER (ORDER BY ts) AS prevValue,
        LEAD(ts) OVER (ORDER BY ts) AS rightTs,
        arg_set_id AS argSetId
      FROM ${rootTable}
      WHERE track_id = ${trackId}
    )
    SELECT * FROM CTE WHERE id = ${id}
  `;

  const counter = await engine.query(query);
  const row = counter.iter({
    value: NUM,
    prevValue: NUM_NULL,
    leftTs: LONG,
    rightTs: LONG_NULL,
    argSetId: NUM_NULL,
  });
  const value = row.value;
  const leftTs = Time.fromRaw(row.leftTs);
  const rightTs = row.rightTs !== null ? Time.fromRaw(row.rightTs) : leftTs;
  const prevValue = row.prevValue !== null ? row.prevValue : value;

  const delta = value - prevValue;
  const duration = rightTs - leftTs;
  const argSetId = row.argSetId;
  const args =
    argSetId == null ? undefined : await getArgs(engine, asArgSetId(argSetId));
  return {ts: leftTs, value, delta, duration, args};
}
