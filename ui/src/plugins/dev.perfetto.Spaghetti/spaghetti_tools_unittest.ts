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
import {ensureDefined} from '../../base/assert';
import type {Trace} from '../../public/trace';
import type {Engine} from '../../trace_processor/engine';
import type {QueryResult} from '../../trace_processor/query_result';
import type {
  ContextProviderRegistration,
  IntellettoToolRegistrar,
  ToolRegistration,
} from '../dev.perfetto.Intelletto';
import type SqlModulesPlugin from '../dev.perfetto.SqlModules';
import type {SqlModules} from '../dev.perfetto.SqlModules/sql_modules';
import {
  collectSpaghettiGraphErrors,
  pickDefaultTargetNodeId,
  validateSpaghettiGraphJson,
} from './graph_check';
import {
  buildNodeCatalogFromRegistry,
  SPAGHETTI_GRAPH_FORMAT,
} from './graph_format';
import {SpaghettiGraphState} from './graph_state';
import {NODE_REGISTRY} from './graph_utils';
import type {NodeManifest} from './node_types';
import {registerSpaghettiTools} from './spaghetti_tools';

function makeFakeEngine(failPredicate?: (sql: string) => string | undefined) {
  const queries: string[] = [];
  const engine = {
    async query(sql: string): Promise<QueryResult> {
      queries.push(sql);
      const err = failPredicate?.(sql);
      if (err !== undefined) {
        throw new Error(err);
      }
      return {} as QueryResult;
    },
  } as unknown as Engine;
  return {engine, queries};
}

function makeFakeSqlModulesPlugin(sqlModules?: SqlModules): SqlModulesPlugin {
  return {
    ensureInitialized: async () => {},
    getSqlModules: () => sqlModules,
  } as unknown as SqlModulesPlugin;
}

function makeFakeRegistrar() {
  const tools = new Map<
    string,
    {
      readonly mutating?: boolean;
      readonly invoke: (rawArgs: Record<string, unknown>) => Promise<string>;
    }
  >();
  const providers = new Map<string, ContextProviderRegistration>();

  const registrar: IntellettoToolRegistrar = {
    registerTool<S extends z.ZodRawShape>(tool: ToolRegistration<S>): void {
      const schema = z.object(tool.shape);
      tools.set(tool.name, {
        mutating: tool.mutating,
        invoke: async (rawArgs) => tool.callback(schema.parse(rawArgs)),
      });
    },
    registerContextProvider(provider: ContextProviderRegistration): void {
      providers.set(provider.id, provider);
    },
  };

  return {registrar, tools, providers};
}

describe('SPAGHETTI_GRAPH_FORMAT', () => {
  test('derives every registered node schema and ports from NODE_REGISTRY', () => {
    for (const type of Object.keys(NODE_REGISTRY)) {
      expect(SPAGHETTI_GRAPH_FORMAT).toContain(`- "${type}" (`);
    }
    expect(SPAGHETTI_GRAPH_FORMAT).toContain(
      'ports: [0: input, 1: stop (optional)]',
    );
    expect(SPAGHETTI_GRAPH_FORMAT).toContain(
      'func: "COUNT" | "SUM" | "AVG" | "MIN" | "MAX";',
    );
  });

  test('automatically renders custom Zod schemas, enums, optional fields, and descriptions', () => {
    const CustomSchema = z
      .object({
        mode: z.enum(['FAST', 'ACCURATE']),
        threshold: z.number().optional().describe('Cutoff score'),
      })
      .describe('Synthetic test operator.');

    const fakeManifest: NodeManifest<z.infer<typeof CustomSchema>> = {
      title: 'Custom Op',
      icon: 'tune',
      hue: 120,
      schema: CustomSchema,
      getInputs: () => [
        {name: 'primary', content: 'Primary'},
        {name: 'aux', content: 'Aux', optional: true},
      ],
      defaultConfig: () => ({mode: 'FAST'}),
      render: () => null,
      isValid: () => true,
      toLogicalOp: () => undefined,
    };

    const catalog = buildNodeCatalogFromRegistry({custom_op: fakeManifest});
    expect(catalog).toContain(
      '- "custom_op" (Custom Op) — Synthetic test operator.',
    );
    expect(catalog).toContain('ports: [0: primary, 1: aux (optional)]');
    expect(catalog).toContain('mode: "FAST" | "ACCURATE";');
    expect(catalog).toContain('threshold?: number; // Cutoff score');
  });
});

