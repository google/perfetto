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
import {assertUnreachable} from '../../../base/assert';
import {classNames} from '../../../base/classnames';
import {Button, ButtonVariant} from '../../../widgets/button';
import {Row} from '../components/row';
import {Stack} from '../components/stack';
import type {ColumnDef} from '../graph_utils';
import {
  DATA_TABLE_COLUMN_TYPES,
  type DataTableColumnType,
  type LogicalOp,
} from '../logical_ir';
import type {NodeManifest} from '../node_types';
import './data_table.scss';

const VALID_IDENT = /^[a-zA-Z_][a-zA-Z0-9_]*$/;
const MIN_INT64 = -9223372036854775808n;
const MAX_INT64 = 9223372036854775807n;

const DataTableColumnSchema = z.object({
  name: z.string(),
  type: z.enum(DATA_TABLE_COLUMN_TYPES),
});

const DataTableConfigSchema = z.object({
  columns: z.array(DataTableColumnSchema),
  rows: z.array(z.array(z.string())),
});

export type DataTableConfig = z.infer<typeof DataTableConfigSchema>;

const TYPE_OPTIONS: readonly {
  readonly label: string;
  readonly value: DataTableColumnType;
}[] = [
  {label: 'int', value: 'int'},
  {label: 'double', value: 'double'},
  {label: 'string', value: 'string'},
  {label: 'timestamp', value: 'timestamp'},
  {label: 'duration', value: 'duration'},
  {label: 'boolean', value: 'boolean'},
];

function isValidInt64(trimmed: string): boolean {
  if (!/^-?\d+$/.test(trimmed)) return false;
  try {
    const val = BigInt(trimmed);
    return val >= MIN_INT64 && val <= MAX_INT64;
  } catch {
    return false;
  }
}

export function isCellValid(raw: string, type: DataTableColumnType): boolean {
  if (raw === '' || type === 'string') return true;
  const trimmed = raw.trim();
  if (trimmed === '') return true;
  switch (type) {
    case 'int':
    case 'timestamp':
    case 'duration':
      return isValidInt64(trimmed);
    case 'double':
      return Number.isFinite(Number(trimmed));
    case 'boolean':
      return /^(true|false|1|0)$/i.test(trimmed);
    default:
      return assertUnreachable(type);
  }
}

function inferPastedType(values: readonly string[]): DataTableColumnType {
  const nonEmpty = values.map((v) => v.trim()).filter((v) => v !== '');
  if (nonEmpty.length === 0) return 'string';
  if (nonEmpty.every((v) => isValidInt64(v))) return 'int';
  if (nonEmpty.every((v) => Number.isFinite(Number(v)))) return 'double';
  if (nonEmpty.every((v) => /^(true|false)$/i.test(v))) return 'boolean';
  return 'string';
}

function nextUniqueColName(
  existing: ReadonlySet<string>,
  startIdx: number,
): string {
  let n = startIdx;
  while (existing.has(`col_${n}`)) {
    n++;
  }
  return `col_${n}`;
}

function getColumnValidities(
  columns: readonly {readonly name: string}[],
): readonly boolean[] {
  const counts = new Map<string, number>();
  for (const col of columns) {
    const key = col.name.toLowerCase();
    counts.set(key, (counts.get(key) ?? 0) + 1);
  }
  return columns.map(
    (col) =>
      VALID_IDENT.test(col.name) &&
      (counts.get(col.name.toLowerCase()) ?? 0) === 1,
  );
}

function parseClipboardGrid(
  text: string,
): readonly (readonly string[])[] | undefined {
  if (!text.includes('\n') && !text.includes('\t')) {
    return undefined;
  }
  const normalized = text.replace(/\r\n/g, '\n').replace(/\r/g, '\n');
  const rawLines = normalized.split('\n');
  if (rawLines.length > 1 && rawLines[rawLines.length - 1] === '') {
    rawLines.pop();
  }
  const delimiter = normalized.includes('\t') ? '\t' : ',';
  return rawLines.map((line) => line.split(delimiter));
}

