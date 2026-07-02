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
import type {NodeManifest, RenderContext, IrContext} from '../node_types';
import {Button, ButtonVariant} from '../../../widgets/button';
import {Row} from '../components/row';
import {Stack} from '../components/stack';
import './join.scss';
import {ColumnPicker} from '../widgets/column_picker';
import {RadioGroup} from '../../../widgets/radio_group';
import { AliasTag } from '../components/alias_tag';

export interface JoinColumn {
  readonly column: string;
  readonly alias: string;
}

export interface JoinConfig {
  readonly leftColumn: string;
  readonly rightColumn: string;
  readonly columns?: JoinColumn[];
  readonly joinType: 'LEFT' | 'INNER';
}

// Compute the resolved aliases for all extend columns.
export function getExtendColumnAliases(
  config: JoinConfig,
  leftAvail: string[],
): JoinColumn[] {
  const leftSet = new Set(leftAvail);
  const columns = config.columns ?? [];
  return columns.map((c) => ({
    column: c.column,
    alias: c.alias || (leftSet.has(c.column) ? `right_${c.column}` : c.column),
  }));
}

function JoinNodeContent(): m.Component<{
  config: JoinConfig;
  updateConfig: (updates: Partial<JoinConfig>) => void;
  ctx: RenderContext;
}> {
  return {
    view({attrs: {config, updateConfig, ctx}}) {
      const leftColumns = ctx.getInputColumns('left');
      const rightColumns = ctx.getInputColumns('right');
      const columns = config.columns ?? [];
      const leftSet = new Set(leftColumns.map((c) => c.name));

      const joinType = config.joinType;
      return m(Stack, {style: {minWidth: '200px'}}, [
        m(
          RadioGroup,
          {
            fillWidth: true,
            selectedValue: joinType,
            onValueChange: (value: string) =>
              updateConfig({joinType: value as 'LEFT' | 'INNER'}),
          },
          [
            m(RadioGroup.Button, {value: 'LEFT'}, 'Left'),
            m(RadioGroup.Button, {value: 'INNER'}, 'Inner'),
          ],
        ),
        m('.pf-spag-section-label', 'Join on'),
        m('.pf-spag-group-grid', [
          m(ColumnPicker, {
            value: config.leftColumn,
            columns: leftColumns,
            placeholder: 'left col',
            className: 'pf-spag-join-col',
            onSelect: (value: string) => {
              updateConfig({leftColumn: value});
            },
          }),
          m('span', {style: {opacity: 0.5}}, '='),
          m(ColumnPicker, {
            value: config.rightColumn,
            columns: rightColumns,
            placeholder: 'right col',
            className: 'pf-spag-join-col',
            onSelect: (value: string) => {
              updateConfig({rightColumn: value});
            },
          }),
        ]),

        m('.pf-spag-section-label', 'Columns to add'),
        m(Stack, {compact: true}, [
          ...columns.map((entry, i) => {
            const defaultAlias = leftSet.has(entry.column)
              ? `right_${entry.column}`
              : entry.column;
            return m(
              Row,
              {
                key: i,
                draggable: true,
                ondragstart: (e: DragEvent) => {
                  e.dataTransfer!.effectAllowed = 'move';
                  e.dataTransfer!.setData('text/plain', String(i));
                  (e.currentTarget as HTMLElement).classList.add('pf-dragging');
                },
                ondragend: (e: DragEvent) => {
                  (e.currentTarget as HTMLElement).classList.remove(
                    'pf-dragging',
                  );
                },
                ondragover: (e: DragEvent) => {
                  e.preventDefault();
                  e.dataTransfer!.dropEffect = 'move';
                  const el = e.currentTarget as HTMLElement;
                  const rect = el.getBoundingClientRect();
                  const isBottom = e.clientY > rect.top + rect.height / 2;
                  el.classList.toggle('pf-drag-over-top', !isBottom);
                  el.classList.toggle('pf-drag-over-bottom', isBottom);
                },
                ondragleave: (e: DragEvent) => {
                  const el = e.currentTarget as HTMLElement;
                  el.classList.remove(
                    'pf-drag-over-top',
                    'pf-drag-over-bottom',
                  );
                },
                ondrop: (e: DragEvent) => {
                  e.preventDefault();
                  const el = e.currentTarget as HTMLElement;
                  const isBottom = el.classList.contains('pf-drag-over-bottom');
                  el.classList.remove(
                    'pf-drag-over-top',
                    'pf-drag-over-bottom',
                  );
                  const fromIdx = parseInt(
                    e.dataTransfer!.getData('text/plain'),
                  );
                  let toIdx = isBottom ? i + 1 : i;
                  if (fromIdx !== toIdx && fromIdx + 1 !== toIdx) {
                    const updated = [...columns];
                    const [moved] = updated.splice(fromIdx, 1);
                    if (fromIdx < toIdx) toIdx--;
                    updated.splice(toIdx, 0, moved);
                    updateConfig({columns: updated});
                  }
                },
              },
              [
                m(Row.DragHandle),
                m(ColumnPicker, {
                  value: entry.column,
                  columns: rightColumns,
                  placeholder: 'column',
                  onSelect: (value: string) => {
                    const updated = [...columns];
                    updated[i] = {...entry, column: value};
                    updateConfig({columns: updated});
                  },
                }),
                m(AliasTag, {
                  alias: entry.alias,
                  placeholder: defaultAlias,
                  onChange: (value: string) => {
                    const updated = [...columns];
                    updated[i] = {...entry, alias: value};
                    updateConfig({columns: updated});
                  },
                }),
                m(Row.DeleteButton, {
                  onclick: () => {
                    updateConfig({
                      columns: columns.filter((_, j) => j !== i),
                    });
                  },
                }),
              ],
            );
          }),
        ]),
        m(Button, {
          label: 'Column',
          icon: 'add',
          variant: ButtonVariant.Filled,
          onclick: () => {
            updateConfig({
              columns: [...columns, {column: '', alias: ''}],
            });
          },
        }),
      ]);
    },
  };
}

