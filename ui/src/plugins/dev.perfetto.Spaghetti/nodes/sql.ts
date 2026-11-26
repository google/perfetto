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
import {Button, ButtonVariant} from '../../../widgets/button';
import {TextInput} from '../../../widgets/text_input';
import {Select} from '../../../widgets/select';
import {InputPortStepper} from '../components/input_port_stepper';
import {Row} from '../components/row';
import {Stack} from '../components/stack';
import './sql.scss';
import type {
  GeneratorContext,
  NodeManifest,
  RenderContext,
} from '../node_types';
import {subquery} from '../generator_utils';
import type {ColumnDef} from '../graph_utils';
import type {SimpleTypeKind} from '../../../trace_processor/perfetto_sql_type';
import type {Port} from '../graph_model';

const SqlOutputColumnSchema = z.object({
  name: z.string(),
  type: z.string(), // SimpleTypeKind or '' for unknown
});

export type SqlOutputColumn = z.infer<typeof SqlOutputColumnSchema>;

const SqlConfigSchema = z
  .object({
    sql: z
      .string()
      .describe('Single SELECT / WITH..SELECT query without trailing ";"'),
    columns: z.array(SqlOutputColumnSchema).optional(),
    /** User-defined alias for each input port, parallel to the inputs array. */
    inputPorts: z.array(z.string()).optional(),
    /** Persisted textarea dimensions. Undefined = use defaults. */
    textareaWidth: z.number().optional(),
    textareaHeight: z.number().optional(),
  })
  .describe(
    'Fallback raw SQL node; connected inputs are bound as CTEs named by inputPorts.',
  );

export type SqlConfig = z.infer<typeof SqlConfigSchema>;

const TYPE_OPTIONS: {label: string; value: string}[] = [
  {label: '(unknown)', value: ''},
  {label: 'int', value: 'int'},
  {label: 'double', value: 'double'},
  {label: 'string', value: 'string'},
  {label: 'boolean', value: 'boolean'},
  {label: 'timestamp', value: 'timestamp'},
  {label: 'duration', value: 'duration'},
  {label: 'bytes', value: 'bytes'},
];

function toColumnDef(col: SqlOutputColumn): ColumnDef {
  if (!col.type) return {name: col.name};
  return {name: col.name, type: {kind: col.type as SimpleTypeKind}};
}

interface InputBinding {
  readonly alias: string;
  readonly source: string;
}

function inputBindings<I>(
  config: SqlConfig,
  ctx: GeneratorContext<I>,
  render: (input: I) => string,
): InputBinding[] {
  const {inputPorts = []} = config;
  const bindings: InputBinding[] = [];
  for (const [i, port] of ctx.inputPorts.entries()) {
    const input = ctx.input(port.name);
    if (input === undefined) continue;
    const alias = inputPorts[i]?.trim() || `input_${i}`;
    bindings.push({alias, source: render(input)});
  }
  return bindings;
}

/** Prepend `alias AS (SELECT * FROM <source>)` CTEs to the user's query. */
function withBindings(sql: string, bindings: readonly InputBinding[]): string {
  if (bindings.length === 0) return sql;
  const ctes = bindings.map(
    (b) => `${b.alias} AS (\n  SELECT * FROM ${b.source}\n)`,
  );
  const trimmed = sql.trimStart();
  const withMatch = trimmed.match(/^with\s/i);
  const body = withMatch ? trimmed.slice(withMatch[0].length) : trimmed;
  return `WITH ${ctes.join(',\n')}${withMatch ? ',\n' : '\n'}${body}`;
}

