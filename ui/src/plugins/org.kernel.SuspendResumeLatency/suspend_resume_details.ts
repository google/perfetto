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

import {Duration, duration, Time, time} from '../../base/time';
import {LONG, NUM, STR_NULL} from '../../trace_processor/query_result';
import m from 'mithril';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';
import {Timestamp} from '../../frontend/widgets/timestamp';
import {DurationWidget} from '../../frontend/widgets/duration';
import {Anchor} from '../../widgets/anchor';
import {Engine} from '../../trace_processor/engine';
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {TrackEventSelection} from '../../public/selection';
import {Trace} from '../../public/trace';
import {ThreadMap} from '../dev.perfetto.Thread/threads';

interface SuspendResumeEventDetails {
  ts: time;
  dur: duration;
  utid: number;
  cpu: number;
  event_type: string;
  device_name: string;
  driver_name: string;
  callback_phase: string;
  thread_state_id: number;
}

export class SuspendResumeDetailsPanel implements TrackEventDetailsPanel {
  private suspendResumeEventDetails?: SuspendResumeEventDetails;

  constructor(
    private readonly trace: Trace,
    private readonly threads: ThreadMap,
  ) {}

  async load({eventId}: TrackEventSelection) {
    this.suspendResumeEventDetails = await loadSuspendResumeEventDetails(
      this.trace.engine,
      eventId,
    );
  }

  render() {
    const eventDetails = this.suspendResumeEventDetails;
    if (eventDetails) {
      const threadInfo = this.threads.get(eventDetails.utid);
      if (!threadInfo) {
        return null;
      }
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
              m(TreeNode, {
                left: 'Thread',
                right: m(
                  Anchor,
                  {
                    icon: 'call_made',
                    onclick: () => {
                      this.goToThread(eventDetails.thread_state_id);
                    },
                  },
                  `${threadInfo.threadName} [${threadInfo.tid}]`,
                ),
              }),
              m(TreeNode, {left: 'CPU', right: eventDetails.cpu}),
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

  goToThread(threadStateId: number) {
    this.trace.selection.selectSqlEvent('thread_state', threadStateId, {
      scrollToSelection: true,
    });
  }
}

async function loadSuspendResumeEventDetails(
  engine: Engine,
  id: number,
): Promise<SuspendResumeEventDetails> {
  const suspendResumeDetailsQuery = `
        SELECT ts,
               dur,
               EXTRACT_ARG(arg_set_id, 'utid') as utid,
               EXTRACT_ARG(arg_set_id, 'ucpu') as ucpu,
               EXTRACT_ARG(arg_set_id, 'event_type') as event_type,
               EXTRACT_ARG(arg_set_id, 'device_name') as device_name,
               EXTRACT_ARG(arg_set_id, 'driver_name') as driver_name,
               EXTRACT_ARG(arg_set_id, 'callback_phase') as callback_phase
        FROM slice
        WHERE slice_id = ${id};
    `;

  const suspendResumeDetailsResult = await engine.query(
    suspendResumeDetailsQuery,
  );
  const suspendResumeEventRow = suspendResumeDetailsResult.iter({
    ts: LONG,
    dur: LONG,
    utid: NUM,
    ucpu: NUM,
    event_type: STR_NULL,
    device_name: STR_NULL,
    driver_name: STR_NULL,
    callback_phase: STR_NULL,
  });
  if (!suspendResumeEventRow.valid()) {
    return {
      ts: Time.fromRaw(0n),
      dur: Duration.fromRaw(0n),
      utid: 0,
      cpu: 0,
      event_type: 'Error',
      device_name: 'Error',
      driver_name: 'Error',
      callback_phase: 'Error',
      thread_state_id: 0,
    };
  }

  const threadStateQuery = `
        SELECT t.id as threadStateId
        FROM thread_state t
        WHERE t.utid = ${suspendResumeEventRow.utid}
              AND t.ts <= ${suspendResumeEventRow.ts}
              AND t.ts + t.dur > ${suspendResumeEventRow.ts};
  `;
  const threadStateResult = await engine.query(threadStateQuery);
  let threadStateId = 0;
  if (threadStateResult.numRows() > 0) {
    const threadStateRow = threadStateResult.firstRow({
      threadStateId: NUM,
    });
    threadStateId = threadStateRow.threadStateId;
  }

  const cpuQuery = `
        SELECT cpu
        FROM cpu
        WHERE cpu.id = ${suspendResumeEventRow.ucpu}
  `;
  const cpuResult = await engine.query(cpuQuery);
  let cpu = 0;
  if (cpuResult.numRows() > 0) {
    const cpuRow = cpuResult.firstRow({
      cpu: NUM,
    });
    cpu = cpuRow.cpu;
  }

  return {
    ts: Time.fromRaw(suspendResumeEventRow.ts),
    dur: Duration.fromRaw(suspendResumeEventRow.dur),
    utid: suspendResumeEventRow.utid,
    cpu: cpu,
    event_type:
      suspendResumeEventRow.event_type !== null
        ? suspendResumeEventRow.event_type
        : 'N/A',
    device_name:
      suspendResumeEventRow.device_name !== null
        ? suspendResumeEventRow.device_name
        : 'N/A',
    driver_name:
      suspendResumeEventRow.driver_name !== null
        ? suspendResumeEventRow.driver_name
        : 'N/A',
    callback_phase:
      suspendResumeEventRow.callback_phase !== null
        ? suspendResumeEventRow.callback_phase
        : 'N/A',
    thread_state_id: threadStateId,
  };
}