describe('validateSpaghettiGraphJson', () => {
  test('accepts docked chain without coordinates and flags auto-layout', () => {
    const json = JSON.stringify({
      nodes: [
        {
          id: 'n1',
          type: 'from',
          config: {table: 'slice'},
          next: {
            id: 'n2',
            type: 'filter',
            config: {
              conditions: [{column: 'dur', op: '>', value: '1000'}],
              conjunction: 'AND',
            },
            next: {
              id: 'n3',
              type: 'limit',
              config: {limitCount: '25', offset: ''},
            },
          },
        },
      ],
    });

    const res = validateSpaghettiGraphJson(json);
    expect(res.errors).toEqual([]);
    expect(res.needsAutoLayout).toBe(true);
    const store = ensureDefined(res.store);
    expect(store.nodes).toHaveLength(1);
    expect(store.nodes[0]?.x).toBe(80);
    expect(store.nodes[0]?.y).toBe(80);
    expect(pickDefaultTargetNodeId(store)).toBe('n3');
  });

  test('preserves explicit distinct root coordinates without auto-layout', () => {
    const json = JSON.stringify({
      nodes: [
        {id: 'a', type: 'from', x: 120, y: 60, config: {table: 'slice'}},
        {id: 'b', type: 'from', x: 500, y: 200, config: {table: 'thread'}},
      ],
      labels: [],
    });

    const res = validateSpaghettiGraphJson(json);
    expect(res.errors).toEqual([]);
    expect(res.needsAutoLayout).toBe(false);
    expect(res.store?.nodes[0]?.x).toBe(120);
    expect(res.store?.nodes[1]?.x).toBe(500);
  });

  test('unstacks multiple roots placed at identical (0, 0) coordinates', () => {
    const json = JSON.stringify({
      nodes: [
        {id: 'a', type: 'from', x: 0, y: 0, config: {table: 'slice'}},
        {id: 'b', type: 'from', x: 0, y: 0, config: {table: 'thread'}},
      ],
    });

    const res = validateSpaghettiGraphJson(json);
    expect(res.errors).toEqual([]);
    expect(res.needsAutoLayout).toBe(true);
    expect(res.store?.nodes[0]?.x).not.toBe(res.store?.nodes[1]?.x);
  });

  test('normalizes redundant inputs[0] matching docked parent', () => {
    const json = JSON.stringify({
      nodes: [
        {
          id: 't1',
          type: 'from',
          config: {table: 'thread'},
        },
        {
          id: 's1',
          type: 'from',
          config: {table: 'sched'},
          next: {
            id: 'j1',
            type: 'join',
            inputs: ['s1', 't1'],
            config: {
              joinType: 'LEFT',
              leftColumn: 'utid',
              rightColumn: 'utid',
              columns: [{column: 'name', alias: 'thread_name'}],
            },
          },
        },
      ],
    });

    const res = validateSpaghettiGraphJson(json);
    expect(res.errors).toEqual([]);
    const joinNode = res.store?.nodes[1]?.next;
    expect(joinNode?.inputs).toEqual([null, 't1']);
  });

  test('rejects unknown node types, duplicate ids, docked sources, dangling inputs, and cycles', () => {
    const badTypesAndDup = validateSpaghettiGraphJson(
      JSON.stringify({
        nodes: [
          {id: 'dup', type: 'not_a_node', config: {}},
          {
            id: 'dup',
            type: 'from',
            config: {table: 'slice'},
            next: {id: 'docked_src', type: 'from', config: {table: 'thread'}},
          },
        ],
      }),
    );
    expect(badTypesAndDup.store).toBeUndefined();
    expect(badTypesAndDup.errors.some((e) => e.includes('not_a_node'))).toBe(
      true,
    );
    expect(badTypesAndDup.errors.some((e) => e.includes('duplicate'))).toBe(
      true,
    );
    expect(badTypesAndDup.errors.some((e) => e.includes('0 input ports'))).toBe(
      true,
    );

    const dangling = validateSpaghettiGraphJson(
      JSON.stringify({
        nodes: [
          {
            id: 'f1',
            type: 'filter',
            inputs: ['missing_id'],
            config: {conditions: []},
          },
        ],
      }),
    );
    expect(dangling.errors.some((e) => e.includes('missing_id'))).toBe(true);

    const cycle = validateSpaghettiGraphJson(
      JSON.stringify({
        nodes: [
          {
            id: 'a',
            type: 'distinct',
            inputs: ['b'],
            config: {},
            next: {
              id: 'b',
              type: 'distinct',
              config: {},
            },
          },
        ],
      }),
    );
    expect(cycle.errors.some((e) => e.includes('Cycle detected'))).toBe(true);
  });
});

