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
  NodeManifest,
  RenderContext,
  SqlStatement,
} from '../node_types';
import {TextInput} from '../../../widgets/text_input';
import type {ColumnDef} from '../graph_utils';
import {moveItem, Row} from '../components/row';
import {AddButton} from '../components/add_button';
import {Stack} from '../components/stack';
import {AliasTag} from '../components/alias_tag';

export interface ExtendEntry {
  readonly expression: string;
  readonly alias: string;
}

export interface ExtendConfig {
  readonly entries: ExtendEntry[];
}

function entryAlias(e: ExtendEntry): string {
  if (e.alias) return e.alias;
  // Use expression text sanitized as a column name.
  if (e.expression) return e.expression.replace(/[^a-zA-Z0-9_]/g, '_');
  return '';
}

function ExtendContent(): m.Component<{
  config: ExtendConfig;
  updateConfig: (updates: Partial<ExtendConfig>) => void;
  ctx: RenderContext;
}> {
  return {
    view({attrs: {config, updateConfig}}) {
      return m(Stack, {className: 'pf-spag-node-wide'}, [
        m(Stack, {compact: true}, [
          ...config.entries.map((entry, i) =>
            m(
              Row,
              {
                key: i,
                reorder: {
                  index: i,
                  onMove: (from: number, to: number) => {
                    updateConfig({entries: moveItem(config.entries, from, to)});
                  },
                },
              },
              [
                m(TextInput, {
                  placeholder: 'expression',
                  value: entry.expression,
                  onChange: (value: string) => {
                    const updated = [...config.entries];
                    updated[i] = {...entry, expression: value};
                    updateConfig({entries: updated});
                  },
                }),
                m(AliasTag, {
                  alias: entry.alias,
                  placeholder: entryAlias(entry) || 'alias',
                  onChange: (value: string) => {
                    const updated = [...config.entries];
                    updated[i] = {...entry, alias: value};
                    updateConfig({entries: updated});
                  },
                }),
                m(Row.DeleteButton, {
                  onclick: () =>
                    updateConfig({
                      entries: config.entries.filter((_, j) => j !== i),
                    }),
                }),
              ],
            ),
          ),
        ]),
        m(AddButton, {
          label: 'Add column',
          onclick: () => {
            updateConfig({
              entries: [...config.entries, {expression: '', alias: ''}],
            });
          },
        }),
      ]);
    },
  };
}

function render(
  config: ExtendConfig,
  updateConfig: (updates: Partial<ExtendConfig>) => void,
  ctx: RenderContext,
): m.Children {
  return m(ExtendContent, {config, updateConfig, ctx});
}

function getOutputColumns(
  config: ExtendConfig,
  ctx: ColumnContext,
): ColumnDef[] | undefined {
  const columns = ctx.getInputColumns('input');
  const added: ColumnDef[] = config.entries
    .filter((e) => e.expression)
    .map((e) => ({name: entryAlias(e)}));
  if (added.length > 0) {
    return [...(columns ?? []), ...added];
  }
  return columns;
}

function isValid(config: ExtendConfig): boolean {
  return config.entries.every((e) => !e.alias || !!e.expression);
}

function tryFold(stmt: SqlStatement, config: ExtendConfig): boolean {
  // Can't append columns to a grouped query.
  if (stmt.groupBy) return false;
  const exprParts = config.entries
    .filter((e) => e.expression)
    .map((e) => `${e.expression} AS ${entryAlias(e)}`);
  if (exprParts.length > 0) {
    stmt.columns = [stmt.columns, ...exprParts].join(', ');
  }
  return true;
}

export const manifest: NodeManifest<ExtendConfig> = {
  title: 'Extend',
  icon: 'add_column_right',
  getInputs: () => [{name: 'input', content: 'Input'}],
  hue: 125,
  defaultConfig: () => ({entries: []}),
  render,
  getOutputColumns,
  isValid,
  tryFold,
};
