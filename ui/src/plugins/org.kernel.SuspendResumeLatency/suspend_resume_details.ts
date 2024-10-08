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
import {globals} from '../../frontend/globals';
import {scrollTo} from '../../public/scroll_helper';
import {Engine} from '../../trace_processor/engine';
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {THREAD_STATE_TRACK_KIND} from '../../public/track_kinds';
import {TrackEventSelection} from '../../public/selection';
import {Trace} from '../../public/trace';

interface SuspendResumeEventDetails {
  ts: time;
  dur: duration;
  utid: number;
  event_type: string;
  device_name: string;
  driver_name: string;
  callback_phase: string;
  thread_state_id: number;
}

export class SuspendResumeDetailsPanel implements TrackEventDetailsPanel {
  private readonly trace: Trace;
  private suspendResumeEventDetails?: SuspendResumeEventDetails;

  constructor(trace: Trace) {
    this.trace = trace;
  }

  async load({eventId}: TrackEventSelection) {
    this.suspendResumeEventDetails = await loadSuspendResumeEventDetails(
      this.trace.engine,
      eventId,
    );
  }

  render() {
    const eventDetails = this.suspendResumeEventDetails;
    if (eventDetails) {
      const threadInfo = this.trace.threads.get(eventDetails.utid);
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
                      this.goToThread(
                        eventDetails.utid,
                        eventDetails.ts,
                        eventDetails.thread_state_id,
                      );
                    },
                  },
                  `${threadInfo.threadName} [${threadInfo.tid}]`,
                ),
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

  goToThread(utid: number, ts: time, threadStateId: number) {
    const threadInfo = this.trace.threads.get(utid);
    if (threadInfo === undefined) {
      return;
    }

    const trackDescriptor = globals.trackManager.findTrack(
      (td) =>
        td.tags?.kind === THREAD_STATE_TRACK_KIND &&
        td.tags?.utid === threadInfo.utid,
    );

    if (trackDescriptor) {
      globals.selectionManager.selectSqlEvent('thread_state', threadStateId);
      scrollTo({
        track: {uri: trackDescriptor.uri, expandGroup: true},
        time: {start: ts},
      });
    }
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

  return {
    ts: Time.fromRaw(suspendResumeEventRow.ts),
    dur: Duration.fromRaw(suspendResumeEventRow.dur),
    utid: suspendResumeEventRow.utid,
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
