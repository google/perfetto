import { McpServer } from '@modelcontextprotocol/sdk/server/mcp';
import { z } from 'zod';
import {addQueryResultsTab} from '../../components/query_table/query_result_tab';
import { Trace } from 'src/public/trace';

export function registerUiTools(server: McpServer, ctxt: Trace) {
  server.tool(
    'show-perfetto-sql-view',
    `Shows a SQL query in the Perfetto SQL view.`,
    {
      query: z.string(),
      viewName: z.string(),
    },
    async ({ query, viewName }) => {
      console.log('show-perfetto-sql-view', query, viewName);

      addQueryResultsTab(ctxt, {
        query: query,
        title: viewName,
      });

      return {
        content: [{ type: 'text', text: "OK" }],
      };
    },
  );
}
