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
}
