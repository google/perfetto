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

// dev.perfetto.IntellettoTimelineTools - contributes timeline-specific tools to
// the Intelletto assistant. These could live in core_tools, but they belong
// with the timeline: the assistant doesn't need to know the timeline exists,
// only that tools drive it. This plugin is the bridge - it depends on both the
// Timeline plugin (the capability) and Intelletto (the registry), so neither a
// core plugin nor the assistant has to know about the other. It's the worked
// example of plugins owning their own tools.

import {z} from 'zod';
import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import {Time} from '../../base/time';
import IntellettoPlugin from '../dev.perfetto.Intelletto';

export default class IntellettoTimelineToolsPlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.IntellettoTimelineTools';
  static readonly description =
    'Registers timeline tools (select an event, focus a time range, select ' +
    'an area, read the viewport) with the Intelletto assistant.';
  // Depends only on Intelletto (the registry). The timeline capabilities these
  // tools use - trace.selection and trace.timeline - are always present, so we
  // don't depend on the Timeline plugin itself; enabling this plugin requires
  // only that the assistant is available to register against.
  static readonly dependencies = [IntellettoPlugin];

  async onTraceLoad(trace: Trace): Promise<void> {
    const intelletto = trace.plugins.getPlugin(IntellettoPlugin);

    intelletto.registerTool({
      name: 'select_event',
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

    intelletto.registerTool({
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

    intelletto.registerTool({
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

    intelletto.registerTool({
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
}
