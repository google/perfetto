import {McpServer} from '@modelcontextprotocol/sdk/server/mcp';
import {Engine} from 'src/trace_processor/engine';
import {z} from 'zod';
import {runQueryForMcp} from './query';

export function registerTraceTools(server: McpServer, engine: Engine) {
  server.tool(
    'execute-perfetto-trace-query',
    `
Tool to query a perfetto trace file.

The [query] param is SQL to execute against Perfetto's trace_processor.

If you are not sure about a query, then it's useful to show the SQL to the user and ask them to confirm.

The Perfetto SQL syntax is described here https://perfetto.dev/docs/analysis/perfetto-sql-syntax

Jank is a common topic and described here https://perfetto.dev/docs/data-sources/frametimeline, generally using [android_jank_cuj], or lower levels tables [actual_frame_timeline_slice], [expected_frame_timeline_slice]

Power is a less common topic and is described here https://perfetto.dev/docs/data-sources/battery-counters

CPU is described a bit here https://perfetto.dev/docs/data-sources/cpu-scheduling

Memory is described here https://perfetto.dev/docs/data-sources/memory-counters

Android logs are described here https://perfetto.dev/docs/data-sources/android-log        
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
    'list_android_processesin_trace',
    `
Tool to list processes.

This lists all the processes in the trace from the [package_list] with profileable and then debug apps first.  
        `,
    {},
    async ({}) => {
      const data = await runQueryForMcp(
        engine,
        `select
package_name as packageName,
version_code as versionCode,
debuggable,
profileable_from_shell as profileable
from package_list
order by profileable desc, debuggable desc`,
      );
      return {
        content: [{type: 'text', text: data}],
      };
    },
  );

  server.tool(
    'list_interesting_trace_tables',
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
`,
      );
      return {
        content: [{type: 'text', text: data}],
      };
    },
  );

  server.tool(
    'list_macrobenchmark_slices',
    `
        Tool to list macrobenchmark slices.
        
        This is relevant because when a trace file includes a macrobenchmark run (a slice called 'measureBlock') 
        then the user is probably interested in the target app and the specific range of time for that 'measureBlock'.

        If it's not in the android processes in the trace then ask the user to provide the name of the target process.
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
}
