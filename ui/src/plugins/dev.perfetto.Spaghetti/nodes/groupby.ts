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

import m from 'mithril';
import type {
  ColumnContext,
  NodeManifest,
  RenderContext,
  SqlStatement,
} from '../node_types';
import {Select} from '../../../widgets/select';
import {moveItem, Row} from '../components/row';
import {AddButton} from '../components/add_button';
import {Stack} from '../components/stack';
import {ColumnPicker} from '../widgets/column_picker';
import type {ColumnDef} from '../graph_utils';
import {AliasTag} from '../components/alias_tag';

export interface Aggregation {
  readonly func: 'COUNT' | 'SUM' | 'AVG' | 'MIN' | 'MAX';
  readonly column: string;
  readonly alias: string;
}

export interface GroupByConfig {
  readonly groupColumns: string[];
  readonly aggregations: Aggregation[];
}

function aggAlias(a: Aggregation): string {
  if (a.alias) return a.alias;
  const col = a.column === '*' ? 'star' : a.column;
  return `${a.func.toLowerCase()}_${col}`;
}

function GroupByContent(): m.Component<{
  config: GroupByConfig;
  updateConfig: (updates: Partial<GroupByConfig>) => void;
  ctx: RenderContext;
}> {
  return {
    view({attrs: {config, updateConfig, ctx}}) {
      return m(Stack, {className: 'pf-spag-node-wide'}, [
        m('.pf-spag-section-label', 'Group by'),
        m(Stack, {compact: true}, [
          ...config.groupColumns.map((col, i) =>
            m(
              Row,
              {
                key: i,
                reorder: {
                  index: i,
                  onMove: (from: number, to: number) => {
                    updateConfig({
                      groupColumns: moveItem(config.groupColumns, from, to),
                    });
                  },
                },
              },
              [
                m(ColumnPicker, {
                  value: col,
                  columns: ctx.availableColumns,
                  placeholder: 'column',
                  onSelect: (value: string) => {
                    const updated = [...config.groupColumns];
                    updated[i] = value;
                    updateConfig({groupColumns: updated});
                  },
                }),
                m(Row.DeleteButton, {
                  onclick: () => {
                    updateConfig({
                      groupColumns: config.groupColumns.filter(
                        (_, j) => j !== i,
                      ),
                    });
                  },
                }),
              ],
            ),
          ),
        ]),
        m(AddButton, {
          label: 'Add grouping',
          onclick: () => {
            updateConfig({groupColumns: [...config.groupColumns, '']});
          },
        }),

        m('.pf-spag-section-label', 'Aggregations'),
        m(Stack, {compact: true}, [
          ...config.aggregations.map((agg, i) =>
            m(
              Row,
              {
                key: i,
                reorder: {
                  index: i,
                  onMove: (from: number, to: number) => {
                    updateConfig({
                      aggregations: moveItem(config.aggregations, from, to),
                    });
                  },
                },
              },
              [
                m(
                  Select,
                  {
                    value: agg.func,
                    onchange: (e: Event) => {
                      const newAggs = [...config.aggregations];
                      newAggs[i] = {
                        ...agg,
                        func: (e.target as HTMLSelectElement)
                          .value as Aggregation['func'],
                      };
                      updateConfig({aggregations: newAggs});
                    },
                  },
                  [
                    m('option', {value: 'COUNT'}, 'COUNT'),
                    m('option', {value: 'SUM'}, 'SUM'),
                    m('option', {value: 'AVG'}, 'AVG'),
                    m('option', {value: 'MIN'}, 'MIN'),
                    m('option', {value: 'MAX'}, 'MAX'),
                  ],
                ),
                m(ColumnPicker, {
                  value: agg.column,
                  columns: [{name: '*'}, ...ctx.availableColumns],
                  placeholder: 'column',
                  onSelect: (value: string) => {
                    const newAggs = [...config.aggregations];
                    newAggs[i] = {...agg, column: value};
                    updateConfig({aggregations: newAggs});
                  },
                }),
                m(AliasTag, {
                  alias: agg.alias,
                  placeholder: aggAlias(agg),
                  onChange: (value: string) => {
                    const newAggs = [...config.aggregations];
                    newAggs[i] = {...agg, alias: value};
                    updateConfig({aggregations: newAggs});
                  },
                }),
                m(Row.DeleteButton, {
                  onclick: () => {
                    updateConfig({
                      aggregations: config.aggregations.filter(
                        (_, j) => j !== i,
                      ),
                    });
                  },
                }),
              ],
            ),
          ),
        ]),
        m(AddButton, {
          label: 'Add aggregation',
          onclick: () => {
            updateConfig({
              aggregations: [
                ...config.aggregations,
                {func: 'COUNT', column: '*', alias: ''},
              ],
            });
          },
        }),
      ]);
    },
  };
}

function render(
  config: GroupByConfig,
  updateConfig: (updates: Partial<GroupByConfig>) => void,
  ctx: RenderContext,
): m.Children {
  return m(GroupByContent, {config, updateConfig, ctx});
}

function getOutputColumns(
  config: GroupByConfig,
  ctx: ColumnContext,
): ColumnDef[] | undefined {
  const columns = ctx.getInputColumns('input');
  const groupCols: ColumnDef[] = config.groupColumns
    .filter((c) => c)
    .map((c) => columns?.find((col) => col.name === c) ?? {name: c});
  const aggAliases: ColumnDef[] = config.aggregations
    .filter((a) => a.column)
    .map((a) => {
      const name = aggAlias(a);
      if (a.func === 'COUNT') {
        return {name, type: {kind: 'int' as const}};
      }
      const orig = columns?.find((c) => c.name === a.column);
      return {name, type: orig?.type};
    });
  const result = [...groupCols, ...aggAliases];
  return result.length > 0 ? result : columns;
}

function isValid(config: GroupByConfig): boolean {
  const hasGroup = config.groupColumns.some((c) => c);
  return hasGroup;
}

function tryFold(stmt: SqlStatement, config: GroupByConfig): boolean {
  if (
    stmt.columns !== '*' ||
    stmt.groupBy !== undefined ||
    stmt.orderBy !== undefined ||
    stmt.limit !== undefined
  ) {
    return false;
  }
  const groupCols = config.groupColumns.filter((c) => c);
  const aggExprs = config.aggregations
    .filter((a) => a.column)
    .map((a) => `${a.func}(${a.column}) AS ${aggAlias(a)}`);
  const selectParts = [...groupCols, ...aggExprs];
  if (selectParts.length > 0) stmt.columns = selectParts.join(', ');
  if (groupCols.length > 0) stmt.groupBy = groupCols.join(', ');
  return true;
}

export const manifest: NodeManifest<GroupByConfig> = {
  title: 'Group By',
  icon: 'workspaces',
  getInputs: () => [{name: 'input', content: 'Input'}],
  hue: 275,
  defaultConfig: () => ({groupColumns: [], aggregations: []}),
  render,
  getOutputColumns,
  isValid,
  tryFold,
};