describe('collectSpaghettiGraphErrors', () => {
  test('reports earliest failing node in a chain without cascading duplicates', async () => {
    const validated = validateSpaghettiGraphJson(
      JSON.stringify({
        nodes: [
          {
            id: 'n1',
            type: 'from',
            config: {table: 'slice'},
            next: {
              id: 'n2',
              type: 'filter',
              config: {
                conditions: [{column: 'nonexistent_col', op: '>', value: '0'}],
              },
              next: {
                id: 'n3',
                type: 'limit',
                config: {limitCount: '10', offset: ''},
              },
            },
          },
        ],
      }),
    );
    const store = ensureDefined(validated.store);

    const {engine} = makeFakeEngine((sql) =>
      sql.includes('nonexistent_col')
        ? 'no such column: nonexistent_col'
        : undefined,
    );

    const errors = await collectSpaghettiGraphErrors(engine, store, undefined);
    expect(errors).toEqual([
      {
        nodeId: 'n2',
        title: 'Filter',
        error: 'no such column: nonexistent_col',
      },
    ]);
  });

  test('catches incomplete node configs and missing required ports while allowing optional stop port', async () => {
    const validated = validateSpaghettiGraphJson(
      JSON.stringify({
        nodes: [
          {
            id: 'src',
            type: 'from',
            config: {table: 'counter'},
            next: {
              id: 'ife',
              type: 'intervals_from_events',
              config: {
                partitionColumns: ['track_id'],
                lastMode: 'CLOSE_AT',
                closeAtExpr: 'trace_end()',
              },
            },
          },
          {
            id: 'j_incomplete',
            type: 'join',
            inputs: ['src'],
            config: {
              joinType: 'LEFT',
              leftColumn: 'id',
              rightColumn: 'id',
              columns: [],
            },
          },
          {
            id: 'gb_invalid',
            type: 'groupby',
            inputs: ['src'],
            config: {
              groupColumns: [],
              aggregations: [{func: 'COUNT', column: '*', alias: 'cnt'}],
            },
          },
        ],
      }),
    );
    const store = ensureDefined(validated.store);
    const {engine} = makeFakeEngine();

    const errors = await collectSpaghettiGraphErrors(engine, store, undefined);
    expect(errors.map((e) => e.nodeId)).toEqual(['j_incomplete', 'gb_invalid']);
    expect(errors[0]?.error).toContain('port 1 ("right")');
    expect(errors[1]?.error).toContain('groupColumns');
  });

  test('validates chart specs and cleans up temporary tables in pipe dialect', async () => {
    const validated = validateSpaghettiGraphJson(
      JSON.stringify({
        nodes: [
          {
            id: 'src',
            type: 'from',
            config: {table: 'slice'},
            next: {
              id: 'gb',
              type: 'groupby',
              config: {
                groupColumns: ['name'],
                aggregations: [{func: 'COUNT', column: '*', alias: 'cnt'}],
              },
              next: {
                id: 'ch',
                type: 'chart',
                config: {
                  numInputs: 1,
                  charts: [{xCol: 'missing_chart_col', yCol: ''}],
                },
              },
            },
          },
        ],
      }),
    );
    const store = ensureDefined(validated.store);
    const {engine, queries} = makeFakeEngine((sql) =>
      sql.includes('missing_chart_col')
        ? 'no such column: missing_chart_col'
        : undefined,
    );

    const errors = await collectSpaghettiGraphErrors(
      engine,
      store,
      undefined,
      'pipe',
    );
    expect(errors).toHaveLength(1);
    expect(errors[0]?.nodeId).toBe('ch');
    expect(queries.some((q) => q.startsWith('DROP TABLE IF EXISTS _qb_chk_'))).toBe(
      true,
    );
  });
});

