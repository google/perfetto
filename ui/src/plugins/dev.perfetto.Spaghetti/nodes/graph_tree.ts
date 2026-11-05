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
import type {GraphTreeKind, LogicalOp} from '../logical_ir';
import type {
  ColumnContext,
  LogicalOpContext,
  NodeManifest,
  RenderContext,
} from '../node_types';
import {ColumnPicker} from '../widgets/column_picker';

const GraphTreeConfigSchema = z.object({
  kind: z.enum(['BFS', 'DFS', 'DOMINATOR']),
  nodeIdColumn: z.string(),
  sourceColumn: z.string(),
  destColumn: z.string(),
  rootIdColumn: z.string(),
});

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

      return m(Stack, {style: {minWidth: '220px'}}, [
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
          m('span', {style: {opacity: 0.5}}, '→'),
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

function toLogicalOp(
  config: GraphTreeConfig,
  ctx: LogicalOpContext,
): LogicalOp | undefined {
  const nodes = ctx.getInputNodeId('nodes');
  const edges = ctx.getInputNodeId('edges');
  const roots = ctx.getInputNodeId('roots');
  if (!nodes || !edges || !roots || !isValid(config)) return undefined;
  return {
    kind: 'graph_tree',
    treeKind: config.kind,
    nodes,
    edges,
    roots,
    nodeIdColumn: config.nodeIdColumn,
    sourceColumn: config.sourceColumn,
    destColumn: config.destColumn,
    rootIdColumn: config.rootIdColumn,
    nodeColumns: ctx.getInputColumns('nodes')?.map((c) => c.name),
  };
}

export const manifest: NodeManifest<GraphTreeConfig> = {
  title: 'Graph Tree',
  icon: 'account_tree',
  getInputs: () => [
    {name: 'nodes', content: 'Nodes'},
    {name: 'edges', content: 'Edges'},
    {name: 'roots', content: 'Roots'},
  ],
  hue: 260,
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
  isValid,
  toLogicalOp,
};
