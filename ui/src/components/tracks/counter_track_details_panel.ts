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
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {Trace} from '../../public/trace';
import {SourceDataset} from '../../trace_processor/dataset';
import {sqlValueToReadableString} from '../../trace_processor/sql_utils';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';
import {Timestamp} from '../widgets/timestamp';
import {CounterRowSchema} from './counter_track';
import {exists} from '../../base/utils';
import {duration, time, Time} from '../../base/time';
import {
  LONG,
  LONG_NULL,
  NUM,
  NUM_NULL,
} from '../../trace_processor/query_result';
import {Engine} from '../../trace_processor/engine';
import {Arg} from '../sql_utils/args';
import {DurationWidget} from '../widgets/duration';

/**
 * Default details panel for CounterTrack that displays all fields from
 * the dataset query.
 *
 * This panel provides a "better than nothing" experience when no custom
 * details panel is specified. It automatically shows:
 * - Common counter fields (ts, value) with appropriate formatting
 * - All other dataset columns as readable strings
 */
export class CounterTrackDetailsPanel<T extends CounterRowSchema>
  implements TrackEventDetailsPanel
{
  private counterDetails?: CounterDetails;

  constructor(
    private readonly trace: Trace,
    private readonly dataset: SourceDataset<T>,
    private readonly data: T,
    private readonly rootTableOrSubquery?: string,
  ) {}

  async load() {
    if (this.data.id !== undefined && this.rootTableOrSubquery) {
      this.counterDetails = await loadCounterDetails(
        this.trace.engine,
        this.data.id,
        this.rootTableOrSubquery,
      );
    }
  }

  render() {
    const data = this.data;
    const details = this.counterDetails;

    return m(
      DetailsShell,
      {
        title: 'Counter',
      },
      m(
        GridLayout,
        m(
          Section,
          {title: 'Details'},
          m(Tree, [
            // Basic counter information
            exists(data.id) &&
              m(TreeNode, {
                left: 'ID',
                right: data.id,
              }),
            m(TreeNode, {
              left: 'Timestamp',
              right: m(Timestamp, {
                trace: this.trace,
                ts: Time.fromRaw(data.ts),
              }),
            }),
            m(TreeNode, {
              left: 'Value',
              right: data.value.toLocaleString(),
            }),

            // Enhanced details if available
            details && [
              m(TreeNode, {
                left: 'Duration to next',
                right: m(DurationWidget, {
                  trace: this.trace,
                  dur: details.duration,
                }),
              }),
              m(TreeNode, {
                left: 'Delta from previous',
                right: details.delta.toLocaleString(),
              }),
              // Show args if available
              details.args &&
                details.args.length > 0 && [
                  m(TreeNode, {
                    left: 'Arguments',
                    right: '',
                  }),
                  ...details.args.map((arg) =>
                    m(TreeNode, {
                      left: `  ${arg.key}`,
                      right: arg.displayValue,
                    }),
                  ),
                ],
            ],

            // List all other fields from the dataset's schema
            ...Object.keys(this.dataset.schema)
              .filter((key) => !['id', 'ts', 'value'].includes(key))
              .map((key) => {
                const value = data[key];
                return m(TreeNode, {
                  left: key,
                  right: sqlValueToReadableString(value),
                });
              }),
          ]),
        ),
      ),
    );
  }
}

interface CounterDetails {
  // The "left" timestamp of the counter sample T(N)
  readonly ts: time;

  // The delta between this sample and the next one's timestamps T(N+1) - T(N)
  readonly duration: duration;

  // The value of the counter sample F(N)
  readonly value: number;

  // The delta between this sample's value and the previous one F(N) - F(N-1)
  readonly delta: number;

  readonly args?: Arg[];
}

async function loadCounterDetails(
  engine: Engine,
  id: number,
  rootTableOrSubquery: string,
): Promise<CounterDetails> {
  const query = `
    WITH data AS (
      SELECT * FROM (${rootTableOrSubquery})
    ),
    CURRENT AS (
      SELECT
        id,
        ts,
        value
      FROM data
      WHERE id = ${id}
    ),
    PREV AS (
      SELECT
        value
      FROM data
      WHERE ts < (SELECT ts FROM CURRENT)
      ORDER BY ts DESC
      LIMIT 1
    ),
    NEXT AS (
      SELECT
        ts
      FROM data
      WHERE ts > (SELECT ts FROM CURRENT)
      ORDER BY ts ASC
      LIMIT 1
    )
    SELECT
      id,
      ts as leftTs,
      value,
      (SELECT value FROM PREV) as prevValue,
      (SELECT ts FROM NEXT) as rightTs
    FROM CURRENT
  `;

  const counter = await engine.query(query);
  const row = counter.iter({
    value: NUM,
    prevValue: NUM_NULL,
    leftTs: LONG,
    rightTs: LONG_NULL,
  });

  if (!row.valid()) {
    throw new Error(`Counter with id ${id} not found`);
  }

  const value = row.value;
  const leftTs = Time.fromRaw(row.leftTs);
  const rightTs = row.rightTs !== null ? Time.fromRaw(row.rightTs) : leftTs;
  const prevValue = row.prevValue !== null ? row.prevValue : value;

  const delta = value - prevValue;
  const duration = rightTs - leftTs;
  // const args =
  //   argSetId == null ? undefined : await getArgs(engine, asArgSetId(argSetId));
  return {ts: leftTs, value, delta, duration};
}
