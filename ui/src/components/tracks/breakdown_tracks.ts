// Copyright (C) 2025 The Android Open Source Project
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

import {sqliteString} from '../../base/string_utils';
import {uuidv4} from '../../base/uuid';
import {SliceTrack} from './slice_track';
import {createQueryCounterTrack} from '../../components/tracks/query_counter_track';
import {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import {SourceDataset} from '../../trace_processor/dataset';
import {
  SqlValue,
  LONG,
  NUM_NULL,
  STR,
  LONG_NULL,
} from '../../trace_processor/query_result';
import {TrackRenderer} from '../../public/track';

/**
 * Aggregation types for the BreakdownTracks.
 * These aggregations will be displayed in a set of counter tracks.
 */
export enum BreakdownTrackAggType {
  COUNT = 'COUNT',
  MAX = 'MAX',
  SUM = 'SUM',
}

/**
 * Breakdown Tracks will always be shown first as
 * a counter track with the aggregation.
 *
 * Slice and pivot tracks will be slice tracks.
 */
enum BreakdownTrackType {
  AGGREGATION,
  SLICE,
  PIVOT,
}

interface BreakdownTrackSqlInfo {
  /**
   * Table columns of interest.Tracks are always filtered/evaluated
   * based on the ordering within this array.
   */
  columns: string[];
  /**
   * Table name for the data to be queried from.
   */
  tableName: string;
  /**
   * This is the value that should be displayed in the
   * aggregation counter track.
   */
  valueCol?: string;
  /**
   * ID column name. Usually this is `id` in a table but
   * it could also be something else such as `binder_txn_id`, etc.
   */
  idCol?: string;
  /**
   * Timestamp column name. Usually this is `ts` in a table but
   * it could also be something else such as `client_ts`, etc.
   */
  tsCol?: string;
  /**
   * Duration column name. Usually this is `dur` in a table
   * but it could also be something like `client_dur`, etc.
   */
  durCol?: string;
  /**
   * Optional join values for values tables that should
   * be joined to the one specified in `tableName`.
   *
   * Usage:
   *  pivots: {
        columns: [`(aidl_name || ' blocked on ' || reason)`],
        tableName: 'android_binder_txns',
        tsCol: 'ts',
        durCol: 'dur',
        joins: [
          {
            joinTableName: 'android_binder_client_server_breakdown',
            joinColumns: ['binder_txn_id'],
          },
        ],
      },
   */
  joins?: BreakdownTrackJoins[]; // To be used for joining with other tables
}

interface BreakdownTrackJoins {
  joinTableName: string;
  joinColumns: string[];
}

export interface BreakdownTrackProps {
  trace: Trace;
  /**
   * This title will display only at the top most (root) track.
   * Best practice is to include the aggregation type.
   *
   * Ex: Max RSS Usage or Binder Txn Counts
   */
  trackTitle: string;
  /**
   * This is the aggregation type used for the counter tracks
   * (described below). For example: COUNT, SUM, MAX, etc.
   */
  aggregationType: BreakdownTrackAggType;
  /**
   * Specified aggregation values are then used to populate
   * a set of counter tracks where values for each counter track
   * will be filtered to the values specified in the columns array.
   */
  aggregation: BreakdownTrackSqlInfo;
  /**
   * The Perfetto modules that should be included in order
   * to query the specified tables in the aggregation, slice, or
   * pivot tracks.
   */
  modules?: string[];
  /**
   * Data that should be displayed as slices after the aggregation
   * tracks are shown. The aggregation tracks will always display first
   * and data specified in this property will be displayed as a child
   * slice track.
   */
  slice?: BreakdownTrackSqlInfo;
  /**
   * Data to be pivoted. This is simlar to the debug pivot tracks where
   * the values of each column will be displayed in a separate track with
   * the corresponding slices.
   */
  pivots?: BreakdownTrackSqlInfo;
}

interface Filter {
  columnName: string;
  columnIndex: number; // Index in the intervals table, -1 for pivot columns
  value?: string;
}

export class BreakdownTracks {
  private readonly props;
  private uri: string;
  private modulesClause: string;
  private sliceJoinClause?: string;
  private pivotJoinClause?: string;
  private intervalsTableName: string;

  constructor(props: BreakdownTrackProps) {
    this.props = props;
    this.uri = `/breakdown_tracks_${this.props.aggregation.tableName}`;

    // Generate a unique table name for this instance's intervals
    this.intervalsTableName = `_ui_dev_perfetto_breakdown_tracks_intervals_${uuidv4().replace(/-/g, '_')}`;

    this.modulesClause = props.modules
      ? props.modules.map((m) => `INCLUDE PERFETTO MODULE ${m};`).join('\n')
      : '';

    if (this.props.aggregationType === BreakdownTrackAggType.COUNT) {
      this.modulesClause += `\nINCLUDE PERFETTO MODULE intervals.intersect;`;
    }

    if (this.props.slice?.joins !== undefined) {
      this.sliceJoinClause = this.getJoinClause(this.props.slice.joins);
    }

    if (this.props.pivots?.joins !== undefined) {
      this.pivotJoinClause = this.getJoinClause(this.props.pivots.joins);
    }
  }

  private getAggregationQuery(filtersClause: string) {
    const {valueCol} = this.props.aggregation;

    if (this.props.aggregationType === BreakdownTrackAggType.COUNT) {
      return `
        SELECT ii.ts, ii.dur, COUNT(*) AS value
        FROM ${this.intervalsTableName} ii
        ${filtersClause}
        GROUP BY ii.group_id
      `;
    }

    return `
      SELECT
        ii.ts,
        ii.dur,
        ${this.props.aggregationType}(ii.${valueCol}) AS value
      FROM ${this.intervalsTableName} ii
      ${filtersClause}
      GROUP BY ii.group_id
    `;
  }

  private getIntervals() {
    const {
      tsCol,
      durCol,
      tableName,
      idCol = 'id',
      valueCol,
      columns,
    } = this.props.aggregation;

    // Collect all columns that need to be included from the source table
    const allColumns: string[] = [];

    // Add aggregation columns (these can be expressions)
    allColumns.push(...columns);

    // Add value column if present
    if (valueCol) {
      allColumns.push(valueCol);
    }

    // Add slice columns if present
    if (this.props.slice) {
      allColumns.push(...this.props.slice.columns);
    }

    // Build the column list - columns can be expressions, so we need to alias them
    // Generate aliases like col_0, col_1, etc. for each column/expression
    const columnSelects = allColumns
      .map((col, idx) => {
        // If it's a simple column name (no parentheses or operators), use src.col
        // Otherwise, it's an expression that should be evaluated as-is
        const isSimpleColumn = /^[a-zA-Z_][a-zA-Z0-9_]*$/.test(col);
        const expr = isSimpleColumn ? `src.${col}` : col;
        return `${expr} AS col_${idx}`;
      })
      .join(',\n        ');

    // For pivot tracks, we need to include the join key columns
    let joinKeyColumns = '';
    if (this.props.pivots?.joins) {
      const joinKeys = this.props.pivots.joins.flatMap(j => j.joinColumns);
      joinKeyColumns = joinKeys.map(key => `,\n        src.${key}`).join('');
    }

    return `
      CREATE OR REPLACE PERFETTO TABLE ${this.intervalsTableName}
      AS
      SELECT
        ii.ts,
        ii.dur,
        ii.group_id,
        ii.id,
        ${columnSelects}${joinKeyColumns}
      FROM interval_self_intersect!(
        (SELECT ${idCol} AS id, ${tsCol} AS ts, ${durCol} AS dur
         FROM ${tableName}
         WHERE ${durCol} > -1 AND ${idCol} IS NOT NULL)
      ) ii
      JOIN ${tableName} src ON src.${idCol} = ii.id
      WHERE ii.interval_ends_at_ts = FALSE
    `;
  }

  private getJoinClause(joins: BreakdownTrackJoins[]) {
    return joins
      .map(
        ({joinTableName, joinColumns}) =>
          `JOIN ${joinTableName} USING(${joinColumns.join(', ')})`,
      )
      .join('\n');
  }

  async createTracks() {
    if (this.modulesClause !== '') {
      await this.props.trace.engine.query(this.modulesClause);
    }

    // Precompute all intersections once using the fast interval_self_intersect
    await this.props.trace.engine.query(this.getIntervals());

    const rootTrackNode = await this.createCounterTrackNode(
      `${this.props.trackTitle}`,
      [],
    );

    this.createBreakdownHierarchy(
      [],
      rootTrackNode,
      this.props.aggregation,
      0,
      BreakdownTrackType.AGGREGATION,
    );

    return rootTrackNode;
  }

  private async createBreakdownHierarchy(
    filters: Filter[],
    parent: TrackNode,
    sqlInfo: BreakdownTrackSqlInfo,
    colIndex: number,
    trackType: BreakdownTrackType,
  ) {
    const {columns} = sqlInfo;
    if (colIndex === columns.length) {
      return;
    }

    const currColName = columns[colIndex];
    const joinClause = this.getTrackSpecificJoinClause(trackType);

    // For aggregation and slice tracks, use the intervals table
    // For pivot tracks, we need to join with the pivot table
    const fromClause =
      trackType === BreakdownTrackType.PIVOT && joinClause
        ? `${this.intervalsTableName} ii\n      ${joinClause}`
        : `${this.intervalsTableName} ii`;

    // Map the column to its alias in the intervals table
    let columnRef: string;
    let resultColumnName: string;
    
    if (trackType === BreakdownTrackType.PIVOT && this.props.pivots) {
      // For pivot tracks, the column might be in the joined table
      columnRef = currColName;
      resultColumnName = currColName;
    } else {
      // For aggregation/slice tracks, find the column index and use the alias
      const allColumns = this.getAllColumns();
      const colIdx = allColumns.indexOf(currColName);
      columnRef = `ii.col_${colIdx}`;
      resultColumnName = `col_${colIdx}`;
    }

    const query = `
      ${this.modulesClause}

      SELECT DISTINCT ${columnRef}
      FROM ${fromClause}
      ${filters.length > 0 ? `WHERE ${buildFilterSqlClause(filters)}` : ''}
    `;

    const res = await this.props.trace.engine.query(query);

    for (const iter = res.iter({}); iter.valid(); iter.next()) {
      const colRaw = iter.get(resultColumnName);
      const colValue = colRaw === null ? 'NULL' : colRaw.toString();
      const name = colValue;

      const newFilters: Filter[] = [
        ...filters,
        {
          columnName: currColName,
          columnIndex: trackType === BreakdownTrackType.PIVOT ? -1 : this.getAllColumns().indexOf(currColName),
          value: colValue,
        },
      ];

      let currNode;
      let nextTrackType = trackType;
      let nextColIndex = colIndex + 1;
      let nextSqlInfo = sqlInfo;

      switch (trackType) {
        case BreakdownTrackType.AGGREGATION:
          currNode = await this.createCounterTrackNode(name, newFilters);
          if (this.props.slice && colIndex === columns.length - 1) {
            nextTrackType = BreakdownTrackType.SLICE;
            nextColIndex = 0;
            nextSqlInfo = this.props.slice;
          }
          break;
        case BreakdownTrackType.SLICE:
          currNode = await this.createSliceTrackNode(
            name,
            newFilters,
            colIndex,
            sqlInfo,
            trackType,
          );
          if (this.props.pivots && colIndex === columns.length - 1) {
            nextTrackType = BreakdownTrackType.PIVOT;
            nextColIndex = 0;
            nextSqlInfo = this.props.pivots;
          }
          break;
        default:
          currNode = await this.createSliceTrackNode(
            name,
            newFilters,
            colIndex,
            sqlInfo,
            trackType,
          );
      }

      parent.addChildInOrder(currNode);
      this.createBreakdownHierarchy(
        newFilters,
        currNode,
        nextSqlInfo,
        nextColIndex,
        nextTrackType,
      );
    }
  }

  private getTrackSpecificJoinClause(trackType: BreakdownTrackType) {
    switch (trackType) {
      case BreakdownTrackType.SLICE:
        return this.sliceJoinClause;
      case BreakdownTrackType.PIVOT:
        return this.pivotJoinClause;
      default:
        return undefined;
    }
  }

  private async createSliceTrackNode(
    title: string,
    newFilters: Filter[],
    columnIndex: number,
    sqlInfo: BreakdownTrackSqlInfo,
    trackType: BreakdownTrackType,
  ) {
    let joinClause = '';

    if (this.sliceJoinClause && trackType === BreakdownTrackType.SLICE) {
      joinClause = this.sliceJoinClause;
    } else if (this.pivotJoinClause && trackType === BreakdownTrackType.PIVOT) {
      joinClause = this.pivotJoinClause;
    }

    return await this.createTrackNode(
      title,
      newFilters,
      (uri: string, _filtersClause: string) => {
        // For slice tracks, we need to convert the filter clause to use original column names
        // instead of ii.col_N references
        const sliceFiltersClause = newFilters.length > 0
          ? `\nWHERE ${buildSliceFilterSqlClause(newFilters)}`
          : '';
        
        return SliceTrack.createMaterialized({
          trace: this.props.trace,
          uri,
          dataset: new SourceDataset({
            schema: {
              ts: LONG,
              dur: LONG_NULL,
              name: STR,
            },
            src: `
              SELECT
                ${sqlInfo.tsCol} AS ts,
                ${sqlInfo.durCol} AS dur,
                ${sqlInfo.columns[columnIndex]} AS name
              FROM ${this.props.aggregation.tableName}
              ${joinClause}
              ${sliceFiltersClause}
            `,
          }),
        });
      },
    );
  }

  private async getCounterTrackSortOrder(filtersClause: string) {
    const aggregationQuery = this.getAggregationQuery(filtersClause);
    const result = await this.props.trace.engine.query(`
      SELECT MAX(value) as max_value FROM (${aggregationQuery})
    `);
    const maxValue = result.firstRow({max_value: NUM_NULL}).max_value;
    return maxValue === null ? 0 : maxValue;
  }

  private async createCounterTrackNode(name: string, newFilters: Filter[]) {
    return await this.createTrackNode(
      name,
      newFilters,
      (uri: string, filtersClause: string) => {
        return createQueryCounterTrack({
          trace: this.props.trace,
          uri,
          data: {
            sqlSource: `
              SELECT ts, value FROM
              (${this.getAggregationQuery(filtersClause)})
            `,
          },
          columns: {
            ts: 'ts',
            value: 'value',
          },
        });
      },
      (filterClause) => this.getCounterTrackSortOrder(filterClause),
    );
  }

  private async createTrackNode(
    name: string,
    filters: Filter[],
    createTrack: (uri: string, filtersClause: string) => Promise<TrackRenderer>,
    getSortOrder?: (filterClause: string) => Promise<number>,
  ) {
    const filtersClause =
      filters.length > 0 ? `\nWHERE ${buildFilterSqlClause(filters)}` : '';
    const uri = `${this.uri}_${uuidv4()}`;

    const renderer = await createTrack(uri, filtersClause);

    this.props.trace.tracks.registerTrack({
      uri,
      renderer,
    });

    const sortOrder = await getSortOrder?.(filtersClause);

    return new TrackNode({
      name,
      uri,
      sortOrder: sortOrder !== undefined ? -sortOrder : undefined,
    });
  }

  // Helper to get all columns in the same order as getIntervals()
  private getAllColumns(): string[] {
    const allColumns: string[] = [];
    allColumns.push(...this.props.aggregation.columns);
    if (this.props.aggregation.valueCol) {
      allColumns.push(this.props.aggregation.valueCol);
    }
    if (this.props.slice) {
      allColumns.push(...this.props.slice.columns);
    }
    return allColumns;
  }
}

function buildFilterSqlClause(filters: Filter[]) {
  return filters.map((filter) => filterToSql(filter)).join(' AND ');
}

function filterToSql(filter: Filter) {
  const {columnName, columnIndex, value} = filter;
  const filterValue: SqlValue | undefined = toSqlValue(value);
  
  // For pivot columns (columnIndex === -1), use the original column name
  // For aggregation/slice columns, use the col_N alias
  const colRef = columnIndex === -1 ? columnName : `ii.col_${columnIndex}`;
  return `${colRef} = ${filterValue === undefined ? '' : filterValue}`;
}

function buildSliceFilterSqlClause(filters: Filter[]) {
  return filters.map((filter) => {
    const {columnName, value} = filter;
    const filterValue: SqlValue | undefined = toSqlValue(value);
    // For slice tracks, use the original column expression from aggregation
    return `${columnName} = ${filterValue === undefined ? '' : filterValue}`;
  }).join(' AND ');
}

function toSqlValue(input: string | undefined): string | number | bigint {
  if (input === undefined || !input.trim()) {
    return '';
  }

  const num = Number(input);
  if (!isNaN(num) && String(num) == input.trim()) {
    return num;
  }

  try {
    return BigInt(input);
  } catch {
    return sqliteString(input);
  }
}
