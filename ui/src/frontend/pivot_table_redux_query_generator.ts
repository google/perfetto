/*
 * Copyright (C) 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

import {sqliteString} from '../base/string_utils';
import {
  Area,
  PivotTableReduxQuery,
  PivotTableReduxState,
} from '../common/state';
import {toNs} from '../common/time';
import {
  getSelectedTrackIds,
} from '../controller/aggregation/slice_aggregation_controller';

import {globals} from './globals';
import {
  Aggregation,
  AggregationFunction,
  TableColumn,
  tableColumnEquals,
} from './pivot_table_redux_types';

export interface Table {
  name: string;
  columns: string[];
}

export const sliceTable = {
  name: 'slice',
  columns: ['type', 'ts', 'dur', 'category', 'name'],
};

// Columns of `slice` table available for aggregation.
export const sliceAggregationColumns = ['ts', 'dur', 'depth'];

// Columns of `thread_slice` table available for aggregation.
export const threadSliceAggregationColumns = [
  'thread_ts',
  'thread_dur',
  'thread_instruction_count',
  'thread_instruction_delta',
];

// List of available tables to query, used to populate selectors of pivot
// columns in the UI.
export const tables: Table[] = [
  sliceTable,
  {
    name: 'process',
    columns: [
      'type',
      'pid',
      'name',
      'parent_upid',
      'uid',
      'android_appid',
      'cmdline',
    ],
  },
  {name: 'thread', columns: ['type', 'name', 'tid', 'upid', 'is_main_thread']},
  {name: 'thread_track', columns: ['type', 'name', 'utid']},
];

// Queried "table column" is either:
// 1. A real one, represented as object with table and column name.
// 2. Pseudo-column 'count' that's rendered as '1' in SQL to use in queries like
// `select sum(1), name from slice group by name`.

export interface RegularColumn {
  kind: 'regular';
  table: string;
  column: string;
}

export interface ArgumentColumn {
  kind: 'argument';
  argument: string;
}

function outerAggregation(fn: AggregationFunction): AggregationFunction {
  if (fn === 'COUNT') {
    return 'SUM';
  }
  return fn;
}

// Exception thrown by query generator in case incoming parameters are not
// suitable in order to build a correct query; these are caught by the UI and
// displayed to the user.
export class QueryGeneratorError extends Error {}

// Internal column name for different rollover levels of aggregate columns.
function aggregationAlias(
    aggregationIndex: number, rolloverLevel: number): string {
  return `agg_${aggregationIndex}_level_${rolloverLevel}`;
}

export function areaFilter(area: Area): string {
  return `
    ts + dur > ${toNs(area.startSec)}
    and ts < ${toNs(area.endSec)}
    and track_id in (${getSelectedTrackIds(area).join(', ')})
  `;
}

export function expression(column: TableColumn): string {
  switch (column.kind) {
    case 'regular':
      return column.column;
    case 'argument':
      return extractArgumentExpression(column.argument);
    default:
      throw new Error(`malformed table column ${column}`);
  }
}

function aggregationExpression(aggregation: Aggregation): string {
  if (aggregation.aggregationFunction === 'COUNT') {
    return 'COUNT()';
  }
  return `${aggregation.aggregationFunction}(${
      expression(aggregation.column)})`;
}

export function extractArgumentExpression(argument: string) {
  return `extract_arg(arg_set_id, ${sqliteString(argument)})`;
}

function generateInnerQuery(
    pivots: TableColumn[],
    aggregations: Aggregation[],
    table: string,
    includeTrack: boolean,
    area: Area,
    constrainToArea: boolean): {query: string, groupByColumns: string[]} {
  const aggregationColumns: string[] = [];

  for (let i = 0; i < aggregations.length; i++) {
    aggregationColumns.push(`${aggregationExpression(aggregations[i])} as ${
        aggregationAlias(i, 0)}`);
  }

  const selectColumns: string[] = [];
  const groupByColumns: string[] = [];

  let argumentCount = 0;
  for (const column of pivots) {
    switch (column.kind) {
      case 'regular': {
        selectColumns.push(column.column);
        groupByColumns.push(column.column);
        break;
      }
      case 'argument': {
        const alias = `pivot_argument_${argumentCount++}`;
        selectColumns.push(
            `${extractArgumentExpression(column.argument)} as ${alias}`);
        groupByColumns.push(alias);
        break;
      }
      default: {
        throw new Error(`malformed table column ${column}`);
      }
    }
  }
  if (includeTrack) {
    selectColumns.push('track_id');
  }

  const query = `
    select
      ${selectColumns.concat(aggregationColumns).join(',\n')}
    from ${table}
    ${(constrainToArea ? `where ${areaFilter(area)}` : '')}
    group by ${
      groupByColumns.concat(includeTrack ? ['track_id'] : []).join(', ')}
  `;

  return {query, groupByColumns};
}

function computeSliceTableAggregations(
    selectedAggregations: Map<string, Aggregation>):
    {tableName: string, flatAggregations: Aggregation[]} {
  let hasThreadSliceColumn = false;
  const allColumns: Aggregation[] = [];
  for (const tableColumn of selectedAggregations.values()) {
    if (tableColumn.column.kind === 'regular' &&
        tableColumn.column.table === 'thread_slice') {
      hasThreadSliceColumn = true;
    }
    allColumns.push(tableColumn);
  }

  return {
    // If any aggregation column from `thread_slice` is present, it's going to
    // be the base table for the pivot table query. Otherwise, `slice` is used.
    // This later is going to be controllable by a UI element.
    tableName: hasThreadSliceColumn ? 'thread_slice' : 'slice',
    flatAggregations: allColumns,
  };
}

// Every aggregation in the request is contained in the result in (number of
// pivots + 1) times for each rollover level. This helper function returs an
// index of the necessary column in the response.
export function aggregationIndex(
    pivotColumns: number, aggregationNo: number, depth: number) {
  return pivotColumns + aggregationNo * (pivotColumns + 1) +
      (pivotColumns - depth);
}

export function generateQueryFromState(
    state: PivotTableReduxState,
    ): PivotTableReduxQuery {
  if (state.selectionArea === undefined) {
    throw new QueryGeneratorError('Should not be called without area');
  }
  return generateQuery(
      state.selectedPivots,
      state.selectedSlicePivots,
      state.selectedAggregations,
      globals.state.areas[state.selectionArea.areaId],
      state.constrainToArea);
}

export function generateQuery(
    nonSlicePivots: RegularColumn[],
    slicePivots: TableColumn[],
    selectedAggregations: Map<string, Aggregation>,
    area: Area,
    constrainToArea: boolean): PivotTableReduxQuery {
  const sliceTableAggregations =
      computeSliceTableAggregations(selectedAggregations);

  if (sliceTableAggregations.flatAggregations.length === 0) {
    throw new QueryGeneratorError('No aggregations selected');
  }

  if (slicePivots.length === 0 && nonSlicePivots.length === 0) {
    throw new QueryGeneratorError('No pivots selected');
  }

  const outerAggregations = [];
  const innerQuery = generateInnerQuery(
      slicePivots,
      sliceTableAggregations.flatAggregations,
      sliceTableAggregations.tableName,
      nonSlicePivots.length > 0,
      area,
      constrainToArea);

  const prefixedSlicePivots =
      innerQuery.groupByColumns.map((p) => `preaggregated.${p}`);
  const renderedNonSlicePivots =
      nonSlicePivots.map((pivot) => `${pivot.table}.${pivot.column}`);
  const totalPivotsArray = renderedNonSlicePivots.concat(prefixedSlicePivots);
  const sortCriteria =
      globals.state.nonSerializableState.pivotTableRedux.sortCriteria;
  const sortClauses: string[] = [];
  for (let i = 0; i < sliceTableAggregations.flatAggregations.length; i++) {
    const agg = `preaggregated.${aggregationAlias(i, 0)}`;
    const fn = outerAggregation(
        sliceTableAggregations.flatAggregations[i].aggregationFunction);
    outerAggregations.push(`${fn}(${agg}) as ${aggregationAlias(i, 0)}`);

    for (let level = 1; level < totalPivotsArray.length; level++) {
      // Peculiar form "SUM(SUM(agg)) over (partition by columns)" here means
      // following: inner SUM(agg) is an aggregation that is going to collapse
      // tracks with the same pivot values, which is going to be post-aggregated
      // by the set of columns by outer **window** SUM function.

      // Need to use complicated query syntax can be avoided by having yet
      // another nested subquery computing only aggregation values with window
      // functions in the wrapper, but the generation code is going to be more
      // complex; so complexity of the query is traded for complexity of the
      // query generator.
      outerAggregations.push(`${fn}(${fn}(${agg})) over (partition by ${
          totalPivotsArray.slice(0, totalPivotsArray.length - level)
              .join(', ')}) as ${aggregationAlias(i, level)}`);
    }

    outerAggregations.push(`${fn}(${fn}(${agg})) over () as ${
        aggregationAlias(i, totalPivotsArray.length)}`);

    if (sortCriteria !== undefined &&
        tableColumnEquals(
            sliceTableAggregations.flatAggregations[i].column,
            sortCriteria.column)) {
      for (let level = totalPivotsArray.length - 1; level >= 0; level--) {
        sortClauses.push(`${aggregationAlias(i, level)} ${sortCriteria.order}`);
      }
    }
  }

  const joins = `
    join thread_track on thread_track.id = preaggregated.track_id
    join thread using (utid)
    join process using (upid)
  `;

  const text = `
    select
      ${
      renderedNonSlicePivots.concat(prefixedSlicePivots, outerAggregations)
          .join(',\n')}
    from (
      ${innerQuery.query}
    ) preaggregated
    ${nonSlicePivots.length > 0 ? joins : ''}
    group by ${renderedNonSlicePivots.concat(prefixedSlicePivots).join(', ')}
    ${sortClauses.length > 0 ? ('order by ' + sortClauses.join(', ')) : ''}
  `;

  return {
    text,
    metadata: {
      tableName: sliceTableAggregations.tableName,
      pivotColumns: (nonSlicePivots as TableColumn[]).concat(slicePivots),
      aggregationColumns: sliceTableAggregations.flatAggregations,
    },
  };
}