function SqlNodeContent(): m.Component<{
  readonly config: SqlConfig;
  readonly updateConfig: (updates: Partial<SqlConfig>) => void;
  readonly ctx: RenderContext;
}> {
  let editing = false;
  let draftSql = '';

  return {
    view({attrs: {config, updateConfig, ctx}}) {
      const {columns = [], inputPorts = []} = config;
      const canRemove = inputPorts.length > 0;
      const displaySql = editing ? draftSql : config.sql;
      const isDirty = editing && draftSql !== config.sql;

      const commitSql = () => {
        if (!editing) return;
        const nextSql = draftSql;
        editing = false;
        if (nextSql !== config.sql) {
          updateConfig({sql: nextSql});
        }
      };

      return m(Stack, {minWidth: 'md'}, [
        // Input table aliases
        inputPorts.length > 0 && [
          m('span.pf-spag-section-label', 'Input tables'),
          m(
            '.pf-spag-sql-col-list',
            ctx.inputPorts.map((port, i) =>
              m('.pf-spag-sql-port-row', {key: port.name}, [
                m('span.pf-spag-hint.pf-spag-sql-port-index', `#${i + 1}`),
                m(TextInput, {
                  placeholder: `input_${i}`,
                  value: inputPorts[i] ?? '',
                  onChange: (value: string) => {
                    const next = [...inputPorts];
                    next[i] = value;
                    updateConfig({inputPorts: next});
                  },
                }),
              ]),
            ),
          ),
        ],
        // Add / remove input buttons
        m(InputPortStepper, {
          canRemove,
          onRemove: () =>
            updateConfig({
              inputPorts: inputPorts.slice(0, inputPorts.length - 1),
            }),
          onAdd: () =>
            updateConfig({
              inputPorts: [...inputPorts, `input_${inputPorts.length}`],
            }),
        }),
        // SQL body
        m('.pf-spag-sql-header', [
          m('span.pf-spag-section-label', 'SQL'),
          isDirty &&
            m(Button, {
              label: 'Apply',
              icon: 'play_arrow',
              compact: true,
              variant: ButtonVariant.Filled,
              title: 'Apply SQL (Ctrl/Cmd+Enter)',
              onpointerdown: (e: PointerEvent) => {
                // Prevent textarea blur from double-committing before click.
                e.preventDefault();
                commitSql();
              },
            }),
        ]),
        m('textarea.pf-spag-sql-textarea', {
          value: displaySql,
          spellcheck: false,
          style:
            config.textareaWidth !== undefined ||
            config.textareaHeight !== undefined
              ? {
                  width:
                    config.textareaWidth !== undefined
                      ? `${config.textareaWidth}px`
                      : undefined,
                  height:
                    config.textareaHeight !== undefined
                      ? `${config.textareaHeight}px`
                      : undefined,
                }
              : undefined,
          onwheel: (e: WheelEvent) => {
            if (e.ctrlKey || e.metaKey) return;
            const el = e.currentTarget as HTMLElement;
            if (
              el.scrollHeight > el.clientHeight ||
              el.scrollWidth > el.clientWidth
            ) {
              e.stopPropagation();
            }
          },
          onfocus: () => {
            if (!editing) {
              draftSql = config.sql;
              editing = true;
            }
          },
          oninput: (e: InputEvent) => {
            editing = true;
            draftSql = (e.target as HTMLTextAreaElement).value;
          },
          onkeydown: (e: KeyboardEvent) => {
            if ((e.metaKey || e.ctrlKey) && e.key === 'Enter') {
              e.preventDefault();
              commitSql();
            }
          },
          onblur: () => {
            commitSql();
          },
          onmouseup: (e: MouseEvent) => {
            const el = e.target as HTMLTextAreaElement;
            if (
              el.offsetWidth !== config.textareaWidth ||
              el.offsetHeight !== config.textareaHeight
            ) {
              // Only persist after an actual resize drag.
              if (
                config.textareaWidth !== undefined ||
                config.textareaHeight !== undefined ||
                el.style.width !== '' ||
                el.style.height !== ''
              ) {
                updateConfig({
                  textareaWidth: el.offsetWidth,
                  textareaHeight: el.offsetHeight,
                });
              }
            }
          },
        }),
        // Output column declarations
        m('span.pf-spag-section-label', 'Output columns'),
        columns.length > 0 &&
          m(
            '.pf-spag-sql-col-list',
            columns.map((col, i) =>
              m('.pf-spag-sql-col-row', {key: i}, [
                m(TextInput, {
                  placeholder: 'column name',
                  value: col.name,
                  onChange: (value: string) => {
                    const next = [...columns];
                    next[i] = {...col, name: value};
                    updateConfig({columns: next});
                  },
                }),
                m(
                  Select,
                  {
                    value: col.type,
                    onchange: (e: Event) => {
                      const next = [...columns];
                      next[i] = {
                        ...col,
                        type: (e.target as HTMLSelectElement).value,
                      };
                      updateConfig({columns: next});
                    },
                  },
                  TYPE_OPTIONS.map((opt) =>
                    m('option', {value: opt.value}, opt.label),
                  ),
                ),
                m(Row.DeleteButton, {
                  onclick: () => {
                    updateConfig({columns: columns.filter((_, j) => j !== i)});
                  },
                }),
              ]),
            ),
          ),
        m(Button, {
          label: 'Add output column',
          icon: 'add',
          variant: ButtonVariant.Filled,
          onclick: () => {
            updateConfig({columns: [...columns, {name: '', type: ''}]});
          },
        }),
      ]);
    },
  };
}

export const manifest: NodeManifest<SqlConfig> = {
  title: 'SQL',
  icon: 'code',
  getInputs(config: SqlConfig): Port[] {
    const {inputPorts = []} = config;
    return inputPorts.map((name, i) => ({
      name: `input_${i}`,
      content: name.trim() || `input_${i}`,
    }));
  },
  hue: 250,
  schema: SqlConfigSchema,
  expensive: true,
  defaultConfig: () => ({
    sql: 'SELECT\n  *\nFROM slice\nLIMIT 100',
    columns: [],
    inputPorts: [],
  }),
  render(config, updateConfig, ctx: RenderContext) {
    return m(SqlNodeContent, {config, updateConfig, ctx});
  },
  getOutputColumns(config) {
    const {columns = []} = config;
    if (columns.length === 0) return undefined;
    return columns.map(toColumnDef);
  },
  generators: {
    // Bind each input to its alias with a WITH clause in front of the user's
    // query, and use the whole thing as a subquery source.
    sql: (config, ctx) => {
      if (config.sql.trim() === '') return undefined;
      const bindings = inputBindings(config, ctx, (i) => i.ref);
      return {from: subquery(withBindings(config.sql, bindings))};
    },
    // The user's SQL is plain SQL, so pipeline inputs can't be nested in it:
    // inputs must already be tables (e.g. materialized expensive nodes).
    psn: (config, ctx) => {
      if (config.sql.trim() === '') return undefined;
      if (ctx.inputs().some(({input}) => input.table === undefined)) {
        return {
          error:
            'SQL node inputs must be tables in PerfettoSQL Next mode (a ' +
            'pipe query cannot be nested inside plain SQL).',
        };
      }
      const bindings = inputBindings(config, ctx, (i) => i.source);
      return {subquery: subquery(withBindings(config.sql, bindings))};
    },
  },
};
