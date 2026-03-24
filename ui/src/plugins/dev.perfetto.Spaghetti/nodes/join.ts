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
import {NodeManifest, RenderContext, IrContext} from '../node_types';
import {Button, ButtonVariant} from '../../../widgets/button';
import {Intent} from '../../../widgets/common';
import {Icon} from '../../../widgets/icon';
import {TextInput} from '../../../widgets/text_input';
import {ColumnPicker} from '../widgets/column_picker';

export interface JoinColumn {
  readonly column: string;
  readonly alias: string;
}

export interface JoinConfig {
  readonly leftColumn: string;
  readonly rightColumn: string;
  readonly columns: JoinColumn[];
}

// Compute the resolved aliases for all extend columns.
export function getExtendColumnAliases(
  config: JoinConfig,
  leftAvail: string[],
): JoinColumn[] {
  const leftSet = new Set(leftAvail);
  return config.columns.map((c) => ({
    column: c.column,
    alias: c.alias || (leftSet.has(c.column) ? `right_${c.column}` : c.column),
  }));
}

function render(
  config: JoinConfig,
  updateConfig: (updates: Partial<JoinConfig>) => void,
  ctx: RenderContext,
): m.Children {
  const leftColumns = ctx.getInputColumns('left');
  const rightColumns = ctx.getInputColumns('right');
  const leftSet = new Set(leftColumns.map((c) => c.name));

  return m('.pf-qb-stack', {style: {minWidth: '200px'}}, [
    // Join key pickers side-by-side
    m('.pf-qb-section-label', 'Join on'),
    m('.pf-qb-group-grid', [
      m(ColumnPicker, {
        value: config.leftColumn,
        columns: leftColumns,
        placeholder: 'left col',
        onSelect: (value: string) => {
          updateConfig({leftColumn: value});
        },
      }),
      m('span', {style: {opacity: 0.5}}, '='),
      m(ColumnPicker, {
        value: config.rightColumn,
        columns: rightColumns,
        placeholder: 'right col',
        onSelect: (value: string) => {
          updateConfig({rightColumn: value});
        },
      }),
    ]),

    // Columns to add from right side
    m('.pf-qb-section-label', 'Columns to add'),
    m('.pf-qb-filter-list', [
      ...config.columns.map((entry, i) => {
        const defaultAlias = leftSet.has(entry.column)
          ? `right_${entry.column}`
          : entry.column;
        return m(
          '.pf-qb-filter-row',
          {
            key: i,
            draggable: config.columns.length > 1,
            ondragstart: (e: DragEvent) => {
              e.dataTransfer!.effectAllowed = 'move';
              e.dataTransfer!.setData('text/plain', String(i));
              (e.currentTarget as HTMLElement).classList.add('pf-dragging');
            },
            ondragend: (e: DragEvent) => {
              (e.currentTarget as HTMLElement).classList.remove('pf-dragging');
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
              el.classList.remove('pf-drag-over-top', 'pf-drag-over-bottom');
            },
            ondrop: (e: DragEvent) => {
              e.preventDefault();
              const el = e.currentTarget as HTMLElement;
              const isBottom = el.classList.contains('pf-drag-over-bottom');
              el.classList.remove('pf-drag-over-top', 'pf-drag-over-bottom');
              const fromIdx = parseInt(e.dataTransfer!.getData('text/plain'));
              let toIdx = isBottom ? i + 1 : i;
              if (fromIdx !== toIdx && fromIdx + 1 !== toIdx) {
                const updated = [...config.columns];
                const [moved] = updated.splice(fromIdx, 1);
                if (fromIdx < toIdx) toIdx--;
                updated.splice(toIdx, 0, moved);
                updateConfig({columns: updated});
              }
            },
          },
          [
            ...(config.columns.length > 1 ? [m(Icon, {icon: 'drag_indicator', className: 'pf-qb-drag-handle'})] : []),
            m(ColumnPicker, {
              value: entry.column,
              columns: rightColumns,
              placeholder: 'column',
              onSelect: (value: string) => {
                const updated = [...config.columns];
                updated[i] = {...entry, column: value};
                updateConfig({columns: updated});
              },
            }),
            m('span', {style: {opacity: 0.5, fontSize: '11px'}}, 'as'),
            m(TextInput, {
              placeholder: defaultAlias,
              value: entry.alias,
              onChange: (value: string) => {
                const updated = [...config.columns];
                updated[i] = {...entry, alias: value};
                updateConfig({columns: updated});
              },
            }),
            m(Button, {
              icon: 'delete',
              variant: ButtonVariant.Filled,
              intent: Intent.Danger,
              className: 'pf-qb-row-delete',
              title: 'Remove column',
              onclick: () => {
                updateConfig({
                  columns: config.columns.filter((_, j) => j !== i),
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
          columns: [...config.columns, {column: '', alias: ''}],
        });
      },
    }),
  ]);
}

function isValid(config: JoinConfig): boolean {
  return config.leftColumn !== '' && config.rightColumn !== '';
}

export const manifest: NodeManifest<JoinConfig> = {
  title: 'Join',
  icon: 'add_circle',
  inputs: [
    {name: 'left', content: 'Left', direction: 'left'},
    {name: 'right', content: 'Right', direction: 'left'},
  ],
  outputs: [{name: 'output', content: 'Output', direction: 'right'}],
  canDockTop: true,
  canDockBottom: true,
  hue: 308,
  getOutputColumns(config, ctx) {
    // Output columns are all left columns, plus select right columns.
    const leftCols = ctx.getInputColumns('left') ?? [];
    const rightCols = ctx.getInputColumns('right') ?? [];
    const leftSet = new Set(leftCols.map((c) => c.name));
    const outputCols = [...leftCols];

    for (const entry of config.columns) {
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
  defaultConfig: () => ({leftColumn: '', rightColumn: '', columns: []}),
  render,
  isValid,
  emitIr(config: JoinConfig, ctx: IrContext) {
    const leftRef = ctx.getInputRef('left');
    const rightRef = ctx.getInputRef('right');
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
    const condition = `ON l.${config.leftColumn} = r.${config.rightColumn}`;
    const sql = `SELECT ${selectClause}\nFROM ${leftRef} AS l\nLEFT JOIN ${rightRef} AS r ${condition}`;
    return {sql};
  },
};
