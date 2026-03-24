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
import {
  ColumnContext,
  NodeManifest,
  RenderContext,
  SqlStatement,
} from '../node_types';
import {Button, ButtonVariant} from '../../../widgets/button';
import {Intent} from '../../../widgets/common';
import {Icon} from '../../../widgets/icon';
import {TextInput} from '../../../widgets/text_input';
import {ColumnPicker} from '../widgets/column_picker';
import {ColumnDef} from '../graph_utils';

export interface ExtractArgEntry {
  readonly column: string;
  readonly argName: string;
  readonly alias: string;
}

export interface ExtractArgConfig {
  readonly extractions: ExtractArgEntry[];
}

function render(
  config: ExtractArgConfig,
  updateConfig: (updates: Partial<ExtractArgConfig>) => void,
  ctx: RenderContext,
): m.Children {
  return m('.pf-qb-stack', [
    m('.pf-qb-filter-list', [
      ...config.extractions.map((entry, i) =>
        m(
          '.pf-qb-filter-row',
          {
            key: i,
            draggable: config.extractions.length > 1,
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
                const newExtractions = [...config.extractions];
                const [moved] = newExtractions.splice(fromIdx, 1);
                if (fromIdx < toIdx) toIdx--;
                newExtractions.splice(toIdx, 0, moved);
                updateConfig({extractions: newExtractions});
              }
            },
          },
          [
            ...(config.extractions.length > 1 ? [m(Icon, {icon: 'drag_indicator', className: 'pf-qb-drag-handle'})] : []),
            m(ColumnPicker, {
              value: entry.column,
              columns: ctx.availableColumns,
              placeholder: 'column',
              onSelect: (value: string) => {
                const newExtractions = [...config.extractions];
                newExtractions[i] = {...entry, column: value};
                updateConfig({extractions: newExtractions});
              },
            }),
            m(TextInput, {
              placeholder: 'arg name',
              value: entry.argName,
              onChange: (value: string) => {
                const newExtractions = [...config.extractions];
                newExtractions[i] = {...entry, argName: value};
                updateConfig({extractions: newExtractions});
              },
            }),
            m(TextInput, {
              placeholder: 'alias',
              value: entry.alias,
              onChange: (value: string) => {
                const newExtractions = [...config.extractions];
                newExtractions[i] = {...entry, alias: value};
                updateConfig({extractions: newExtractions});
              },
            }),
            m(Button, {
              icon: 'delete',
              variant: ButtonVariant.Filled,
              intent: Intent.Danger,
              className: 'pf-qb-row-delete',
              title: 'Remove extraction',
              onclick: () => {
                const newExtractions = config.extractions.filter(
                  (_, j) => j !== i,
                );
                updateConfig({extractions: newExtractions});
              },
            }),
          ],
        ),
      ),
    ]),
    m(Button, {
      label: 'Add extraction',
      variant: ButtonVariant.Filled,
      icon: 'add',
      onclick: () => {
        updateConfig({
          extractions: [
            ...config.extractions,
            {column: '', argName: '', alias: ''},
          ],
        });
      },
    }),
  ]);
}

function getOutputColumns(
  config: ExtractArgConfig,
  ctx: ColumnContext,
): ColumnDef[] | undefined {
  const columns = ctx.getInputColumns('input');
  const extractAliases: ColumnDef[] = config.extractions
    .filter((e) => e.column && e.argName && e.alias)
    .map((e) => ({name: e.alias}));
  if (extractAliases.length > 0) {
    return [...(columns ?? []), ...extractAliases];
  }
  return columns;
}

function isValid(config: ExtractArgConfig): boolean {
  return config.extractions.every(
    (e) =>
      (!e.column && !e.argName && !e.alias) ||
      (e.column && e.argName && e.alias),
  );
}

function tryFold(stmt: SqlStatement, config: ExtractArgConfig): boolean {
  if (stmt.columns !== '*') return false;
  const exprParts = config.extractions
    .filter((e) => e.column && e.argName && e.alias)
    .map((e) => `extract_arg(${e.column}, '${e.argName}') AS ${e.alias}`);
  if (exprParts.length > 0) {
    stmt.columns = ['*', ...exprParts].join(', ');
  }
  return true;
}

export const manifest: NodeManifest<ExtractArgConfig> = {
  title: 'Extract Arg',
  icon: 'data_object',
  inputs: [{name: 'input', content: 'Input', direction: 'left'}],
  outputs: [{name: 'output', content: 'Output', direction: 'right'}],
  canDockTop: true,
  canDockBottom: true,
  hue: 95,
  defaultConfig: () => ({extractions: []}),
  render,
  getOutputColumns,
  isValid,
  tryFold,
};