function render(
  config: JoinConfig,
  updateConfig: (updates: Partial<JoinConfig>) => void,
  ctx: RenderContext,
): m.Children {
  return m(JoinNodeContent, {config, updateConfig, ctx});
}

function isValid(config: JoinConfig): boolean {
  return config.leftColumn !== '' && config.rightColumn !== '';
}

export const manifest: NodeManifest<JoinConfig> = {
  title: 'Join',
  icon: 'join_left',
  resolveIcon: (config) =>
    config.joinType === 'LEFT' ? 'join_left' : 'join_inner',
  getInputs: () => [
    {name: 'left', content: 'Left'},
    {name: 'right', content: 'Right'},
  ],
  hue: 308,
  getOutputColumns(config, ctx) {
    // Output columns are all left columns, plus select right columns.
    const leftCols = ctx.getInputColumns('left') ?? [];
    const rightCols = ctx.getInputColumns('right') ?? [];
    const leftSet = new Set(leftCols.map((c) => c.name));
    const outputCols = [...leftCols];
    const columns = config.columns ?? [];

    for (const entry of columns) {
      const colName = entry.column;
      if (!colName) continue;
      const rightCol = rightCols.find((c) => c.name === colName);
      if (rightCol) {
        const alias =
          entry.alias || (leftSet.has(colName) ? `right_${colName}` : colName);
        outputCols.push({name: alias, type: rightCol.type});
      }
    }

    return outputCols;
  },
  defaultConfig: () => ({
    joinType: 'LEFT',
    leftColumn: '',
    rightColumn: '',
    columns: [],
  }),
  render,
  isValid,
  emitIr(config: JoinConfig, ctx: IrContext) {
    const leftRef = ctx.getInputRef('left');
    const rightRef = ctx.getInputRef('right');
    if (!leftRef) return undefined;
    if (!rightRef) return {sql: `SELECT *\nFROM ${leftRef}`};
    const leftCols = ctx.getInputColumns('left');

    const selectCols: string[] = ['l.*'];
    const aliases = getExtendColumnAliases(
      config,
      (leftCols ?? []).map((c) => c.name),
    );
    for (const a of aliases) {
      const expr = `r.${a.column}`;
      selectCols.push(a.alias !== a.column ? `${expr} AS ${a.alias}` : expr);
    }
    let selectClause: string;
    if (selectCols.length > 1) {
      selectClause = '\n  ' + selectCols.join(',\n  ');
    } else {
      selectClause = selectCols[0];
    }
    const joinKw =
      (config.joinType ?? 'LEFT') === 'INNER' ? 'JOIN' : 'LEFT JOIN';
    const condition = `ON l.${config.leftColumn} = r.${config.rightColumn}`;
    const sql = `SELECT ${selectClause}\nFROM ${leftRef} AS l\n${joinKw} ${rightRef} AS r ${condition}`;
    return {sql};
  },
};