describe('registerSpaghettiTools', () => {
  test('registers tools & context provider and syncs state, undo history, and navigation', async () => {
    const {engine} = makeFakeEngine();
    const navigatedRoutes: string[] = [];
    const trace = {
      engine,
      navigate: (route: string) => {
        navigatedRoutes.push(route);
      },
    } as unknown as Trace;

    const fakeSqlModules = {
      getTable: (name: string) =>
        name === 'slice'
          ? {
              name: 'slice',
              columns: [
                {name: 'id', type: {kind: 'int'}},
                {name: 'name', type: {kind: 'string'}},
                {name: 'dur', type: {kind: 'duration'}},
              ],
            }
          : undefined,
      getModuleForTable: () => undefined,
    } as unknown as SqlModules;

    const state = new SpaghettiGraphState({persist: false});
    const {registrar, tools, providers} = makeFakeRegistrar();
    registerSpaghettiTools(
      registrar,
      trace,
      makeFakeSqlModulesPlugin(fakeSqlModules),
      state,
    );

    expect([...tools.keys()]).toEqual([
      'get_spaghetti_graph',
      'set_spaghetti_graph',
      'check_spaghetti_graph',
      'validate_spaghetti_graph',
    ]);

    const getTool = ensureDefined(tools.get('get_spaghetti_graph'));
    const setTool = ensureDefined(tools.get('set_spaghetti_graph'));
    const checkTool = ensureDefined(tools.get('check_spaghetti_graph'));
    const validateTool = ensureDefined(tools.get('validate_spaghetti_graph'));
    const ctxProvider = ensureDefined(
      providers.get('dev.perfetto.Spaghetti#selected_node'),
    );

    expect(await getTool.invoke({})).toBe('<empty>');
    expect(ctxProvider.getContext()).toBeUndefined();

    // Structural failure throws before mutating state or navigating.
    await expect(
      setTool.invoke({
        graph: JSON.stringify({
          nodes: [{id: 'bad', type: 'unknown_type', config: {}}],
        }),
      }),
    ).rejects.toThrow(/Invalid Spaghetti graph/);
    expect(state.store.nodes).toHaveLength(0);
    expect(navigatedRoutes).toEqual([]);

    const candidateJson = JSON.stringify({
      nodes: [
        {
          id: 's1',
          type: 'from',
          config: {table: 'slice'},
          next: {
            id: 'l1',
            type: 'limit',
            config: {limitCount: '10', offset: ''},
          },
        },
      ],
    });

    // Dry-run validate leaves state untouched.
    expect(await validateTool.invoke({graph: candidateJson})).toBe(
      'OK: graph is valid and runs cleanly.',
    );
    expect(state.store.nodes).toHaveLength(0);

    // Apply updates state, selects sink node, navigates to #!/spaghetti, and pushes undo history.
    expect(await setTool.invoke({graph: candidateJson})).toBe(
      'OK: Spaghetti graph applied and runs cleanly.',
    );
    expect(navigatedRoutes).toEqual(['#!/spaghetti']);
    expect(state.getActiveNodeId()).toBe('l1');
    expect(await checkTool.invoke({})).toBe('OK: graph runs cleanly.');

    const selectedCtx = ensureDefined(ctxProvider.getContext());
    expect(selectedCtx.summary).toBe('Spaghetti node: Limit');
    expect(selectedCtx.data).toEqual({
      nodeId: 'l1',
      type: 'limit',
      title: 'Limit',
      config: {limitCount: '10', offset: ''},
      columns: ['id', 'name', 'dur'],
    });

    // Undo restores previous empty state; redo restores applied graph.
    expect(state.canUndo()).toBe(true);
    state.undo();
    expect(state.store.nodes).toHaveLength(0);
    state.redo();
    expect(state.store.nodes).toHaveLength(1);
  });
});
