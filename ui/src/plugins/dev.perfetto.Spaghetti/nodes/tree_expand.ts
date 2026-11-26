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
import {moveItem} from '../../../base/array_utils';
import {Button, ButtonVariant} from '../../../widgets/button';
import {RadioGroup} from '../../../widgets/radio_group';
import {DraggableStack} from '../components/draggable_stack';
import {Stack} from '../components/stack';
import {TreeColumnsRow} from '../components/tree_columns_row';
import {subquery, wrapTreeIdStages} from '../generator_utils';
import type {ColumnDef} from '../graph_utils';
import type {NodeManifest, RenderContext} from '../node_types';
import {ColumnPicker} from '../widgets/column_picker';

const TreeExpandConfigSchema = z
  .object({
    direction: z.enum(['UP', 'DOWN']),
    idColumn: z.string(),
    parentIdColumn: z.string(),
    byColumns: z.array(z.string()),
    orderColumns: z.array(z.string()).optional(),
    chargeColumns: z.array(z.string()).optional(),
    chargeEnd: z.enum(['LEAF', 'ROOT']).optional(),
  })
  .describe(
    'Expand each tree node into a chain of matching rows from the Inserted relation, optionally charging columns to LEAF or ROOT.',
  );

export type TreeExpandConfig = z.infer<typeof TreeExpandConfigSchema>;

function nonEmpty(arr: readonly string[] | undefined): string[] {
  return (arr ?? []).filter((c) => c !== '');
}

function TreeExpandContent(): m.Component<{
  config: TreeExpandConfig;
  updateConfig: (updates: Partial<TreeExpandConfig>) => void;
  ctx: RenderContext;
}> {
  return {
    view({attrs: {config, updateConfig, ctx}}) {
      const insertedCols = ctx.getInputColumns('inserted');
      const orderColumns = config.orderColumns ?? [];
      const chargeColumns = config.chargeColumns ?? [];
      const chargeEnd = config.chargeEnd ?? 'LEAF';

      return m(Stack, {minWidth: 'lg'}, [
        m(
          RadioGroup,
          {
            fillWidth: true,
            selectedValue: config.direction,
            onValueChange: (value: string) =>
              updateConfig({direction: value as 'UP' | 'DOWN'}),
          },
          [
            m(
              RadioGroup.Button,
              {value: 'DOWN', icon: 'arrow_downward'},
              'Down',
            ),
            m(RadioGroup.Button, {value: 'UP', icon: 'arrow_upward'}, 'Up'),
          ],
        ),
        m(TreeColumnsRow, {
          idColumn: config.idColumn,
          parentIdColumn: config.parentIdColumn,
          columns: ctx.availableColumns,
          onIdChange: (idColumn) => updateConfig({idColumn}),
          onParentIdChange: (parentIdColumn) => updateConfig({parentIdColumn}),
        }),
        m('.pf-spag-section-label', 'Join keys (BY)'),
        m(DraggableStack, {
          rows: config.byColumns.map((col, i) => [
            m(ColumnPicker, {
              value: col,
              columns: ctx.availableColumns,
              placeholder: 'column',
              onSelect: (value: string) =>
                updateConfig({byColumns: config.byColumns.with(i, value)}),
            }),
          ]),
          onReorder: (from, to) =>
            updateConfig({byColumns: moveItem(config.byColumns, from, to)}),
          onDelete: (i) =>
            updateConfig({byColumns: config.byColumns.toSpliced(i, 1)}),
        }),
        m(Button, {
          label: 'Add key column',
          icon: 'add',
          variant: ButtonVariant.Filled,
          onclick: () => updateConfig({byColumns: [...config.byColumns, '']}),
        }),
        m('.pf-spag-section-label', 'Chain order (optional)'),
        m(DraggableStack, {
          rows: orderColumns.map((col, i) => [
            m(ColumnPicker, {
              value: col,
              columns: insertedCols,
              placeholder: 'column',
              onSelect: (value: string) =>
                updateConfig({orderColumns: orderColumns.with(i, value)}),
            }),
          ]),
          onReorder: (from, to) =>
            updateConfig({orderColumns: moveItem(orderColumns, from, to)}),
          onDelete: (i) =>
            updateConfig({orderColumns: orderColumns.toSpliced(i, 1)}),
        }),
        m(Button, {
          label: 'Add order column',
          icon: 'add',
          variant: ButtonVariant.Filled,
          onclick: () => updateConfig({orderColumns: [...orderColumns, '']}),
        }),
        m('.pf-spag-section-label', 'Charge columns (optional)'),
        m(DraggableStack, {
          rows: chargeColumns.map((col, i) => [
            m(ColumnPicker, {
              value: col,
              columns: ctx.availableColumns,
              placeholder: 'column',
              onSelect: (value: string) =>
                updateConfig({chargeColumns: chargeColumns.with(i, value)}),
            }),
          ]),
          onReorder: (from, to) =>
            updateConfig({chargeColumns: moveItem(chargeColumns, from, to)}),
          onDelete: (i) =>
            updateConfig({chargeColumns: chargeColumns.toSpliced(i, 1)}),
        }),
        m(Button, {
          label: 'Add charge column',
          icon: 'add',
          variant: ButtonVariant.Filled,
          onclick: () => updateConfig({chargeColumns: [...chargeColumns, '']}),
        }),
        chargeColumns.length > 0 &&
          m(
            RadioGroup,
            {
              fillWidth: true,
              selectedValue: chargeEnd,
              onValueChange: (value: string) =>
                updateConfig({chargeEnd: value as 'LEAF' | 'ROOT'}),
            },
            [
              m(RadioGroup.Button, {value: 'LEAF'}, 'To leaf'),
              m(RadioGroup.Button, {value: 'ROOT'}, 'To root'),
            ],
          ),
      ]);
    },
  };
}

