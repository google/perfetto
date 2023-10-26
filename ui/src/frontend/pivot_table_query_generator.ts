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
  PivotTableQuery,
  PivotTableState,
} from '../common/state';
import {
  getSelectedTrackKeys,
} from '../controller/aggregation/slice_aggregation_controller';

import {globals} from './globals';
import {
  Aggregation,
  TableColumn,
} from './pivot_table_types';
import {SqlTables} from './sql_table/well_known_tables';

export interface Table {
  name: string;
  displayName: string;
  columns: string[];
}

export const sliceTable = {
  name: SqlTables.slice.name,
  displayName: 'slice',
  columns: [
    'type',
    'ts',
    'dur',
    'category',
    'name',
    'depth',
    'pid',
    'process_name',
    'tid',
    'thread_name',
  ],
};

// Columns of `slice` table available for aggregation.
export const sliceAggregationColumns = [
  'ts',
  'dur',
  'depth',
  'thread_ts',
  'thread_dur',
  'thread_instruction_count',
  'thread_instruction_delta',
];

// List of available tables to query, used to populate selectors of pivot
// columns in the UI.
export const tables: Table[] = [
  sliceTable,
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

// Exception thrown by query generator in case incoming parameters are not
// suitable in order to build a correct query; these are caught by the UI and
// displayed to the user.
export class QueryGeneratorError extends Error {}

// Internal column name for different rollover levels of aggregate columns.
function aggregationAlias(aggregationIndex: number): string {
  return `agg_${aggregationIndex}`;
}

export function areaFilters(area: Area): string[] {
  return [
    `ts + dur > ${area.start}`,
    `ts < ${area.end}`,
    `track_id in (${getSelectedTrackKeys(area).join(', ')})`,
  ];
}

export function expression(column: TableColumn): string {
  switch (column.kind) {
    case 'regular':
      return `${column.table}.${column.column}`;
    case 'argument':
      return extractArgumentExpression(column.argument, SqlTables.slice.name);
  }
}

function aggregationExpression(aggregation: Aggregation): string {
  if (aggregation.aggregationFunction === 'COUNT') {
    return 'COUNT()';
  }
  return `${aggregation.aggregationFunction}(${
      expression(aggregation.column)})`;
}

export function extractArgumentExpression(argument: string, table?: string) {
  const prefix = table === undefined ? '' : `${table}.`;
  return `extract_arg(${prefix}arg_set_id, ${sqliteString(argument)})`;
}

export function aggregationIndex(pivotColumns: number, aggregationNo: number) {
  return pivotColumns + aggregationNo;
}

export function generateQueryFromState(state: PivotTableState):
    PivotTableQuery {
  if (state.selectionArea === undefined) {
    throw new QueryGeneratorError('Should not be called without area');
  }

  const sliceTableAggregations = [...state.selectedAggregations.values()];
  if (sliceTableAggregations.length === 0) {
    throw new QueryGeneratorError('No aggregations selected');
  }

  const pivots = state.selectedPivots;

  const aggregations = sliceTableAggregations.map(
      (agg, index) =>
          `${aggregationExpression(agg)} as ${aggregationAlias(index)}`);
  const countIndex = aggregations.length;
  // Extra count aggregation, needed in order to compute combined averages.
  aggregations.push('COUNT() as hidden_count');

  const renderedPivots = pivots.map(expression);
  const sortClauses: string[] = [];
  for (let i = 0; i < sliceTableAggregations.length; i++) {
    const sortDirection = sliceTableAggregations[i].sortDirection;
    if (sortDirection !== undefined) {
      sortClauses.push(`${aggregationAlias(i)} ${sortDirection}`);
    }
  }

  const whereClause = state.constrainToArea ?
      `where ${
          areaFilters(globals.state.areas[state.selectionArea.areaId])
              .join(' and\n')}` :
      '';
  const text = `
    INCLUDE PERFETTO MODULE experimental.slices;

    select
      ${renderedPivots.concat(aggregations).join(',\n')}
    from ${SqlTables.slice.name}
    ${whereClause}
    group by ${renderedPivots.join(', ')}
    ${sortClauses.length > 0 ? ('order by ' + sortClauses.join(', ')) : ''}
  `;

  return {
    text,
    metadata: {
      pivotColumns: pivots,
      aggregationColumns: sliceTableAggregations,
      countIndex,
    },
  };
}
