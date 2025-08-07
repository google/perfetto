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
import {Engine} from 'src/trace_processor/engine';
import {z} from 'zod';
import {runQueryForMcp} from './query';

export function registerTraceTools(server: McpServer, engine: Engine) {
  server.tool(
    'perfetto-execute-query',
    `
       Tool to query the perfetto trace file loaded in Perfetto UI currently.

       The query is SQL to execute against Perfetto's trace_processor.

       If you are not sure about a query, then it's useful to show the SQL to the user and ask them to confirm.

       The stdlib is documented at https://perfetto.dev/docs/analysis/stdlib-docs
       It is worth fetching this fully in order to use best practices in queries.

       It's generally faster to use the existing stdlib tables, and aggregated results rather than
       querying large result sets and processing after retrieved. So reuse standard views where possible
       In addition, if querying some of the perfetto modules listed are resulting in error or empty results,
       try using the prelude module listed at https://perfetto.dev/docs/analysis/stdlib-docs#package-prelude

       The Perfetto SQL syntax is described here https://perfetto.dev/docs/analysis/perfetto-sql-syntax

       Jank is a common topic and described here https://perfetto.dev/docs/data-sources/frametimeline
       Using the information in expected_frame_timeline_slice and actual_frame_timeline_slice as the primary
       source for jank is preferred.

       Power is a less common topic and is described here https://perfetto.dev/docs/data-sources/battery-counters

       CPU is described a bit here https://perfetto.dev/docs/data-sources/cpu-scheduling

       Memory is described here https://perfetto.dev/docs/data-sources/memory-counters

       Android logs are described here https://perfetto.dev/docs/data-sources/android-log

       The perfetto stdlib can be included by executing
        \`INCLUDE PERFETTO MODULE\` for \`viz.*\`, \`slices.*\`, \`android.*\`. More can be loaded dynamically if
        needed. But loading extra must always be done in separate queries or it messes up the SQL results.
        `,
    {query: z.string()},
    async ({query}) => {
      const data = await runQueryForMcp(engine, query);
      return {
        content: [{type: 'text', text: data}],
      };
    },
  );

  server.tool(
    'perfetto-list-android-processes',
    `
        Tool to list process details from the trace.

        This lists all the processes in the trace from the \`process\` table.
        `,
    {},
    async ({}) => {
      const data = await runQueryForMcp(
        engine,
        `select *
      from process`,
      );
      return {
        content: [{type: 'text', text: data}],
      };
    },
  );

  server.tool(
    'perfetto-list-interesting-tables',
    `
        Tool to list interesting tables and views.
        
        It's basically a query on [sqlite_schema], but excluding 'sqlite_' and '_' prefixed tables which tend to
        be internal implementation details.
        
        This is relevant if queries aren't working, they may need to be loaded via the 'INCLUDE PERFETTO MODULE'
        query.
         
        If tables you expect to be there based on public samples aren't, please mention it so that the user can
        tweak the tool to automatically include them.
        `,
    {},
    async ({}) => {
      const data = await runQueryForMcp(
        engine,
        `
        SELECT 
            name, type
        FROM 
            sqlite_schema
        WHERE 
            type in ('table', 'view') 
            AND name NOT LIKE 'sqlite_%'
            AND name NOT LIKE '\_%' ESCAPE '\'
`,
      );
      return {
        content: [{type: 'text', text: data}],
      };
    },
  );

  server.tool(
    'perfetto-list-macrobenchmark-slices',
    `
        Tool to list macrobenchmark slices.
        
        This is relevant because when a trace file includes a macrobenchmark run (a slice called 'measureBlock') 
        then the user is probably interested in the target app and the specific range of time for that 'measureBlock'.      

        So a \`measureBlock\` in the app \`com.google.android.horologist.mediasample.benchmark\`, 
        would usually be testing against an app called \`com.google.android.horologist.mediasample\`.
        But this is not always true, so ask the user if it's missing.
        `,
    {},
    async ({}) => {
      const data = await runQueryForMcp(
        engine,
        `
        SELECT
          s.name AS slice_name,
          s.ts,
          s.dur,
          t.name AS thread_name,
          p.name AS process_name
        FROM
          slice s
        JOIN
          thread_track tt ON s.track_id = tt.id
        JOIN
          thread t ON tt.utid = t.utid
        JOIN
          process p ON t.upid = p.upid
        WHERE
          s.name = 'measureBlock'
        ORDER BY
          s.ts
`,
      );
      return {
        content: [{type: 'text', text: data}],
      };
    },
  );

  server.tool(
    'perfetto-list-table-structure',
    `
        Tool to list the structure of a table.
        
        It's basically a query of \`pragma table_info('TABLE_NAME')\`.   
        `,
    {table: z.string()},
    async ({table}) => {
      const data = await runQueryForMcp(
        engine,
        `pragma table_info('${table}')`,
      );
      return {
        content: [{type: 'text', text: data}],
      };
    },
  );
}
