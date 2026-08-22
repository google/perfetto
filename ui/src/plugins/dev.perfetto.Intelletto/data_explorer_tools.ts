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
import DataExplorerPlugin from '../dev.perfetto.DataExplorer';
import {GRAPH_FORMAT} from '../dev.perfetto.DataExplorer/graph_format';
import {formatGraphErrors} from '../dev.perfetto.DataExplorer/graph_check';
import type {ContextRegistry} from './context';
import type {ToolRegistry} from './tools';

/**
 * Register the Data Explorer's graph tools (get_graph, set_graph, check_graph,
 * validate_graph) and selected-node context provider against the Intelletto
 * registries. The tool descriptions live here (Intelletto owns the model-
 * facing prose); the implementations call the Data Explorer plugin's public
 * hooks (getActiveGraphJson, setActiveGraphJson, checkActiveGraph, dryRunGraph,
 * getSelectedNodeContext). DE has no dependency on Intelletto and isn't
 * transitively enabled by it.
 */
export function registerDataExplorerTools(
  tools: ToolRegistry,
  context: ContextRegistry,
  trace: Trace,
): void {
  const de = trace.plugins.getPlugin(DataExplorerPlugin);

  tools.registerTool({
    name: 'get_graph',
    description:
      'Read the current Data Explorer query graph as JSON. Call this to see ' +
      'what the user is exploring before answering questions about it, and ' +
      'ALWAYS before set_graph when editing an existing graph - copy its ' +
      'shape rather than rebuilding from scratch. Returns the string ' +
      '"<empty>" when there is no graph yet.',
    shape: {},
    callback: async () => de.getActiveGraphJson() ?? '<empty>',
  });

  tools.registerTool({
    name: 'set_graph',
    description:
      'Replace the Data Explorer query graph with a new one and switch the ' +
      'UI to the Data Explorer so the user sees it. Use this when the user ' +
      'asks you to build, change, or visualise a query/pipeline in the Data ' +
      'Explorer. The argument is the whole graph as a JSON string. A ' +
      'structurally invalid graph (bad JSON, unknown node type, dangling or ' +
      'one-sided edge) comes back as a tool error listing every problem - ' +
      'fix them and retry. If the graph is structurally fine but a node ' +
      'fails to run (bad SQL, missing column/table), it is still applied and ' +
      'the per-node runtime errors are returned; fix the SQL and call ' +
      'set_graph again until it reports it runs cleanly.\n\n' +
      GRAPH_FORMAT,
    mutating: true,
    shape: {
      graph: z
        .string()
        .describe(
          'The complete graph, as a JSON string in the documented format ' +
            '(an object with "nodes" and "rootNodeIds").',
        ),
    },
    callback: async ({graph}) => {
      // Throws (-> tool error) on structural problems, before any UI change.
      de.setActiveGraphJson(trace, graph);
      // Applied; now report any runtime errors so the model can iterate.
      const errors = await de.checkActiveGraph(trace);
      if (errors.length === 0) {
        return 'OK: graph applied and runs cleanly.';
      }
      return (
        'Graph applied, but some nodes fail to run. Fix these and call ' +
        'set_graph again:\n' +
        formatGraphErrors(errors)
      );
    },
  });

  tools.registerTool({
    name: 'check_graph',
    description:
      'Run the current Data Explorer graph against the trace and report any ' +
      'per-node errors (bad SQL, missing columns/tables, invalid config) ' +
      'without changing it. Returns "OK: graph runs cleanly." when there are ' +
      'none. Use this to verify the graph after editing, or to diagnose what ' +
      'the user means by "my graph is broken".',
    shape: {},
    callback: async () => {
      const errors = await de.checkActiveGraph(trace);
      if (errors.length === 0) {
        return 'OK: graph runs cleanly.';
      }
      return 'Graph has errors:\n' + formatGraphErrors(errors);
    },
  });

  tools.registerTool({
    name: 'validate_graph',
    description:
      'Check a candidate graph JSON for problems WITHOUT applying it - the ' +
      'current graph and the UI are left untouched. Reports structural ' +
      'problems (bad JSON, unknown node type, dangling or one-sided edges) ' +
      'and, if structurally sound, runtime errors from running it (bad SQL, ' +
      'missing columns/tables). Returns "OK: graph is valid and runs ' +
      'cleanly." when there are none. Use this to iterate on a graph before ' +
      'committing it with set_graph. See set_graph for the JSON format.',
    shape: {
      graph: z
        .string()
        .describe(
          'The candidate graph as a JSON string, same format as set_graph.',
        ),
    },
    callback: async ({graph}) => de.dryRunGraph(trace, graph),
  });

  // Tell the assistant which node the user currently has selected in the
  // query builder, so it can answer "this node" / "the selected step"
  // questions and edit the right node without asking.
  context.registerContextProvider({
    id: 'dev.perfetto.DataExplorer#selected_node',
    description:
      'The node the user has selected in the Data Explorer query builder. ' +
      '"nodeId" and "type" match the get_graph / set_graph JSON format, so ' +
      'the selected node can be located and edited there. "state" is that ' +
      'node\'s serialized config; "columns" are the column names it outputs.',
    getContext: () => {
      const ctx = de.getSelectedNodeContext();
      if (ctx === undefined) return undefined;
      return {
        summary: `Selected node: ${ctx.title}`,
        data: ctx,
      };
    },
  });
}
