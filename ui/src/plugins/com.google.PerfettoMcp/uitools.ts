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

import { McpServer } from '@modelcontextprotocol/sdk/server/mcp';
import { z } from 'zod';
import { Trace } from '../../public/trace';
import { Time } from '../../base/time';

export function registerUiTools(server: McpServer, trace: Trace) {
  // Automatically expose all registered commands as tools.
  // We sanitize the command ID to be a valid tool name.
  for (const cmd of trace.commands.commands) {
    const toolName = `perfetto-command-${cmd.id.replace(/[.#]/g, '-')}`;
    server.tool(
      toolName,
      `Executes the Perfetto command: ${cmd.name}`,
      {
        args: z.array(z.unknown()).optional().describe('Arguments for the command'),
      },
      async ({ args }) => {
        try {
          const result = trace.commands.runCommand(cmd.id, ...(args ?? []));
          return {
            content: [{ type: 'text', text: `Command executed. Result: ${JSON.stringify(result)}` }],
          };
        } catch (e) {
          return {
            content: [{ type: 'text', text: `Error executing command: ${String(e)}` }],
            isError: true,
          };
        }
      },
    );
  }

  server.tool(
    'perfetto-list-tracks',
    'Returns a list of all tracks currently available in the UI.',
    {},
    async () => {
      const tracks = trace.tracks.getAllTracks().map((t) => ({
        uri: t.uri,
        title: typeof t.description === 'string' ? t.description : t.uri,
        tags: t.tags,
      }));
      return {
        content: [{ type: 'text', text: JSON.stringify(tracks, null, 2) }],
      };
    },
  );

  server.tool(
    'perfetto-list-workspaces',
    'Returns a list of all workspaces and their track structure.',
    {},
    async () => {
      const workspaces = trace.workspaces.all.map((w) => ({
        id: w.id,
        title: w.title,
        pinnedTracks: w.pinnedTracks.map((t) => ({ uri: t.uri, name: t.name })),
        tracks: w.children.map((t) => ({ uri: t.uri, name: t.name })),
      }));
      return {
        content: [{ type: 'text', text: JSON.stringify(workspaces, null, 2) }],
      };
    },
  );

  server.tool(
    'perfetto-get-selection',
    'Returns the current selection state in the UI.',
    {},
    async () => {
      return {
        content: [{ type: 'text', text: JSON.stringify(trace.selection.selection, null, 2) }],
      };
    },
  );

  server.tool(
    'perfetto-pan-and-select',
    'Combined tool to pan the timeline to a time range and select a specific SQL event.',
    {
      startTime: z.string().optional().describe('Start time in nanoseconds'),
      endTime: z.string().optional().describe('End time in nanoseconds'),
      focusTable: z.string().optional().describe('SQL table name of the event to focus'),
      focusId: z.number().optional().describe('ID of the event to focus'),
    },
    async ({ startTime, endTime, focusTable, focusId }) => {
      if (startTime && endTime) {
        trace.timeline.panSpanIntoView(
          Time.fromRaw(BigInt(startTime)),
          Time.fromRaw(BigInt(endTime)),
          { align: 'zoom' },
        );
      }
      if (focusTable && focusId !== undefined) {
        trace.selection.selectSqlEvent(focusTable, focusId, {
          scrollToSelection: true,
          switchToCurrentSelectionTab: true,
        });
      }
      return {
        content: [{ type: 'text', text: 'OK' }],
      };
    },
  );

  // Legacy/Compatibility tools refactored to use standard commands
  server.tool(
    'show-perfetto-sql-view',
    'Shows a SQL query in the Perfetto SQL view. (Deprecated: use perfetto-command-dev-perfetto-RunQueryAndShowTab)',
    {
      query: z.string(),
      viewName: z.string(),
    },
    async ({ query, viewName }) => {
      trace.commands.runCommand('dev.perfetto.RunQueryAndShowTab', query, viewName);
      return {
        content: [{ type: 'text', text: 'OK' }],
      };
    },
  );

  server.tool(
    'show-timeline',
    'Shows context in the Timeline view. (Deprecated: use perfetto-pan-and-select)',
    {
      timeSpan: z.object({ startTime: z.string(), endTime: z.string() }).optional(),
      focus: z.object({ table: z.string(), id: z.number() }).optional(),
    },
    async ({ timeSpan, focus }) => {
      if (timeSpan) {
        trace.timeline.panSpanIntoView(
          Time.fromRaw(BigInt(timeSpan.startTime)),
          Time.fromRaw(BigInt(timeSpan.endTime)),
          { align: 'zoom' },
        );
      }
      if (focus) {
        trace.selection.selectSqlEvent(focus.table, focus.id, {
          scrollToSelection: true,
          switchToCurrentSelectionTab: true,
        });
      }
      return {
        content: [{ type: 'text', text: 'OK' }],
      };
    },
  );
}
