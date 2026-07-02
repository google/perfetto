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
import type {
  ColumnContext,
  IrContext,
  NodeManifest,
  RenderContext,
} from '../node_types';
import {Button, ButtonVariant} from '../../../widgets/button';
import {Row} from '../components/row';
import {Stack} from '../components/stack';
import {ColumnPicker} from '../widgets/column_picker';
import type {ColumnDef} from '../graph_utils';

export interface DropConfig {
  // Names of the input columns to drop.
  readonly columns: string[];
}

function DropContent(): m.Component<{
  config: DropConfig;
  updateConfig: (updates: Partial<DropConfig>) => void;
  ctx: RenderContext;
}> {
  return {
    view({attrs: {config, updateConfig, ctx}}) {
      return m(Stack, [
        config.columns.length === 0 &&
          m(
            '.pf-spag-passthrough-hint',
            {
              style: {
                opacity: 0.5,
                fontStyle: 'italic',
                fontSize: '11px',
                padding: '2px 4px',
              },
            },
            'All columns (passthrough)',
          ),
        m(Stack, {compact: true}, [
          ...config.columns.map((column, i) =>
            m(Row, {key: i}, [
              m(ColumnPicker, {
                value: column,
                columns: ctx.availableColumns,
                placeholder: 'column',
                onSelect: (value: string) => {
                  const updated = [...config.columns];
                  updated[i] = value;
                  updateConfig({columns: updated});
                },
              }),
              m(Row.DeleteButton, {
                onclick: () => {
                  updateConfig({
                    columns: config.columns.filter((_, j) => j !== i),
                  });
                },
              }),
            ]),
          ),
        ]),
        m(Button, {
          label: 'Drop column',
          variant: ButtonVariant.Filled,
          icon: 'add',
          onclick: () => {
            updateConfig({columns: [...config.columns, '']});
          },
        }),
      ]);
    },
  };
}

function render(
  config: DropConfig,
  updateConfig: (updates: Partial<DropConfig>) => void,
  ctx: RenderContext,
): m.Children {
  return m(DropContent, {config, updateConfig, ctx});
}

function getOutputColumns(
  config: DropConfig,
  ctx: ColumnContext,
): ColumnDef[] | undefined {
  const columns = ctx.getInputColumns('input');
  if (columns === undefined) return undefined;
  const dropped = new Set(config.columns.filter((c) => c));
  return columns.filter((c) => !dropped.has(c.name));
}

function isValid(): boolean {
  return true;
}

function emitIr(config: DropConfig, ctx: IrContext) {
  const ref = ctx.getInputRef('input');
  if (!ref) return undefined;

  const dropped = new Set(config.columns.filter((c) => c));
  const inputColumns = ctx.getInputColumns('input');

  // Without knowing the input columns (or with nothing to drop) we can't
  // expand the keep-list, so pass everything through.
  const kept =
    inputColumns?.filter((c) => !dropped.has(c.name)).map((c) => c.name) ?? [];
  if (dropped.size === 0 || kept.length === 0) {
    return {sql: `SELECT *\nFROM ${ref}`};
  }

  const selectClause = kept.length > 1 ? '\n  ' + kept.join(',\n  ') : kept[0];
  return {sql: `SELECT ${selectClause}\nFROM ${ref}`};
}

export const manifest: NodeManifest<DropConfig> = {
  title: 'Drop',
  icon: 'remove',
  getInputs: () => [{name: 'input', content: 'Input'}],
  hue: 5,
  defaultConfig: () => ({columns: []}),
  render,
  getOutputColumns,
  isValid,
  emitIr,
};
