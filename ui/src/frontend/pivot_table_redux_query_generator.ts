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

export interface CountColumn {
  kind: 'count';
}

export interface RegularColumn {
  kind: 'regular';
  table: string;
  column: string;
}

export interface ArgumentColumn {
  kind: 'argument';
  argument: string;
}

export type TableColumn = CountColumn|RegularColumn|ArgumentColumn;

export function tableColumnEquals(first: TableColumn, second: TableColumn) {
  switch (first.kind) {
    case 'count': {
      return second.kind === 'count';
    }
    case 'regular': {
      return second.kind === 'regular' && first.table === second.table &&
          first.column === second.column;
    }
    case 'argument': {
      return second.kind === 'argument' && first.argument === second.argument;
    }
    default: {
      throw new Error(`malformed table column ${first}`);
    }
  }
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
    ts > ${toNs(area.startSec)}
    and ts < ${toNs(area.endSec)}
    and track_id in (${getSelectedTrackIds(area).join(', ')})
  `;
}

function aggregationExpression(aggregation: TableColumn): string {
  switch (aggregation.kind) {
    case 'count': {
      return 'COUNT()';
    }
    case 'regular': {
      return `SUM(${aggregation.column})`;
    }
    case 'argument': {
      return `SUM(${extractArgumentExpression(aggregation.argument)})`;
    }
    default: {
      throw new Error(`malformed table column ${aggregation}`);
    }
  }
}

export function extractArgumentExpression(argument: string) {
  return `extract_arg(arg_set_id, ${sqliteString(argument)})`;
}

function generateInnerQuery(
    pivots: TableColumn[],
    aggregations: TableColumn[],
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
      case 'count': {
        throw new Error(`count should not be a pivot!`);
      }
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
    selectedAggregations: Map<string, TableColumn>):
    {tableName: string, flatAggregations: TableColumn[]} {
  let hasThreadSliceColumn = false;
  const allColumns: TableColumn[] = [];
  for (const tableColumn of selectedAggregations.values()) {
    if (tableColumn.kind === 'regular' &&
        tableColumn.table === 'thread_slice') {
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
  if (state.selectionArea === null) {
    throw new QueryGeneratorError('Should not be called without area');
  }
  return generateQuery(
      state.selectedPivotsMap,
      state.selectedAggregations,
      globals.state.areas[state.selectionArea.areaId],
      state.constrainToArea);
}

export function generateQuery(
    selectedPivots: Map<string, TableColumn>,
    selectedAggregations: Map<string, TableColumn>,
    area: Area,
    constrainToArea: boolean): PivotTableReduxQuery {
  const sliceTableAggregations =
      computeSliceTableAggregations(selectedAggregations);
  const slicePivots: TableColumn[] = [];
  const nonSlicePivots: RegularColumn[] = [];

  if (sliceTableAggregations.flatAggregations.length === 0) {
    throw new QueryGeneratorError('No aggregations selected');
  }

  for (const tableColumn of selectedPivots.values()) {
    switch (tableColumn.kind) {
      case 'count': {
        throw new Error('count can not be used as a pivot');
      }
      case 'regular': {
        if (tableColumn.table === 'slice' ||
            tableColumn.table === 'thread_slice') {
          slicePivots.push(tableColumn);
        } else {
          nonSlicePivots.push(tableColumn);
        }
        break;
      }
      case 'argument': {
        slicePivots.push(tableColumn);
        break;
      }
      default: {
        throw new Error(`malformed table column ${tableColumn}`);
      }
    }
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
    outerAggregations.push(`SUM(${agg}) as ${aggregationAlias(i, 0)}`);

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
      outerAggregations.push(`SUM(SUM(${agg})) over (partition by ${
          totalPivotsArray.slice(0, totalPivotsArray.length - level)
              .join(', ')}) as ${aggregationAlias(i, level)}`);
    }

    outerAggregations.push(`SUM(SUM(${agg})) over () as ${
        aggregationAlias(i, totalPivotsArray.length)}`);

    if (sortCriteria !== undefined &&
        tableColumnEquals(
            sliceTableAggregations.flatAggregations[i], sortCriteria.column)) {
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
