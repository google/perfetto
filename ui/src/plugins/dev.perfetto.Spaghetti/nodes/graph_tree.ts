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
import {RadioGroup} from '../../../widgets/radio_group';
import {Row} from '../components/row';
import {Stack} from '../components/stack';
import type {ColumnDef} from '../graph_utils';
import type {GraphTreeKind} from '../sql_types';
import {indentTail, subquery} from '../generator_utils';
import type {
  ColumnContext,
  NodeGenerators,
  NodeManifest,
  RenderContext,
} from '../node_types';
import {ColumnPicker} from '../widgets/column_picker';

const GraphTreeConfigSchema = z
  .object({
    kind: z.enum(['BFS', 'DFS', 'DOMINATOR']),
    nodeIdColumn: z.string(),
    sourceColumn: z.string(),
    destColumn: z.string(),
    rootIdColumn: z.string(),
  })
  .describe(
    'Compute BFS, DFS, or DOMINATOR spanning tree from nodes, edges, and roots.',
  );

export type GraphTreeConfig = z.infer<typeof GraphTreeConfigSchema>;

function GraphTreeContent(): m.Component<{
  config: GraphTreeConfig;
  updateConfig: (updates: Partial<GraphTreeConfig>) => void;
  ctx: RenderContext;
}> {
  return {
    view({attrs: {config, updateConfig, ctx}}) {
      const nodeCols = ctx.getInputColumns('nodes');
      const edgeCols = ctx.getInputColumns('edges');
      const rootCols = ctx.getInputColumns('roots');

      return m(Stack, {minWidth: 'md'}, [
        m(
          RadioGroup,
          {
            fillWidth: true,
            selectedValue: config.kind,
            onValueChange: (value: string) =>
              updateConfig({kind: value as GraphTreeKind}),
          },
          [
            m(RadioGroup.Button, {value: 'BFS'}, 'BFS'),
            m(RadioGroup.Button, {value: 'DFS'}, 'DFS'),
            m(RadioGroup.Button, {value: 'DOMINATOR'}, 'Dom'),
          ],
        ),
        m('.pf-spag-section-label', 'Node ID'),
        m(ColumnPicker, {
          value: config.nodeIdColumn,
          columns: nodeCols,
          placeholder: 'id',
          onSelect: (value: string) => updateConfig({nodeIdColumn: value}),
        }),
        m('.pf-spag-section-label', 'Edges (src → dst)'),
        m(Row, [
          m(ColumnPicker, {
            value: config.sourceColumn,
            columns: edgeCols,
            placeholder: 'src',
            onSelect: (value: string) => updateConfig({sourceColumn: value}),
          }),
          m('span.pf-spag-operator', '→'),
          m(ColumnPicker, {
            value: config.destColumn,
            columns: edgeCols,
            placeholder: 'dst',
            onSelect: (value: string) => updateConfig({destColumn: value}),
          }),
        ]),
        m('.pf-spag-section-label', 'Root ID'),
        m(ColumnPicker, {
          value: config.rootIdColumn,
          columns: rootCols,
          placeholder: 'id',
          onSelect: (value: string) => updateConfig({rootIdColumn: value}),
        }),
      ]);
    },
  };
}

function getOutputColumns(
  config: GraphTreeConfig,
  ctx: ColumnContext,
): ColumnDef[] | undefined {
  const nodeCols = ctx.getInputColumns('nodes');
  if (nodeCols === undefined) return undefined;
  const carried = nodeCols
    .filter((c) => c.name !== 'parent_id')
    .map((c) =>
      c.name === config.nodeIdColumn && config.nodeIdColumn !== 'id'
        ? {...c, name: 'id'}
        : c,
    );
  return [...carried, {name: 'parent_id', type: {kind: 'int' as const}}];
}

function isValid(config: GraphTreeConfig): boolean {
  return (
    config.nodeIdColumn !== '' &&
    config.sourceColumn !== '' &&
    config.destColumn !== '' &&
    config.rootIdColumn !== ''
  );
}

function carriedNodeColumns(
  config: GraphTreeConfig,
  nodeColumns: readonly string[] | undefined,
): string[] {
  if (nodeColumns === undefined || nodeColumns.length === 0) return ['n.*'];
  return nodeColumns
    .filter((c) => c !== 'parent_id')
    .map((c) =>
      c === config.nodeIdColumn && config.nodeIdColumn !== 'id'
        ? `n.${c} AS id`
        : `n.${c}`,
    );
}

