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

import m from 'mithril';
import {z} from 'zod';
import {ensureDefined} from '../../base/assert';
import type {Trace} from '../../public/trace';
import type {IntellettoToolRegistrar} from '../dev.perfetto.Intelletto';
import type SqlModulesPlugin from '../dev.perfetto.SqlModules';
import type {SqlModules} from '../dev.perfetto.SqlModules/sql_modules';
import {
  collectSpaghettiGraphErrors,
  formatSpaghettiGraphErrors,
  pickDefaultTargetNodeId,
  validateSpaghettiGraphJson,
} from './graph_check';
import {SPAGHETTI_GRAPH_FORMAT} from './graph_format';
import type {SpaghettiGraphState} from './graph_state';
import {
  flattenNodes,
  getOutputColumnsForNode,
  serializeStore,
  tryGetManifest,
} from './graph_utils';

export interface SelectedSpaghettiNodeContext {
  readonly nodeId: string;
  readonly type: string;
  readonly title: string;
  readonly config: unknown;
  readonly columns: readonly string[];
}

export function getSelectedNodeContext(
  state: SpaghettiGraphState,
  sqlModules: SqlModules | undefined,
): SelectedSpaghettiNodeContext | undefined {
  const activeNodeId = state.getActiveNodeId();
  if (activeNodeId === undefined) return undefined;
  const index = flattenNodes(state.store.nodes);
  const node = index.nodes[activeNodeId];
  if (node === undefined) return undefined;
  const title = tryGetManifest(node.type)?.title ?? node.type;
  const columns = (
    getOutputColumnsForNode(index, activeNodeId, sqlModules) ?? []
  ).map((c) => c.name);
  return {
    nodeId: node.id,
    type: node.type,
    title,
    config: node.config,
    columns,
  };
}

export function getActiveSpaghettiGraphJson(
  state: SpaghettiGraphState,
): string | undefined {
  if (state.store.nodes.length === 0) return undefined;
  return JSON.stringify(JSON.parse(serializeStore(state.store)), null, 2);
}

export async function dryRunSpaghettiGraph(
  trace: Trace,
  sqlModulesPlugin: SqlModulesPlugin,
  state: SpaghettiGraphState,
  json: string,
): Promise<string> {
  const validated = validateSpaghettiGraphJson(json);
  if (validated.errors.length > 0 || validated.store === undefined) {
    return (
      `Invalid Spaghetti graph (${validated.errors.length} problem` +
      `${validated.errors.length === 1 ? '' : 's'}):\n- ` +
      validated.errors.join('\n- ')
    );
  }

  await sqlModulesPlugin.ensureInitialized();
  const runtimeErrors = await collectSpaghettiGraphErrors(
    trace.engine,
    validated.store,
    sqlModulesPlugin.getSqlModules(),
    state.dialect,
  );
  if (runtimeErrors.length === 0) {
    return 'OK: graph is valid and runs cleanly.';
  }
  return (
    'Graph is structurally valid, but some nodes fail to run:\n' +
    formatSpaghettiGraphErrors(runtimeErrors)
  );
}

/**
 * Registers Spaghetti query builder graph tools and selected-node context
 * provider with Intelletto.
 */