function isValid(config: TreeExpandConfig): boolean {
  return (
    config.idColumn !== '' &&
    config.parentIdColumn !== '' &&
    nonEmpty(config.byColumns).length > 0
  );
}

function extraInsertedColumns(
  inputCols: readonly ColumnDef[],
  insertedCols: readonly ColumnDef[],
): ColumnDef[] {
  const inputSet = new Set(inputCols.map((c) => c.name));
  return insertedCols.filter((c) => !inputSet.has(c.name));
}

function treeExpandSql(
  ref: string,
  insertedRef: string,
  config: TreeExpandConfig,
  inputCols: readonly ColumnDef[],
  insertedCols: readonly ColumnDef[],
): string {
  const {direction, idColumn, parentIdColumn} = config;
  const keys = nonEmpty(config.byColumns);
  const orderCols = nonEmpty(config.orderColumns);
  const chargeSet = new Set(nonEmpty(config.chargeColumns));
  const chargeEnd = config.chargeEnd ?? 'LEAF';
  const extras = extraInsertedColumns(inputCols, insertedCols);

  const joinCond = keys.map((k) => `ins.${k} IS src.${k}`).join(' AND ');
  const orderDir = direction === 'UP' ? 'DESC' : 'ASC';
  const orderExpr =
    orderCols.length > 0
      ? orderCols.map((c) => `ins.${c} ${orderDir}`).join(', ')
      : '1';
  const keepChargeCond =
    chargeEnd === 'LEAF' ? 'e._pos = e._cnt' : 'e._pos = 1';

  const selectCols: string[] = inputCols.map((c) => {
    if (c.name === idColumn) return `e._new_id AS ${idColumn}`;
    if (c.name === parentIdColumn) {
      return `IIF(e._pos > 1, e._new_id - 1, parent_tail._new_id) AS ${parentIdColumn}`;
    }
    if (chargeSet.has(c.name)) {
      return `IIF(${keepChargeCond}, src.${c.name}, 0) AS ${c.name}`;
    }
    return `src.${c.name}`;
  });
  for (const extra of extras) {
    selectCols.push(`e._ins_${extra.name} AS ${extra.name}`);
  }

  const insSelects = extras.map((c) => `ins.${c.name} AS _ins_${c.name}`);

  return [
    'WITH',
    `  _joined AS (\n` +
      `    SELECT\n` +
      `      ${[
        `src.${idColumn} AS _orig_id`,
        `src.${parentIdColumn} AS _orig_parent_id`,
        `ROW_NUMBER() OVER (PARTITION BY src.${idColumn} ORDER BY ${orderExpr}) AS _pos`,
        `COUNT(*) OVER (PARTITION BY src.${idColumn}) AS _cnt`,
        ...insSelects,
      ].join(',\n      ')}\n` +
      `    FROM ${ref} AS src\n` +
      `    LEFT JOIN ${insertedRef} AS ins ON ${joinCond}\n` +
      `  ),`,
    `  _expanded AS (\n` +
      `    SELECT\n` +
      `      _joined.*,\n` +
      `      ROW_NUMBER() OVER (ORDER BY _orig_id, _pos) - 1 AS _new_id\n` +
      `    FROM _joined\n` +
      `  )`,
    `SELECT\n  ${selectCols.join(',\n  ')}\n` +
      `FROM _expanded AS e\n` +
      `JOIN ${ref} AS src ON src.${idColumn} = e._orig_id\n` +
      `LEFT JOIN _expanded AS parent_tail\n` +
      `  ON parent_tail._orig_id = e._orig_parent_id AND parent_tail._pos = parent_tail._cnt`,
  ].join('\n');
}

