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
import {Checkbox} from '../../../widgets/checkbox';
import {RadioGroup} from '../../../widgets/radio_group';
import {Select} from '../../../widgets/select';
import {TextInput} from '../../../widgets/text_input';
import {ColumnPickerWithAlias} from '../components/column_picker_with_alias';
import {DraggableStack} from '../components/draggable_stack';
import {Row} from '../components/row';
import {Stack} from '../components/stack';
import {TreeColumnsRow} from '../components/tree_columns_row';
import {subquery, wrapTreeIdStages} from '../generator_utils';
import type {ColumnDef} from '../graph_utils';
import type {
  GeneratorContext,
  NodeManifest,
  RenderContext,
} from '../node_types';
import {ColumnPicker} from '../widgets/column_picker';

const TreeFindOrderTermSchema = z.object({
  column: z.string(),
  order: z.enum(['ASC', 'DESC']),
});

const TreeFindColumnSchema = z.object({
  column: z.string(),
  alias: z.string().describe('Output column name ("" auto-generates)'),
});

const TreeFindConfigSchema = z
  .object({
    joinType: z.enum(['INNER', 'LEFT']),
    direction: z.enum(['ANCESTOR', 'DESCENDANT']),
    orSelf: z.boolean(),
    idColumn: z.string(),
    parentIdColumn: z.string(),
    targetIdColumn: z.string(),
    alias: z
      .string()
      .describe('Table alias for the Target relation in WHERE/ORDER BY'),
    whereExpr: z.string().describe('Filter expression referencing alias.<col>'),
    orderBy: z.array(TreeFindOrderTermSchema).optional(),
    columns: z.array(TreeFindColumnSchema).optional(),
  })
  .describe(
    'Find the nearest matching ancestor or descendant in Target for each node in the Input tree.',
  );

export type TreeFindConfig = z.infer<typeof TreeFindConfigSchema>;

function resolvedColumns(
  config: TreeFindConfig,
  inputCols: readonly ColumnDef[],
): Array<{column: string; alias: string}> {
  const inputSet = new Set(inputCols.map((c) => c.name));
  const prefix = config.alias || 'found';
  return (config.columns ?? []).map((entry) => ({
    column: entry.column,
    alias:
      entry.alias !== ''
        ? entry.alias
        : inputSet.has(entry.column)
          ? `${prefix}_${entry.column}`
          : entry.column,
  }));
}