export function registerSpaghettiTools(
  intelletto: IntellettoToolRegistrar,
  trace: Trace,
  sqlModulesPlugin: SqlModulesPlugin,
  state: SpaghettiGraphState,
): void {
  intelletto.registerTool({
    name: 'get_spaghetti_graph',
    description:
      'Read the current Spaghetti query builder graph as JSON. Call this to ' +
      'inspect what the user is building on the Spaghetti page (/spaghetti) ' +
      'before answering questions about it, and ALWAYS before ' +
      'set_spaghetti_graph when modifying an existing Spaghetti graph so you ' +
      'preserve existing nodes, ids, and positions. Returns "<empty>" when ' +
      'there are no nodes on the Spaghetti canvas.',
    shape: {},
    callback: async () => getActiveSpaghettiGraphJson(state) ?? '<empty>',
  });

  intelletto.registerTool({
    name: 'set_spaghetti_graph',
    description:
      'Replace the Spaghetti query builder graph with a new one and switch ' +
      'the UI to the Spaghetti page (/spaghetti) so the user sees the pipeline ' +
      'and its materialized query results. Use this when the user is working ' +
      'in Spaghetti or asks to build, edit, or visualize a query pipeline in ' +
      'Spaghetti. Structural errors (bad JSON, unknown node type, invalid ' +
      'config schema, docked source node, dangling input, or cycle) fail ' +
      'before changing the UI - read the errors, fix the JSON, and retry. If ' +
      'structurally valid, the graph is applied and per-node runtime SQL ' +
      'errors (if any) are returned so you can fix them and call ' +
      'set_spaghetti_graph again until it runs cleanly.\n\n' +
      SPAGHETTI_GRAPH_FORMAT,
    mutating: true,
    shape: {
      graph: z
        .string()
        .describe(
          'The complete Spaghetti graph as a JSON string in the documented ' +
            'format (an object with "nodes" and optional "labels").',
        ),
      selectedNodeId: z
        .string()
        .optional()
        .describe(
          'Optional node id to select so its SQL and results display in the ' +
            'bottom panel. Defaults to a sink node.',
        ),
    },
    callback: async ({graph, selectedNodeId}) => {
      const validated = validateSpaghettiGraphJson(graph);
      if (validated.errors.length > 0 || validated.store === undefined) {
        throw new Error(
          `Invalid Spaghetti graph (${validated.errors.length} problem` +
            `${validated.errors.length === 1 ? '' : 's'}):\n- ` +
            validated.errors.join('\n- '),
        );
      }

      await sqlModulesPlugin.ensureInitialized();
      const nextStore = ensureDefined(validated.store);
      const wasEmpty = state.store.nodes.length === 0;
      const targetNodeId = selectedNodeId ?? pickDefaultTargetNodeId(nextStore);

      state.replaceStore(nextStore, {
        pushHistory: true,
        selectedNodeId: targetNodeId,
        autoLayout: validated.needsAutoLayout,
        recenter: wasEmpty || validated.needsAutoLayout,
      });

      trace.navigate('#!/spaghetti');
      m.redraw();

      const errors = await collectSpaghettiGraphErrors(
        trace.engine,
        state.store,
        sqlModulesPlugin.getSqlModules(),
        state.dialect,
      );
      if (errors.length === 0) {
        return 'OK: Spaghetti graph applied and runs cleanly.';
      }
      return (
        'Graph applied, but some nodes fail to run. Fix these and call ' +
        'set_spaghetti_graph again:\n' +
        formatSpaghettiGraphErrors(errors)
      );
    },
  });

  intelletto.registerTool({
    name: 'check_spaghetti_graph',
    description:
      'Run the current Spaghetti query builder graph against the trace and ' +
      'report any per-node errors (incomplete config, missing input wires, ' +
      'bad SQL expressions, missing columns/tables) without modifying it. ' +
      'Returns "OK: graph runs cleanly." when there are none. Use this to ' +
      'diagnose why a Spaghetti graph is broken or verify edits.',
    shape: {},
    callback: async () => {
      await sqlModulesPlugin.ensureInitialized();
      const errors = await collectSpaghettiGraphErrors(
        trace.engine,
        state.store,
        sqlModulesPlugin.getSqlModules(),
        state.dialect,
      );
      if (errors.length === 0) {
        return 'OK: graph runs cleanly.';
      }
      return 'Graph has errors:\n' + formatSpaghettiGraphErrors(errors);
    },
  });

  intelletto.registerTool({
    name: 'validate_spaghetti_graph',
    description:
      'Dry-run a candidate Spaghetti graph JSON WITHOUT applying it - the ' +
      'active Spaghetti graph and UI are left untouched. Checks structural ' +
      'validity first and, if structurally sound, test-executes every node ' +
      'against the trace engine. Returns "OK: graph is valid and runs ' +
      'cleanly." when there are no issues. See set_spaghetti_graph for the ' +
      'JSON format.',
    shape: {
      graph: z
        .string()
        .describe(
          'Candidate Spaghetti graph JSON string, same format as ' +
            'set_spaghetti_graph.',
        ),
    },
    callback: async ({graph}) =>
      dryRunSpaghettiGraph(trace, sqlModulesPlugin, state, graph),
  });

  intelletto.registerContextProvider({
    id: 'dev.perfetto.Spaghetti#selected_node',
    description:
      'The active (pinned or selected) node in the Spaghetti query builder. ' +
      '"nodeId", "type", and "config" match the get_spaghetti_graph / ' +
      'set_spaghetti_graph JSON format; "columns" lists its output column ' +
      'names.',
    getContext: () => {
      const ctx = getSelectedNodeContext(
        state,
        sqlModulesPlugin.getSqlModules(),
      );
      if (ctx === undefined) return undefined;
      return {
        summary: `Spaghetti node: ${ctx.title}`,
        data: ctx,
      };
    },
  });
}
