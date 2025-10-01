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
  value?: string;
}

export class BreakdownTracks {
  private readonly props;
  private uri: string;
  private modulesClause: string;
  private sliceJoinClause?: string;
  private pivotJoinClause?: string;

  constructor(props: BreakdownTrackProps) {
    this.props = props;
    this.uri = `/breakdown_tracks_${this.props.aggregation.tableName}`;

    this.modulesClause = props.modules
      ? props.modules.map((m) => `INCLUDE PERFETTO MODULE ${m};`).join('\n')
      : '';

    if (this.props.aggregationType === BreakdownTrackAggType.COUNT) {
      this.modulesClause += `\nINCLUDE PERFETTO MODULE intervals.overlap;`;
    }

    if (this.props.slice?.joins !== undefined) {
      this.sliceJoinClause = this.getJoinClause(this.props.slice.joins);
    }

    if (this.props.pivots?.joins !== undefined) {
      this.pivotJoinClause = this.getJoinClause(this.props.pivots.joins);
    }
  }

  private getAggregationQuery(filtersClause: string) {
    if (this.props.aggregationType === BreakdownTrackAggType.COUNT) {
      return `
        intervals_overlap_count
        !((
            SELECT ${this.props.aggregation.tsCol} AS ts,
            ${this.props.aggregation.durCol} AS dur
            FROM ${this.props.aggregation.tableName}
            ${filtersClause}
        ), ts, dur)
      `;
    }

    return `
      SELECT
      ${this.props.aggregation.tsCol} AS ts,
      ${this.props.aggregation.durCol} dur,
      ${this.props.aggregationType}(${this.props.aggregation.valueCol}) AS value
      FROM _ui_dev_perfetto_breakdown_tracks_intervals
      ${filtersClause}
      GROUP BY ${this.props.aggregation.tsCol}
    `;
  }

  // TODO: Modify this to use self_interval_intersect when it is available.
  private getIntervals() {
    const {tsCol, durCol, valueCol, columns, tableName} =
      this.props.aggregation;

    return `
      CREATE OR REPLACE PERFETTO TABLE _ui_dev_perfetto_breakdown_tracks_intervals
      AS
      WITH
        x AS (
          SELECT overlap.*,
          lead(${tsCol}) OVER (PARTITION BY group_name ORDER BY ${tsCol}) - ${tsCol} AS dur
          FROM intervals_overlap_count_by_group!(${tableName}, ${tsCol}, ${durCol}, ${columns[columns.length - 1]}) overlap
        )
      SELECT x.ts, x.dur,
        ${columns.map((col) => `${tableName}.${col}`).join(', ')},
        ${tableName}.${valueCol}
      FROM x
      JOIN ${tableName}
        ON
          ${tableName}.${columns[columns.length - 1]} = x.group_name
          AND _ui_dev_perfetto_breakdown_tracks_is_spans_overlapping(x.ts, x.ts + x.dur, ${tableName}.${tsCol}, ${tableName}.${tsCol} + ${tableName}.${durCol});
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

    if (this.props.aggregationType !== BreakdownTrackAggType.COUNT) {
      await this.props.trace.engine.query(`
        CREATE OR REPLACE PERFETTO FUNCTION _ui_dev_perfetto_breakdown_tracks_is_spans_overlapping(
          ts1 LONG,
          ts_end1 LONG,
          ts2 LONG,
          ts_end2 LONG)
        RETURNS BOOL
        AS
        SELECT (IIF($ts1 < $ts2, $ts2, $ts1) < IIF($ts_end1 < $ts_end2, $ts_end1, $ts_end2));

        ${this.getIntervals()}
      `);
    }

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

    const query = `
      ${this.modulesClause}

      SELECT DISTINCT ${currColName}
      FROM ${this.props.aggregation.tableName}
      ${joinClause !== undefined ? joinClause : ''}
      ${filters.length > 0 ? `WHERE ${buildFilterSqlClause(filters)}` : ''}
    `;

    const res = await this.props.trace.engine.query(query);

    for (const iter = res.iter({}); iter.valid(); iter.next()) {
      const colRaw = iter.get(currColName);
      const colValue = colRaw === null ? 'NULL' : colRaw.toString();
      const name = colValue;

      const newFilters = [
        ...filters,
        {
          columnName: currColName,
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
      (uri: string, filtersClause: string) => {
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
              ${filtersClause}
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
}

function buildFilterSqlClause(filters: Filter[]) {
  return filters.map((filter) => `${filterToSql(filter)}`).join(' AND ');
}

function filterToSql(filter: Filter) {
  const {columnName, value} = filter;

  const filterValue: SqlValue | undefined = toSqlValue(value);
  return `${columnName} = ${filterValue === undefined ? '' : filterValue}`;
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