function TreeFindContent(): m.Component<{
  config: TreeFindConfig;
  updateConfig: (updates: Partial<TreeFindConfig>) => void;
  ctx: RenderContext;
}> {
  return {
    view({attrs: {config, updateConfig, ctx}}) {
      const targetCols = ctx.getInputColumns('target');
      const inputCols = ctx.availableColumns;
      const inputSet = new Set(inputCols.map((c) => c.name));
      const orderBy = config.orderBy ?? [];
      const columns = config.columns ?? [];
      const prefix = config.alias || 'found';

      return m(Stack, {minWidth: 'lg'}, [
        m(
          RadioGroup,
          {
            fillWidth: true,
            selectedValue: config.joinType,
            onValueChange: (value: string) =>
              updateConfig({joinType: value as 'INNER' | 'LEFT'}),
          },
          [
            m(RadioGroup.Button, {value: 'LEFT'}, 'Left'),
            m(RadioGroup.Button, {value: 'INNER'}, 'Inner'),
          ],
        ),
        m(
          RadioGroup,
          {
            fillWidth: true,
            selectedValue: config.direction,
            onValueChange: (value: string) =>
              updateConfig({direction: value as 'ANCESTOR' | 'DESCENDANT'}),
          },
          [
            m(RadioGroup.Button, {value: 'ANCESTOR'}, 'Nearest ancestor'),
            m(RadioGroup.Button, {value: 'DESCENDANT'}, 'Nearest descendant'),
          ],
        ),
        m(Checkbox, {
          label: 'Include self (OR SELF)',
          checked: config.orSelf,
          onchange: (e: Event) =>
            updateConfig({orSelf: (e.target as HTMLInputElement).checked}),
        }),
        m(TreeColumnsRow, {
          idColumn: config.idColumn,
          parentIdColumn: config.parentIdColumn,
          columns: inputCols,
          onIdChange: (idColumn) => updateConfig({idColumn}),
          onParentIdChange: (parentIdColumn) => updateConfig({parentIdColumn}),
        }),
        m('.pf-spag-section-label', 'Target ID & alias'),
        m(Row, [
          m(ColumnPicker, {
            value: config.targetIdColumn,
            columns: targetCols,
            placeholder: 'id',
            onSelect: (targetIdColumn: string) =>
              updateConfig({targetIdColumn}),
          }),
          m('span.pf-spag-operator', 'AS'),
          m(TextInput, {
            placeholder: 'found',
            value: config.alias,
            onChange: (alias: string) => updateConfig({alias: alias.trim()}),
          }),
        ]),
        m('.pf-spag-section-label', 'Where'),
        m(TextInput, {
          placeholder: `${prefix}.name = 'foo'`,
          value: config.whereExpr,
          onChange: (whereExpr: string) => updateConfig({whereExpr}),
        }),
        config.direction === 'DESCENDANT' && [
          m('.pf-spag-section-label', 'Tie-breaker order (optional)'),
          m(DraggableStack, {
            rows: orderBy.map((term, i) => [
              m(ColumnPicker, {
                value: term.column,
                columns: targetCols,
                placeholder: 'column',
                onSelect: (column: string) =>
                  updateConfig({orderBy: orderBy.with(i, {...term, column})}),
              }),
              m(
                Select,
                {
                  value: term.order,
                  onchange: (e: Event) =>
                    updateConfig({
                      orderBy: orderBy.with(i, {
                        ...term,
                        order: (e.target as HTMLSelectElement).value as
                          'ASC' | 'DESC',
                      }),
                    }),
                },
                [
                  m('option', {value: 'ASC'}, 'ASC'),
                  m('option', {value: 'DESC'}, 'DESC'),
                ],
              ),
            ]),
            onReorder: (from, to) =>
              updateConfig({orderBy: moveItem(orderBy, from, to)}),
            onDelete: (i) => updateConfig({orderBy: orderBy.toSpliced(i, 1)}),
          }),
          m(Button, {
            label: 'Add order term',
            icon: 'add',
            variant: ButtonVariant.Filled,
            onclick: () =>
              updateConfig({
                orderBy: [...orderBy, {column: '', order: 'ASC'}],
              }),
          }),
        ],
        m('.pf-spag-section-label', 'Target columns to carry'),
        m(DraggableStack, {
          rows: columns.map((entry, i) =>
            m(ColumnPickerWithAlias, {
              value: entry.column,
              columns: targetCols,
              placeholder: 'column',
              alias: entry.alias,
              aliasPlaceholder:
                entry.column !== '' && inputSet.has(entry.column)
                  ? `${prefix}_${entry.column}`
                  : entry.column || 'alias',
              onSelect: (column: string) =>
                updateConfig({columns: columns.with(i, {...entry, column})}),
              onAliasChange: (alias: string) =>
                updateConfig({columns: columns.with(i, {...entry, alias})}),
            }),
          ),
          onReorder: (from, to) =>
            updateConfig({columns: moveItem(columns, from, to)}),
          onDelete: (i) => updateConfig({columns: columns.toSpliced(i, 1)}),
        }),
        m(Button, {
          label: 'Add target column',
          icon: 'add',
          variant: ButtonVariant.Filled,
          onclick: () =>
            updateConfig({columns: [...columns, {column: '', alias: ''}]}),
        }),
      ]);
    },
  };
}

function isValid(config: TreeFindConfig): boolean {
  return (
    config.idColumn !== '' &&
    config.parentIdColumn !== '' &&
    config.targetIdColumn !== '' &&
    config.alias !== '' &&
    config.whereExpr.trim() !== ''
  );
}

function activeOrderTerms(
  config: TreeFindConfig,
): Array<{column: string; order: 'ASC' | 'DESC'}> {
  if (config.direction !== 'DESCENDANT') return [];
  return (config.orderBy ?? []).filter((t) => t.column !== '');
}

function treeFindSelect(
  config: TreeFindConfig,
  ctx: GeneratorContext<unknown>,
): string[] {
  const inputCols = ctx.getInputColumns('input') ?? [];
  const picked = resolvedColumns(config, inputCols)
    .filter((c) => c.column !== '')
    .map((c) =>
      c.alias !== c.column
        ? `${config.alias}.${c.column} AS ${c.alias}`
        : `${config.alias}.${c.column}`,
    );
  return ['_in.*', ...picked];
}