function DataTableContent(): m.Component<{
  readonly config: DataTableConfig;
  readonly updateConfig: (updates: Partial<DataTableConfig>) => void;
}> {
  return {
    view({attrs: {config, updateConfig}}) {
      const colValid = getColumnValidities(config.columns);
      const canDeleteCol = config.columns.length > 1;

      const addColumn = () => {
        const lowerNames = new Set(
          config.columns.map((c) => c.name.toLowerCase()),
        );
        const name = nextUniqueColName(lowerNames, config.columns.length + 1);
        updateConfig({
          columns: [...config.columns, {name, type: 'string'}],
          rows: config.rows.map((r) => [...r, '']),
        });
      };

      const addRow = () => {
        updateConfig({
          rows: [...config.rows, config.columns.map(() => '')],
        });
      };

      const handlePaste = (
        e: ClipboardEvent,
        startRow: number,
        startCol: number,
      ) => {
        const text = e.clipboardData?.getData('text/plain') ?? '';
        const grid = parseClipboardGrid(text);
        if (grid === undefined || grid.length === 0) return;

        e.preventDefault();
        const maxColsNeeded = Math.max(
          config.columns.length,
          ...grid.map((r) => startCol + r.length),
        );
        const maxRowsNeeded = Math.max(
          config.rows.length,
          startRow + grid.length,
        );

        const nextCols = [...config.columns];
        const lowerNames = new Set(nextCols.map((c) => c.name.toLowerCase()));
        for (let c = nextCols.length; c < maxColsNeeded; c++) {
          const pastedSamples = grid.map((gr) => gr[c - startCol] ?? '');
          const name = nextUniqueColName(lowerNames, c + 1);
          lowerNames.add(name);
          nextCols.push({name, type: inferPastedType(pastedSamples)});
        }

        const nextRows: string[][] = [];
        for (let r = 0; r < maxRowsNeeded; r++) {
          const existing = config.rows[r] ?? [];
          const row: string[] = [];
          for (let c = 0; c < maxColsNeeded; c++) {
            row.push(existing[c] ?? '');
          }
          nextRows.push(row);
        }

        for (const [dr, pastedRow] of grid.entries()) {
          const targetRow = nextRows[startRow + dr];
          if (targetRow === undefined) continue;
          for (const [dc, cellVal] of pastedRow.entries()) {
            targetRow[startCol + dc] = cellVal;
          }
        }

        updateConfig({columns: nextCols, rows: nextRows});
      };

      return m(Stack, [
        m(
          '.pf-spag-data-table-wrap',
          {
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
          },
          m('table.pf-spag-data-table', [
            m('thead', [
              m('tr', [
                ...config.columns.map((col, colIdx) =>
                  m('th.pf-spag-data-table__th', [
                    m('.pf-spag-data-table__col-header', [
                      m('.pf-spag-data-table__col-top', [
                        m('input.pf-spag-data-table__col-name', {
                          className: classNames(
                            colValid[colIdx] === false &&
                              'pf-spag-data-table__col-name--invalid',
                          ),
                          value: col.name,
                          placeholder: 'col',
                          spellcheck: false,
                          oninput: (e: InputEvent) => {
                            const next = [...config.columns];
                            next[colIdx] = {
                              ...col,
                              name: (e.target as HTMLInputElement).value.trim(),
                            };
                            updateConfig({columns: next});
                          },
                        }),
                        canDeleteCol &&
                          m(
                            'button.pf-spag-data-table__col-del',
                            {
                              type: 'button',
                              title: 'Delete column',
                              onclick: () => {
                                updateConfig({
                                  columns: config.columns.filter(
                                    (_, j) => j !== colIdx,
                                  ),
                                  rows: config.rows.map((r) =>
                                    r.filter((_, j) => j !== colIdx),
                                  ),
                                });
                              },
                            },
                            '×',
                          ),
                      ]),
                      m(
                        'select.pf-spag-data-table__col-type',
                        {
                          value: col.type,
                          onchange: (e: Event) => {
                            const next = [...config.columns];
                            next[colIdx] = {
                              ...col,
                              type: (e.target as HTMLSelectElement)
                                .value as DataTableColumnType,
                            };
                            updateConfig({columns: next});
                          },
                        },
                        TYPE_OPTIONS.map((opt) =>
                          m('option', {value: opt.value}, opt.label),
                        ),
                      ),
                    ]),
                  ]),
                ),
                m('th.pf-spag-data-table__th.pf-spag-data-table__th--actions'),
              ]),
            ]),
            m(
              'tbody',
              config.rows.length === 0
                ? m('tr', [
                    m(
                      'td.pf-spag-data-table__empty',
                      {colSpan: config.columns.length + 1},
                      'No rows (0 rows emitted)',
                    ),
                  ])
                : config.rows.map((row, rowIdx) =>
                    m('tr.pf-spag-data-table__tr', [
                      ...config.columns.map((col, colIdx) => {
                        const val = row[colIdx] ?? '';
                        const valid = isCellValid(val, col.type);
                        return m('td.pf-spag-data-table__td', [
                          m('input.pf-spag-data-table__cell', {
                            className: classNames(
                              !valid && 'pf-spag-data-table__cell--invalid',
                            ),
                            value: val,
                            placeholder: 'NULL',
                            spellcheck: false,
                            onpaste: (e: ClipboardEvent) =>
                              handlePaste(e, rowIdx, colIdx),
                            oninput: (e: InputEvent) => {
                              const nextVal = (e.target as HTMLInputElement)
                                .value;
                              const nextRows = config.rows.map((r, rIdx) => {
                                if (rIdx !== rowIdx) return r;
                                return config.columns.map((_, cIdx) =>
                                  cIdx === colIdx ? nextVal : (r[cIdx] ?? ''),
                                );
                              });
                              updateConfig({rows: nextRows});
                            },
                          }),
                        ]);
                      }),
                      m(
                        'td.pf-spag-data-table__td.pf-spag-data-table__td--actions',
                        m(Row.DeleteButton, {
                          onclick: () =>
                            updateConfig({
                              rows: config.rows.filter((_, j) => j !== rowIdx),
                            }),
                        }),
                      ),
                    ]),
                  ),
            ),
          ]),
        ),
        m('.pf-spag-data-table-actions', [
          m(Button, {
            label: '+ Row',
            variant: ButtonVariant.Filled,
            onclick: addRow,
          }),
          m(Button, {
            label: '+ Column',
            variant: ButtonVariant.Filled,
            onclick: addColumn,
          }),
        ]),
      ]);
    },
  };
}

