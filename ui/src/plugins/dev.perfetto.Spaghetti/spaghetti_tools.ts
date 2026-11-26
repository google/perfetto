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
  checkSpaghettiGraph,
  formatSerializedSql,
  formatSpaghettiGraphErrors,
  pickDefaultTargetNodeId,
  type SpaghettiGraphCheckResult,
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
import type {MaterializationService} from './materialization_service';
import {serialize} from './serialize';

export interface SelectedSpaghettiNodeContext {
  readonly nodeId: string;
  readonly type: string;
  readonly title: string;
  readonly config: unknown;
  readonly columns: readonly string[];
  readonly sql?: string;
  readonly materializedTable?: string;
  readonly error?: string;
}

export function getSelectedNodeContext(
  state: SpaghettiGraphState,
  sqlModules: SqlModules | undefined,
  matService?: MaterializationService,
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
  const serialized = serialize(index, activeNodeId, state.dialect, {
    sqlModules,
  });
  const sql = serialized.ok ? formatSerializedSql(serialized) : undefined;
  const isCurrentPlan = matService?.plan?.outputId === activeNodeId;
  const matTable = isCurrentPlan ? matService?.outputTable : undefined;
  const warmTable =
    matTable !== undefined && matTable.startsWith('_qb_')
      ? matTable
      : undefined;
  const error = !serialized.ok
    ? serialized.error
    : isCurrentPlan
      ? matService?.error
      : undefined;

  return {
    nodeId: node.id,
    type: node.type,
    title,
    config: node.config,
    columns,
    ...(sql !== undefined ? {sql} : {}),
    ...(warmTable !== undefined ? {materializedTable: warmTable} : {}),
    ...(error !== undefined ? {error} : {}),
  };
}

export function getActiveSpaghettiGraphJson(
  state: SpaghettiGraphState,
): string | undefined {
  if (state.store.nodes.length === 0) return undefined;
  return JSON.stringify(JSON.parse(serializeStore(state.store)), null, 2);
}

function formatCheckResult(
  result: SpaghettiGraphCheckResult,
  okHeader: string,
  errorHeader: string,
): string {
  const parts: string[] = [];
  if (result.errors.length === 0) {
    parts.push(okHeader);
  } else {
    parts.push(`${errorHeader}\n${formatSpaghettiGraphErrors(result.errors)}`);
  }
  if (result.sql !== undefined) {
    const label =
      result.targetNodeId !== undefined
        ? `Generated SQL (node ${result.targetNodeId}):`
        : 'Generated SQL:';
    parts.push(`${label}\n\`\`\`sql\n${result.sql}\n\`\`\``);
  }
  return parts.join('\n\n');
}