function treeFindSql(
  ref: string,
  targetRef: string,
  config: TreeFindConfig,
  ctx: GeneratorContext<unknown>,
): string {
  const {idColumn, parentIdColumn, targetIdColumn, alias, direction, orSelf} =
    config;
  const walkCte =
    direction === 'ANCESTOR'
      ? `  _walk(_anchor, _cand, _dist) AS (\n` +
        (orSelf
          ? `    SELECT ${idColumn}, ${idColumn}, 0 FROM ${ref}\n`
          : `    SELECT ${idColumn}, ${parentIdColumn}, 1 FROM ${ref} WHERE ${parentIdColumn} IS NOT NULL\n`) +
        `    UNION ALL\n` +
        `    SELECT w._anchor, p.${parentIdColumn}, w._dist + 1\n` +
        `    FROM _walk AS w\n` +
        `    JOIN ${ref} AS p ON p.${idColumn} = w._cand\n` +
        `    WHERE p.${parentIdColumn} IS NOT NULL\n` +
        `  ),`
      : `  _walk(_anchor, _cand, _dist) AS (\n` +
        `    SELECT ${idColumn}, ${idColumn}, 0 FROM ${ref}\n` +
        `    UNION ALL\n` +
        `    SELECT w._anchor, c.${idColumn}, w._dist + 1\n` +
        `    FROM _walk AS w\n` +
        `    JOIN ${ref} AS c ON c.${parentIdColumn} = w._cand\n` +
        `  ),`;

  const distGuard =
    direction === 'DESCENDANT' && !orSelf ? 'w._dist > 0 AND ' : '';
  const extraOrder = activeOrderTerms(config).map(
    (t) => `${alias}.${t.column} ${t.order}`,
  );
  const rankOrder = ['w._dist ASC', ...extraOrder].join(', ');
  const joinKw = config.joinType === 'INNER' ? 'JOIN' : 'LEFT JOIN';
  const selectCols = treeFindSelect(config, ctx);

  return [
    'WITH RECURSIVE',
    walkCte,
    `  _ranked AS (\n` +
      `    SELECT\n` +
      `      w._anchor,\n` +
      `      w._cand,\n` +
      `      ROW_NUMBER() OVER (PARTITION BY w._anchor ORDER BY ${rankOrder}) AS _rn\n` +
      `    FROM _walk AS w\n` +
      `    JOIN ${targetRef} AS ${alias} ON ${alias}.${targetIdColumn} = w._cand\n` +
      `    WHERE ${distGuard}(${config.whereExpr.trim()})\n` +
      `  ),`,
    `  _best AS (\n` +
      `    SELECT _anchor, _cand FROM _ranked WHERE _rn = 1\n` +
      `  )`,
    `SELECT\n  ${selectCols.join(',\n  ')}\n` +
      `FROM ${ref} AS _in\n` +
      `${joinKw} _best ON _best._anchor = _in.${idColumn}\n` +
      `${joinKw} ${targetRef} AS ${alias} ON ${alias}.${targetIdColumn} = _best._cand`,
  ].join('\n');
}

export const manifest: NodeManifest<TreeFindConfig> = {
  title: 'Tree Find',
  icon: 'manage_search',
  getInputs: () => [
    {name: 'input', content: 'Input'},
    {name: 'target', content: 'Target'},
  ],
  hue: 80,
  schema: TreeFindConfigSchema,
  expensive: true,
  defaultConfig: () => ({
    joinType: 'LEFT',
    direction: 'ANCESTOR',
    orSelf: false,
    idColumn: 'id',
    parentIdColumn: 'parent_id',
    targetIdColumn: 'id',
    alias: 'found',
    whereExpr: '',
    orderBy: [],
    columns: [],
  }),
  render: (config, updateConfig, ctx) =>
    m(TreeFindContent, {config, updateConfig, ctx}),
  getOutputColumns(config, ctx) {
    const inputCols = ctx.getInputColumns('input');
    if (inputCols === undefined) return undefined;
    const targetCols = ctx.getInputColumns('target') ?? [];
    const outputCols: ColumnDef[] = [...inputCols];
    for (const entry of resolvedColumns(config, inputCols)) {
      if (entry.column === '') continue;
      const col = targetCols.find((c) => c.name === entry.column);
      outputCols.push({name: entry.alias, type: col?.type});
    }
    return outputCols;
  },
  generators: {
    sql: (config, ctx) => {
      const input = ctx.input('input');
      if (input === undefined || !isValid(config)) return undefined;
      const target = ctx.input('target');
      if (target === undefined) return {};
      return {from: subquery(treeFindSql(input.ref, target.ref, config, ctx))};
    },
    psn: (config, ctx) => {
      if (ctx.input('input') === undefined || !isValid(config)) {
        return undefined;
      }
      const target = ctx.input('target');
      if (target === undefined) return {};
      const targetSrc =
        config.targetIdColumn === 'id'
          ? target.source
          : target.withStages([`RENAME ${config.targetIdColumn} AS id`]);
      const prefix = config.joinType === 'LEFT' ? 'LEFT ' : '';
      const orSelf = config.orSelf ? ' OR SELF' : '';
      const orderTerms = activeOrderTerms(config).map(
        (t) => `${config.alias}.${t.column} ${t.order}`,
      );
      const orderClause =
        orderTerms.length > 0 ? ` ORDER BY ${orderTerms.join(', ')}` : '';
      const findStage =
        `${prefix}TREE FIND ${targetSrc} AS ${config.alias} ` +
        `NEAREST ${config.direction}${orSelf} ` +
        `WHERE ${config.whereExpr.trim()}${orderClause}`;
      const pickedCols = resolvedColumns(
        config,
        ctx.getInputColumns('input') ?? [],
      ).filter((c) => c.column !== '');
      const stages =
        pickedCols.length > 0
          ? [
              'AS _in',
              findStage,
              `SELECT ${treeFindSelect(config, ctx).join(', ')}`,
            ]
          : [findStage];
      return {
        stages: wrapTreeIdStages(
          config.idColumn,
          config.parentIdColumn,
          stages,
          pickedCols.length === 0,
        ),
      };
    },
  },
};
