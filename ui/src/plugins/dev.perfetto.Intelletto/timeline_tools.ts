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

import {z} from 'zod';
import type {Trace} from '../../public/trace';
import {Time} from '../../base/time';
import type {ToolRegistry} from './tools';

export function registerTimelineTools(reg: ToolRegistry, trace: Trace): void {
  reg.registerTool({
    name: 'get_selection',
    description:
      'Read what the user currently has selected in the UI (a track event, a ' +
      'time-range area selection, a track, or nothing). Use this to resolve ' +
      'what the user means by "this" / "the selected slice".',
    shape: {},
    callback: async () => {
      const sel = trace.selection.selection;
      switch (sel.kind) {
        case 'track_event':
          return JSON.stringify({
            kind: 'track_event',
            trackUri: sel.trackUri,
            eventId: sel.eventId,
            ts: Number(sel.ts),
            dur: sel.dur === undefined ? undefined : Number(sel.dur),
          });
        case 'area':
          return JSON.stringify({
            kind: 'area',
            start: Number(sel.start),
            end: Number(sel.end),
            trackUris: sel.trackUris,
          });
        case 'track':
          return JSON.stringify({kind: 'track', trackUri: sel.trackUri});
        case 'note':
          return JSON.stringify({kind: 'note', id: sel.id});
        case 'empty':
          return JSON.stringify({kind: 'empty'});
      }
    },
  });

  reg.registerTool({
    name: 'select_track_event',
    description:
      'Select a trace event by its track URI and event id (e.g. trackUri ' +
      '"foo", eventId 1234), scrolling the timeline to it. This is the ' +
      'low-level counterpart to select_sql_event: use it when you already ' +
      'have a trackUri/eventId pair - either from get_selection (a ' +
      'track_event kind) or from resolving a SQL row yourself. Prefer ' +
      'select_sql_event when you only have a SQL table and row id, since it ' +
      'handles the lookup.',
    mutating: true,
    shape: {
      trackUri: z.string().describe('URI of the track containing the event.'),
      eventId: z.number().describe('Event id within that track.'),
    },
    callback: async ({trackUri, eventId}) => {
      trace.selection.selectTrackEvent(trackUri, eventId, {
        scrollToSelection: true,
        switchToCurrentSelectionTab: true,
      });
      return 'OK';
    },
  });

  reg.registerTool({
    name: 'select_sql_event',
    description:
      'Select a single trace event by its SQL table and row id (e.g. table ' +
      '"slice", id 1234), scrolling the timeline to it. Use this to point ' +
      'the user at a specific slice/event you found via run_query.',
    mutating: true,
    shape: {
      table: z.string().describe('SQL table the id refers to, e.g. "slice".'),
      id: z.number().describe('Row id within that table.'),
    },
    callback: async ({table, id}) => {
      trace.selection.selectSqlEvent(table, id, {
        scrollToSelection: true,
        switchToCurrentSelectionTab: true,
      });
      return 'OK';
    },
  });

  reg.registerTool({
    name: 'show_timeline',
    description:
      'Pan/zoom the timeline to a time range so the user can see it. ' +
      'Timestamps are trace-processor nanoseconds (bigints sent as ' +
      'strings); query the min/max of the relevant table to get a valid ' +
      'range.',
    mutating: true,
    shape: {
      startTime: z.string().describe('Range start, ns, as a string bigint.'),
      endTime: z.string().describe('Range end, ns, as a string bigint.'),
    },
    callback: async ({startTime, endTime}) => {
      const start = Time.fromRaw(BigInt(startTime));
      const end = Time.fromRaw(BigInt(endTime));
      trace.timeline.panSpanIntoView(start, end, {align: 'zoom'});
      return 'OK';
    },
  });

  reg.registerTool({
    name: 'select_area',
    description:
      'Select a time range on the timeline (an "area selection"), which ' +
      'drives the aggregation panels. Optionally restrict to specific ' +
      'tracks by their URIs; omit to select the range across all tracks. ' +
      'Timestamps are trace-processor nanoseconds as string bigints.',
    mutating: true,
    shape: {
      startTime: z.string().describe('Range start, ns, as a string bigint.'),
      endTime: z.string().describe('Range end, ns, as a string bigint.'),
      trackUris: z
        .array(z.string())
        .optional()
        .describe('Track URIs to restrict the selection to. Omit for all.'),
    },
    callback: async ({startTime, endTime, trackUris}) => {
      const start = Time.fromRaw(BigInt(startTime));
      const end = Time.fromRaw(BigInt(endTime));
      trace.selection.selectArea(
        {start, end, trackUris: trackUris ?? []},
        {scrollToSelection: true},
      );
      return 'OK';
    },
  });

  reg.registerTool({
    name: 'get_viewport',
    description:
      'Read the timeline viewport - the start and end (trace-processor ' +
      'nanoseconds) of the time range currently visible. Use this to know ' +
      'what window the user is looking at before querying or focusing.',
    shape: {},
    callback: async () => {
      const span = trace.timeline.visibleWindow.toTimeSpan();
      return JSON.stringify({
        start: Number(span.start),
        end: Number(span.end),
      });
    },
  });
}