function isValid(config: DataTableConfig): boolean {
  if (config.columns.length === 0) return false;
  const colValid = getColumnValidities(config.columns);
  if (colValid.some((v) => !v)) return false;
  for (const row of config.rows) {
    for (const [colIdx, col] of config.columns.entries()) {
      if (!isCellValid(row[colIdx] ?? '', col.type)) {
        return false;
      }
    }
  }
  return true;
}

function getOutputColumns(config: DataTableConfig): ColumnDef[] | undefined {
  const colValid = getColumnValidities(config.columns);
  if (config.columns.length === 0 || colValid.some((v) => !v)) {
    return undefined;
  }
  return config.columns.map((col) => ({
    name: col.name,
    type: {kind: col.type},
  }));
}

function toLogicalOp(config: DataTableConfig): LogicalOp | undefined {
  if (!isValid(config)) return undefined;
  return {
    kind: 'data_table',
    columns: config.columns,
    rows: config.rows.map((row) =>
      config.columns.map((_, idx) => row[idx] ?? ''),
    ),
  };
}

export const manifest: NodeManifest<DataTableConfig> = {
  title: 'Data Table',
  icon: 'grid_on',
  hue: 192,
  schema: DataTableConfigSchema,
  defaultConfig: () => ({
    columns: [
      {name: 'id', type: 'int'},
      {name: 'name', type: 'string'},
    ],
    rows: [['1', 'example']],
  }),
  render: (config, updateConfig) => m(DataTableContent, {config, updateConfig}),
  getOutputColumns,
  isValid,
  toLogicalOp,
};
