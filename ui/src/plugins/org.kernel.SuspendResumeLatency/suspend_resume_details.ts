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

import {AsyncLimiter} from '../../base/async_limiter';
import {Duration, duration, Time, time} from '../../base/time';
import {raf} from '../../core/raf_scheduler';
import {LONG, NUM, STR_NULL} from '../../trace_processor/query_result';
import m from 'mithril';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';
import {Timestamp} from '../../frontend/widgets/timestamp';
import {DurationWidget} from '../../frontend/widgets/duration';
import {Engine} from '../../trace_processor/engine';
import {TrackSelectionDetailsPanel} from '../../public/details_panel';

interface SuspendResumeEventDetails {
  ts: time;
  dur: duration;
  utid: number;
  event_type: string;
  device_name: string;
  driver_name: string;
  callback_phase: string;
}

export class SuspendResumeDetailsPanel implements TrackSelectionDetailsPanel {
  private readonly queryLimiter = new AsyncLimiter();
  private readonly engine: Engine;
  private id?: number;
  private suspendResumeEventDetails?: SuspendResumeEventDetails;

  constructor(engine: Engine) {
    this.engine = engine;
  }

  render(id: number): m.Children {
    if (id !== this.id) {
      this.id = id;
      this.queryLimiter.schedule(async () => {
        this.suspendResumeEventDetails = await loadSuspendResumeEventDetails(
          this.engine,
          id,
        );
        raf.scheduleFullRedraw();
      });
    }

    return this.renderView();
  }

  private renderView() {
    const eventDetails = this.suspendResumeEventDetails;
    if (eventDetails) {
      return m(
        DetailsShell,
        {title: 'Suspend / Resume Event'},
        m(
          GridLayout,
          m(
            Section,
            {title: 'Properties'},
            m(
              Tree,
              m(TreeNode, {
                left: 'Device Name',
                right: eventDetails.device_name,
              }),
              m(TreeNode, {
                left: 'Start time',
                right: m(Timestamp, {ts: eventDetails.ts}),
              }),
              m(TreeNode, {
                left: 'Duration',
                right: m(DurationWidget, {dur: eventDetails.dur}),
              }),
              m(TreeNode, {
                left: 'Driver Name',
                right: eventDetails.driver_name,
              }),
              m(TreeNode, {
                left: 'Callback Phase',
                right: eventDetails.callback_phase,
              }),
              m(TreeNode, {left: 'Event Type', right: eventDetails.event_type}),
            ),
          ),
        ),
      );
    } else {
      return m(DetailsShell, {
        title: 'Suspend / Resume Event',
        description: 'Loading...',
      });
    }
  }

  isLoading(): boolean {
    return this.suspendResumeEventDetails === undefined;
  }
}

async function loadSuspendResumeEventDetails(
  engine: Engine,
  id: number,
): Promise<SuspendResumeEventDetails> {
  const query = `
        SELECT ts,
               dur,
               EXTRACT_ARG(arg_set_id, 'utid') as utid,
               EXTRACT_ARG(arg_set_id, 'event_type') as event_type,
               EXTRACT_ARG(arg_set_id, 'device_name') as device_name,
               EXTRACT_ARG(arg_set_id, 'driver_name') as driver_name,
               EXTRACT_ARG(arg_set_id, 'callback_phase') as callback_phase
        FROM slice
        WHERE slice_id = ${id};
    `;

  const result = await engine.query(query);
  const row = result.iter({
    ts: LONG,
    dur: LONG,
    utid: NUM,
    event_type: STR_NULL,
    device_name: STR_NULL,
    driver_name: STR_NULL,
    callback_phase: STR_NULL,
  });
  if (!row.valid()) {
    return {
      ts: Time.fromRaw(0n),
      dur: Duration.fromRaw(0n),
      utid: 0,
      event_type: 'Error',
      device_name: 'Error',
      driver_name: 'Error',
      callback_phase: 'Error',
    };
  }

  return {
    ts: Time.fromRaw(row.ts),
    dur: Duration.fromRaw(row.dur),
    utid: row.utid,
    event_type: row.event_type !== null ? row.event_type : 'N/A',
    device_name: row.device_name !== null ? row.device_name : 'N/A',
    driver_name: row.driver_name !== null ? row.driver_name : 'N/A',
    callback_phase: row.callback_phase !== null ? row.callback_phase : 'N/A',
  };
}
