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

import {McpServer} from '@modelcontextprotocol/sdk/server/mcp';
import {z} from 'zod';
import {addQueryResultsTab} from '../../components/query_table/query_result_tab';
import {Trace} from '../../public/trace';
import {Time} from '../../base/time';

export function registerUiTools(server: McpServer, ctxt: Trace) {
  server.tool(
    'show-perfetto-sql-view',
    `Shows a SQL query in the Perfetto SQL view.`,
    {
      query: z.string(),
      viewName: z.string(),
    },
    async ({query, viewName}) => {
      addQueryResultsTab(ctxt, {
        query: query,
        title: viewName,
      });

      return {
        content: [{type: 'text', text: 'OK'}],
      };
    },
  );

  server.tool(
    'show-timeline',
    `
      Shows some context in the Timeline view.
      'timeSpan' controls the range of time to be shown.
      'focus' controls the row to be shown. For example { table: 'slice', id: 1234 }
    `,
    {
      timeSpan: z
        .object({
          startMicros: z.number(),
          endMicros: z.number(),
        })
        .optional(),
      focus: z
        .object({
          table: z.string(),
          id: z.number(),
        })
        .optional(),
    },
    async ({timeSpan, focus}) => {
      if (timeSpan) {
        ctxt.timeline.setViewportTime(
          Time.fromMicros(timeSpan.startMicros),
          Time.fromMicros(timeSpan.endMicros),
        );
      }
      if (focus) {
        ctxt.selection.selectSqlEvent(focus.table, focus.id, {
          scrollToSelection: true,
          switchToCurrentSelectionTab: true,
        });
      }

      return {
        content: [{type: 'text', text: 'OK'}],
      };
    },
  );
}
