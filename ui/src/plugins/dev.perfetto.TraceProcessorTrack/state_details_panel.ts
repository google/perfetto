// Copyright (C) 2026 The Android Open Source Project
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

import {Time, type duration, type time} from '../../base/time';
import type {Engine} from '../../trace_processor/engine';
import type {Trace} from '../../public/trace';
import {LONG, NUM_NULL, STR_NULL} from '../../trace_processor/query_result';
import type {TrackEventDetailsPanel} from '../../public/details_panel';
import m from 'mithril';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';
import {Timestamp} from '../../components/widgets/timestamp';
import {DurationWidget} from '../../components/widgets/duration';
import type {TrackEventSelection} from '../../public/selection';
import {hasArgs, renderArguments} from '../../components/details/args';
import {asArgSetId} from '../../components/sql_utils/core_types';
import {type ArgsDict, getArgs} from '../../components/sql_utils/args';

interface StateDetails {
  ts: time;
  duration: duration;
  value: string;
  category?: string;
  args?: ArgsDict;
}

export class StateDetailsPanel implements TrackEventDetailsPanel {
  private readonly trace: Trace;
  private readonly engine: Engine;
  private readonly trackName: string;
  private stateDetails?: StateDetails;

  constructor(trace: Trace, trackName: string) {
    this.trace = trace;
    this.engine = trace.engine;
    this.trackName = trackName;
  }

  async load({eventId}: TrackEventSelection) {
    this.stateDetails = await loadStateDetails(this.engine, eventId);
  }

  render() {
    const stateInfo = this.stateDetails;
    if (stateInfo) {
      const args =
        hasArgs(stateInfo.args) &&
        m(
          Section,
          {title: 'Arguments'},
          m(Tree, renderArguments(this.trace, stateInfo.args)),
        );

      return m(
        DetailsShell,
        {title: 'State', description: `${this.trackName}`},
        m(
          GridLayout,
          m(
            Section,
            {title: 'Properties'},
            m(
              Tree,
              m(TreeNode, {left: 'Track', right: `${this.trackName}`}),
              m(TreeNode, {left: 'Value', right: `${stateInfo.value}`}),
              m(TreeNode, {
                left: 'Start time',
                right: m(Timestamp, {trace: this.trace, ts: stateInfo.ts}),
              }),
              m(TreeNode, {
                left: 'Duration',
                right: m(DurationWidget, {
                  trace: this.trace,
                  dur: stateInfo.duration,
                }),
              }),
              stateInfo.category &&
                m(TreeNode, {left: 'Category', right: `${stateInfo.category}`}),
            ),
          ),
          args,
        ),
      );
    } else {
      return m(DetailsShell, {title: 'State', description: 'Loading...'});
    }
  }

  isLoading(): boolean {
    return this.stateDetails === undefined;
  }
}

async function loadStateDetails(
  engine: Engine,
  id: number,
): Promise<StateDetails> {
  const query = `
    SELECT
      ts,
      dur,
      value,
      category,
      arg_set_id as argSetId
    FROM state
    WHERE id = ${id}
  `;

  const result = await engine.query(query);
  const row = result.iter({
    ts: LONG,
    dur: LONG,
    value: STR_NULL,
    category: STR_NULL,
    argSetId: NUM_NULL,
  });

  const ts = Time.fromRaw(row.ts);
  const duration = Time.fromRaw(row.dur);
  const argSetId = row.argSetId;
  const args =
    argSetId == null ? undefined : await getArgs(engine, asArgSetId(argSetId));

  return {
    ts,
    duration,
    value: row.value ?? '[null]',
    category: row.category ?? undefined,
    args,
  };
}