const generators: NodeGenerators<GraphTreeConfig> = {
  sql: (config, ctx) => {
    const nodes = ctx.input('nodes');
    const edges = ctx.input('edges');
    const roots = ctx.input('roots');
    if (!nodes || !edges || !roots || !isValid(config)) return undefined;

    const carried = carriedNodeColumns(
      config,
      ctx.getInputColumns('nodes')?.map((c) => c.name),
    );
    const edgesSql = `SELECT ${config.sourceColumn} AS source_node_id, ${config.destColumn} AS dest_node_id FROM ${edges.ref} WHERE ${config.sourceColumn} IS NOT NULL AND ${config.destColumn} IS NOT NULL`;
    const rootsSql = `SELECT ${config.rootIdColumn} AS node_id FROM ${roots.ref} WHERE ${config.rootIdColumn} IS NOT NULL`;

    // BFS / DFS: join the reachability macro straight onto the nodes.
    if (config.kind === 'BFS' || config.kind === 'DFS') {
      const macro =
        config.kind === 'BFS' ? 'graph_reachable_bfs!' : 'graph_reachable_dfs!';
      return {
        includes: ['graphs.search'],
        from:
          `${nodes.ref} AS n\n` +
          `JOIN ${macro}(\n  (${edgesSql}),\n  (${rootsSql})\n) AS _tree ON n.${config.nodeIdColumn} = _tree.node_id`,
        select: [...carried, '_tree.parent_node_id AS parent_id'],
      };
    }

    // DOMINATOR: add a synthetic super-root feeding every root, then compute
    // the dominator tree over the reachable subgraph.
    return {
      includes: ['graphs.search', 'graphs.dominator_tree'],
      from: subquery(
        [
          'WITH',
          `  _edges AS (\n    ${edgesSql}\n  ),`,
          `  _roots AS (\n    ${rootsSql}\n  ),`,
          '  _reach AS (\n    SELECT node_id FROM graph_reachable_dfs!(_edges, _roots)\n  ),',
          `  _super AS (\n    SELECT COALESCE(MAX(${config.nodeIdColumn}), 0) + 1 AS root_id FROM ${nodes.ref}\n  ),`,
          '  _flow AS (\n    SELECT e.source_node_id, e.dest_node_id\n    FROM _edges AS e\n    JOIN _reach AS r ON e.source_node_id = r.node_id\n    UNION ALL\n    SELECT (SELECT root_id FROM _super) AS source_node_id, node_id AS dest_node_id\n    FROM _roots\n  ),',
          '  _dom AS (\n    SELECT\n      node_id,\n      IIF(dominator_node_id = (SELECT root_id FROM _super), NULL, dominator_node_id) AS parent_id\n    FROM graph_dominator_tree!(_flow, (SELECT root_id FROM _super))\n    WHERE node_id != (SELECT root_id FROM _super)\n  )',
          `SELECT\n  ${[...carried, '_dom.parent_id'].join(',\n  ')}\nFROM ${nodes.ref} AS n\nJOIN _dom ON n.${config.nodeIdColumn} = _dom.node_id`,
        ].join('\n'),
      ),
    };
  },
  // GRAPH ... TREE expects NODES(id), EDGES(src, dst) and roots(id).
  psn: (config, ctx) => {
    const nodes = ctx.input('nodes');
    const edges = ctx.input('edges');
    const roots = ctx.input('roots');
    if (!nodes || !edges || !roots || !isValid(config)) return undefined;
    const nodesSrc =
      config.nodeIdColumn === 'id'
        ? nodes.source
        : nodes.withStages([`RENAME ${config.nodeIdColumn} AS id`]);
    const edgesSrc =
      config.sourceColumn === 'src' && config.destColumn === 'dst'
        ? edges.source
        : edges.withStages([
            `SELECT ${config.sourceColumn} AS src, ${config.destColumn} AS dst`,
          ]);
    const rootsSrc =
      config.rootIdColumn === 'id'
        ? roots.source
        : roots.withStages([`SELECT ${config.rootIdColumn} AS id`]);
    return {
      source: [
        `GRAPH ${config.kind} TREE`,
        `  NODES ${indentTail(nodesSrc, '  ')}`,
        `  EDGES ${indentTail(edgesSrc, '  ')}`,
        `  FROM ${indentTail(rootsSrc, '  ')}`,
      ].join('\n'),
    };
  },
};

export const manifest: NodeManifest<GraphTreeConfig> = {
  title: 'Graph Tree',
  icon: 'account_tree',
  getInputs: () => [
    {name: 'nodes', content: 'Nodes'},
    {name: 'edges', content: 'Edges'},
    {name: 'roots', content: 'Roots'},
  ],
  hue: 76,
  expensive: true,
  schema: GraphTreeConfigSchema,
  defaultConfig: () => ({
    kind: 'BFS',
    nodeIdColumn: 'id',
    sourceColumn: 'src',
    destColumn: 'dst',
    rootIdColumn: 'id',
  }),
  render: (config, updateConfig, ctx) =>
    m(GraphTreeContent, {config, updateConfig, ctx}),
  getOutputColumns,
  generators,
};