export const manifest: NodeManifest<TreeExpandConfig> = {
  title: 'Tree Expand',
  icon: 'unfold_more',
  getInputs: () => [
    {name: 'input', content: 'Input'},
    {name: 'inserted', content: 'Expand with'},
  ],
  hue: 108,
  schema: TreeExpandConfigSchema,
  expensive: true,
  defaultConfig: () => ({
    direction: 'DOWN',
    idColumn: 'id',
    parentIdColumn: 'parent_id',
    byColumns: [],
    orderColumns: [],
    chargeColumns: [],
    chargeEnd: 'LEAF',
  }),
  render: (config, updateConfig, ctx) =>
    m(TreeExpandContent, {config, updateConfig, ctx}),
  getOutputColumns(_config, ctx) {
    const inputCols = ctx.getInputColumns('input');
    if (inputCols === undefined) return undefined;
    const insertedCols = ctx.getInputColumns('inserted') ?? [];
    return [...inputCols, ...extraInsertedColumns(inputCols, insertedCols)];
  },
  generators: {
    sql: (config, ctx) => {
      const input = ctx.input('input');
      if (input === undefined || !isValid(config)) return undefined;
      const inserted = ctx.input('inserted');
      if (inserted === undefined) return {};
      const inputCols = ctx.getInputColumns('input');
      if (inputCols === undefined) {
        return {
          error: 'Tree Expand needs known input columns in Standard SQL mode.',
        };
      }
      const insertedCols = ctx.getInputColumns('inserted') ?? [];
      return {
        from: subquery(
          treeExpandSql(
            input.ref,
            inserted.ref,
            config,
            inputCols,
            insertedCols,
          ),
        ),
      };
    },
    psn: (config, ctx) => {
      if (ctx.input('input') === undefined || !isValid(config)) {
        return undefined;
      }
      const inserted = ctx.input('inserted');
      if (inserted === undefined) return {};
      const keys = nonEmpty(config.byColumns);
      const orderCols = nonEmpty(config.orderColumns);
      const chargeCols = nonEmpty(config.chargeColumns);
      const orderClause =
        orderCols.length > 0 ? ` ORDER BY ${orderCols.join(', ')}` : '';
      const chargeClause =
        chargeCols.length > 0
          ? ` CHARGE ${chargeCols.join(', ')} TO ${config.chargeEnd ?? 'LEAF'}`
          : '';
      return {
        stages: wrapTreeIdStages(config.idColumn, config.parentIdColumn, [
          `TREE EXPAND ${config.direction} ${inserted.source} BY ${keys.join(', ')}${orderClause}${chargeClause}`,
        ]),
      };
    },
  },
};