export async function dryRunSpaghettiGraph(
  trace: Trace,
  sqlModulesPlugin: SqlModulesPlugin,
  state: SpaghettiGraphState,
  json: string,
  nodeId?: string,
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
  const check = await checkSpaghettiGraph(
    trace.engine,
    validated.store,
    sqlModulesPlugin.getSqlModules(),
    state.dialect,
    nodeId,
  );
  return formatCheckResult(
    check,
    'OK: graph is valid and runs cleanly.',
    'Graph is structurally valid, but some nodes fail to run:',
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
  matService?: MaterializationService,
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
      'structurally valid, the graph is applied, the generated SQL is ' +
      'returned, and per-node runtime SQL errors (if any) are reported so you ' +
      'can fix them and call set_spaghetti_graph again until it runs ' +
      'cleanly.\n\n' +
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
      const sqlModules = sqlModulesPlugin.getSqlModules();
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

      if (matService !== undefined) {
        await matService.scheduleUpdate(
          state.store,
          state.getActiveNodeId(),
          sqlModules,
          state.dialect,
        );
      }

      const check = await checkSpaghettiGraph(
        trace.engine,
        state.store,
        sqlModules,
        state.dialect,
        targetNodeId,
      );
      return formatCheckResult(
        check,
        `Applied Spaghetti graph cleanly (selected node: ${targetNodeId ?? 'none'}).`,
        'Applied Spaghetti graph, but some nodes have errors. Fix these and call set_spaghetti_graph again:',
      );
    },
  });

  intelletto.registerTool({
    name: 'check_spaghetti_graph',
    description:
      'Compile and test-run the current Spaghetti query builder graph ' +
      'against the trace, returning the generated SQL and any per-node ' +
      'errors (incomplete config, missing input wires, bad SQL expressions, ' +
      'missing columns/tables) without modifying the graph. Pass `nodeId` to ' +
      'inspect the generated SQL and validate a specific node; omit it to ' +
      'validate the entire graph.',
    shape: {
      nodeId: z
        .string()
        .optional()
        .describe(
          'Optional node id to inspect and validate. Omit to check all nodes ' +
            'and show the active/sink node SQL.',
        ),
    },
    callback: async ({nodeId}) => {
      if (state.store.nodes.length === 0) {
        return 'Spaghetti graph is empty.';
      }
      await sqlModulesPlugin.ensureInitialized();
      const targetId = nodeId ?? state.getActiveNodeId();
      const check = await checkSpaghettiGraph(
        trace.engine,
        state.store,
        sqlModulesPlugin.getSqlModules(),
        state.dialect,
        nodeId,
      );
      // If checking the whole graph (no explicit nodeId), prefer showing SQL
      // for the active node if one is selected.
      if (
        nodeId === undefined &&
        targetId !== undefined &&
        targetId !== check.targetNodeId
      ) {
        const index = flattenNodes(state.store.nodes);
        const serialized = serialize(index, targetId, state.dialect, {
          sqlModules: sqlModulesPlugin.getSqlModules(),
        });
        if (serialized.ok) {
          return formatCheckResult(
            {
              errors: check.errors,
              targetNodeId: targetId,
              sql: formatSerializedSql(serialized),
            },
            'OK: graph runs cleanly.',
            'Graph has errors:',
          );
        }
      }
      return formatCheckResult(
        check,
        'OK: graph runs cleanly.',
        'Graph has errors:',
      );
    },
  });

  intelletto.registerTool({
    name: 'validate_spaghetti_graph',
    description:
      'Dry-run a candidate Spaghetti graph JSON WITHOUT applying it - the ' +
      'active Spaghetti graph and UI are left untouched. Checks structural ' +
      'validity first, compiles SQL, and test-executes against the trace ' +
      'engine. Returns the generated SQL and "OK" when there are no issues, ' +
      'or the exact structural/runtime errors. See set_spaghetti_graph for ' +
      'the JSON format.',
    shape: {
      graph: z
        .string()
        .describe(
          'Candidate Spaghetti graph JSON string, same format as ' +
            'set_spaghetti_graph.',
        ),
      nodeId: z
        .string()
        .optional()
        .describe(
          'Optional node id to validate and inspect SQL for. Omit to validate ' +
            'all nodes.',
        ),
    },
    callback: async ({graph, nodeId}) =>
      dryRunSpaghettiGraph(trace, sqlModulesPlugin, state, graph, nodeId),
  });

  intelletto.registerContextProvider({
    id: 'dev.perfetto.Spaghetti#selected_node',
    description:
      'The active (pinned or selected) node in the Spaghetti query builder. ' +
      '"nodeId", "type", and "config" match the get_spaghetti_graph / ' +
      'set_spaghetti_graph JSON format; "columns" lists its output column ' +
      'names; "sql" is its compiled query; "materializedTable" (when present) ' +
      'is the warm cached _qb_* table in TraceProcessor that can be queried ' +
      'directly with run_query; "error" (when present) is the latest ' +
      'compilation or SQL runtime error.',
    getContext: () => {
      const ctx = getSelectedNodeContext(
        state,
        sqlModulesPlugin.getSqlModules(),
        matService,
      );
      if (ctx === undefined) return undefined;
      return {
        summary: `Spaghetti node: ${ctx.title}`,
        data: ctx,
      };
    },
  });
}
