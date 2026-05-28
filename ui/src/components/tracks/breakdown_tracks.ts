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
import {CounterTrack} from './counter_track';
import type {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import {SourceDataset} from '../../trace_processor/dataset';
import {
  type SqlValue,
  LONG,
  NUM_NULL,
  STR,
  type QueryResult,
  NUM,
} from '../../trace_processor/query_result';
import type {TrackRenderer} from '../../public/track';
import type {TrackEventDetailsPanel} from '../../public/details_panel';

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
   * The column name that uniquely identifies a slice in the slice table.
   */
  sliceIdColumn?: string;
  /**
   * Data to be pivoted. This is simlar to the debug pivot tracks where
   * the values of each column will be displayed in a separate track with
   * the corresponding slices.
   */
  pivots?: BreakdownTrackSqlInfo;
  /**
   * Whether to sort the tracks based on their maximum value.
   */
  sortTracks?: boolean;
  /**
   * Optional custom details panel for the slice tracks.
   */
  detailsPanel?: (trace: Trace) => TrackEventDetailsPanel;
  /**
   * Optional description for the root track.
   */
  description?: string;
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
  // Counter tracks defer their CREATE PERFETTO TABLE here on the legacy
  // (non-projected) path; flushed in one batched engine.query at end of
  // createTracks(). Unused on the projected path.
  private pendingTableCreates: string[] = [];

  // Projected-path state. When useProjectedPath is true, createTracks builds
  // one (id, ts, dur, k0..kN, s0..sN) projection plus one self-intersect
  // segments table, and per-track renderers query those instead of running
  // intervals_overlap_count! per node. Eliminates the O(N_tracks) macro
  // multiplier that dominates trace load for big hierarchies.
  private readonly useProjectedPath: boolean;
  private projectedTableName?: string;
  private segmentsTableName?: string;
  private projectedAggColNames?: readonly string[];
  private projectedSliceColNames?: readonly string[];

  constructor(props: BreakdownTrackProps) {
    this.props = props;
    this.uri = `/breakdown_tracks_${this.props.aggregation.tableName}`;
    this.useProjectedPath = BreakdownTracks.shouldUseProjectedPath(props);

    this.modulesClause = props.modules
      ? props.modules.map((m) => `INCLUDE PERFETTO MODULE ${m};`).join('\n')
      : '';

    if (this.props.aggregationType === BreakdownTrackAggType.COUNT) {
      this.modulesClause += this.useProjectedPath
        ? `\nINCLUDE PERFETTO MODULE intervals.intersect;`
        : `\nINCLUDE PERFETTO MODULE intervals.overlap;`;
    }

    if (this.useProjectedPath) {
      const unique = uuidv4().replace(/-/g, '_');
      this.projectedTableName = `_breakdown_projected_${unique}`;
      this.segmentsTableName = `_breakdown_segments_${unique}`;
      this.projectedAggColNames = this.props.aggregation.columns.map(
        (_, i) => `k${i}`,
      );
      this.projectedSliceColNames = (this.props.slice?.columns ?? []).map(
        (_, i) => `s${i}`,
      );
    }

    if (this.props.slice?.joins !== undefined) {
      this.sliceJoinClause = this.getJoinClause(this.props.slice.joins);
    }

    if (this.props.pivots?.joins !== undefined) {
      this.pivotJoinClause = this.getJoinClause(this.props.pivots.joins);
    }
  }

  private static shouldUseProjectedPath(props: BreakdownTrackProps): boolean {
    return (
      props.aggregationType === BreakdownTrackAggType.COUNT &&
      props.aggregation.joins === undefined &&
      props.slice?.joins === undefined &&
      props.pivots === undefined
    );
  }

  private getAggregationQuery(filtersClause: string) {
    if (this.useProjectedPath) {
      // Query the shared self-intersect segments table. One row per
      // (atomic segment, original interval) pair, plus an end-marker row at
      // each interval's end. SUM(IIF(NOT interval_ends_at_ts, 1, 0)) per
      // group_id counts active intervals at each segment; end-markers
      // contribute 0 so the counter naturally drops on segment boundaries
      // where intervals end.
      return `
        SELECT MIN(ts) AS ts,
               SUM(IIF(interval_ends_at_ts = FALSE, 1, 0)) AS value
        FROM ${this.segmentsTableName}
        ${filtersClause}
        GROUP BY group_id
        ORDER BY ts
      `;
    }

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

  // Build the two tables that drive the projected path:
  //   _breakdown_projected: (id, ts, dur, k0..kN, s0..sN) — one row per source
  //     row, with the user-supplied agg/slice column expressions pre-evaluated
  //     under stable names. Filters everywhere downstream become raw column
  //     equality on this table.
  //   _breakdown_segments: per-atomic-segment, per-active-id rows from
  //     interval_self_intersect, denormalized with the agg cols inline so
  //     per-track counter renderers don't need to JOIN back.
  private async buildProjectedTables() {
    const {tsCol, durCol, tableName} = this.props.aggregation;
    const aggCols = this.props.aggregation.columns;
    const sliceCols = this.props.slice?.columns ?? [];
    const idExpr = this.props.sliceIdColumn ?? 'ROW_NUMBER() OVER ()';

    const projectedSelect = [
      `${idExpr} AS id`,
      `${tsCol} AS ts`,
      `${durCol} AS dur`,
      ...aggCols.map((col, i) => `${col} AS k${i}`),
      ...sliceCols.map((col, i) => `${col} AS s${i}`),
    ].join(', ');

    const denormAggCols = this.projectedAggColNames!.map((n) => `p.${n}`).join(
      ', ',
    );

    await this.props.trace.engine.query(`
      CREATE PERFETTO TABLE ${this.projectedTableName} AS
      SELECT ${projectedSelect}
      FROM ${tableName};

      CREATE PERFETTO TABLE ${this.segmentsTableName} AS
      SELECT
        iss.ts,
        iss.group_id,
        iss.interval_ends_at_ts,
        ${denormAggCols}
      FROM interval_self_intersect!((
        SELECT id, ts, dur FROM ${this.projectedTableName}
      )) iss
      JOIN ${this.projectedTableName} p USING(id);
    `);
  }

  async createTracks() {
    if (this.modulesClause !== '') {
      await this.props.trace.engine.query(this.modulesClause);
    }

    if (this.useProjectedPath) {
      await this.buildProjectedTables();
    } else if (this.props.aggregationType !== BreakdownTrackAggType.COUNT) {
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

    // Column names used by the hierarchy walk to read group-by results and
    // build per-track filters. On the projected path these are the stable
    // projected names (k0..kN, s0..sN); on the legacy path they are the
    // original column expressions.
    const aggColNames = this.useProjectedPath
      ? [...this.projectedAggColNames!]
      : this.props.aggregation.columns;
    const sliceColNames = this.useProjectedPath
      ? [...this.projectedSliceColNames!]
      : this.props.slice?.columns ?? [];
    const pivotColNames = this.props.pivots?.columns ?? [];

    const allColNames = [];
    allColNames.push(...aggColNames);
    allColNames.push(...sliceColNames);
    allColNames.push(...pivotColNames);

    const query = this.useProjectedPath
      ? `
        SELECT ${allColNames.join(', ')}
        FROM ${this.projectedTableName}
        GROUP BY ${allColNames.join(', ')}
      `
      : `
        SELECT ${allColNames.join(', ')}
        FROM ${this.props.aggregation.tableName}
        ${this.sliceJoinClause ?? ''}
        ${this.pivotJoinClause ?? ''}
        GROUP BY ${allColNames.join(', ')}
      `;
    const res = await this.props.trace.engine.query(query);

    await this.createBreakdownHierarchy(
      rootTrackNode,
      res,
      aggColNames,
      sliceColNames,
      pivotColNames,
    );

    await this.flushPendingTableCreates();

    return rootTrackNode;
  }

  private async flushPendingTableCreates() {
    if (this.pendingTableCreates.length === 0) return;
    const batch = this.pendingTableCreates.join('\n');
    this.pendingTableCreates = [];
    await this.props.trace.engine.query(batch);
  }

  private async createBreakdownHierarchy(
    rootNode: TrackNode,
    queryResult: QueryResult,
    aggColumns: string[],
    sliceColumns: string[],
    pivotColumns: string[],
  ) {
    const cache: Map<string, Map<string, TrackNode>> = new Map();
    if (rootNode.uri) {
      cache.set(rootNode.uri, new Map<string, TrackNode>());
    }

    const iter = queryResult.iter({});
    for (; iter.valid(); iter.next()) {
      let state: {currentNode: TrackNode; currentFilters: Filter[]} = {
        currentNode: rootNode,
        currentFilters: [],
      };

      state = await this.processRowForColumns(
        iter,
        state,
        aggColumns,
        cache,
        (name, filters) => this.createCounterTrackNode(name, filters),
      );

      state = await this.processRowForColumns(
        iter,
        state,
        sliceColumns,
        cache,
        (name, filters, colIndex) =>
          this.createSliceTrackNode(
            name,
            filters,
            colIndex,
            this.props.slice!,
            BreakdownTrackType.SLICE,
          ),
      );

      state = await this.processRowForColumns(
        iter,
        state,
        pivotColumns,
        cache,
        (name, filters, colIndex) =>
          this.createSliceTrackNode(
            name,
            filters,
            colIndex,
            this.props.pivots!,
            BreakdownTrackType.PIVOT,
          ),
      );
    }
  }

  private async processRowForColumns(
    iter: {get: (col: string) => SqlValue},
    {
      currentNode,
      currentFilters,
    }: {currentNode: TrackNode; currentFilters: Filter[]},
    columns: string[],
    cache: Map<string, Map<string, TrackNode>>,
    createNode: (
      name: string,
      filters: Filter[],
      colIndex: number,
    ) => Promise<TrackNode>,
  ): Promise<{currentNode: TrackNode; currentFilters: Filter[]}> {
    for (let colIndex = 0; colIndex < columns.length; colIndex++) {
      let children = cache.get(currentNode.uri!);
      if (!children) {
        children = new Map<string, TrackNode>();
        cache.set(currentNode.uri!, children);
      }
      const colName = columns[colIndex];
      const colResRaw = iter.get(colName);
      const childName = colResRaw === null ? 'NULL' : colResRaw.toString();

      currentFilters.push({
        columnName: colName,
        value: childName,
      });

      let childNode = children.get(childName);
      if (!childNode) {
        childNode = await createNode(childName, currentFilters, colIndex);
        currentNode.addChildInOrder(childNode);
        children.set(childName, childNode);
      }
      currentNode = childNode;
    }
    return {currentNode, currentFilters};
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
        // On the projected path, source from the pre-computed projection:
        // id/ts/dur are already aliased there, and the slice column is
        // stored under sN, so the per-track query is a cheap raw-eq filter.
        // shouldUseProjectedPath excludes joins+pivots, so trackType is
        // always SLICE here.
        const src = this.useProjectedPath
          ? `
              SELECT id, ts, dur,
                     ${this.projectedSliceColNames![columnIndex]} AS name
              FROM ${this.projectedTableName}
              ${filtersClause}
            `
          : `
              SELECT
                ${this.props.sliceIdColumn ? this.props.sliceIdColumn : 'ROW_NUMBER() OVER()'} AS id,
                ${sqlInfo.tsCol} AS ts,
                ${sqlInfo.durCol} AS dur,
                ${sqlInfo.columns[columnIndex]} AS name
              FROM ${this.props.aggregation.tableName}
              ${joinClause}
              ${filtersClause}
            `;
        return SliceTrack.create({
          trace: this.props.trace,
          uri,
          dataset: new SourceDataset({
            schema: {
              id: NUM,
              ts: LONG,
              dur: LONG,
              name: STR,
            },
            src,
          }),
          detailsPanel: this.props.detailsPanel
            ? () => this.props.detailsPanel!(this.props.trace)
            : undefined,
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
        const sqlSource = `SELECT ts, value FROM (${this.getAggregationQuery(filtersClause)})`;
        if (this.useProjectedPath) {
          // Lazy: the per-render macro work that would dominate trace load
          // never happens here — getAggregationQuery returns a cheap filter
          // over the shared self-intersect segments table, and CounterTrack's
          // useData fires it only on render.
          return CounterTrack.create({
            trace: this.props.trace,
            uri,
            sqlSource,
          });
        }
        // Legacy: defer the CREATE PERFETTO TABLE so all counter tables in
        // this BreakdownTracks instance flush in one batched engine.query at
        // end of createTracks(). The renderer holds the chosen table name;
        // the table is materialized before createTracks() returns.
        const tableName = `_breakdown_counter_${uuidv4().replace(/-/g, '_')}`;
        this.pendingTableCreates.push(
          `CREATE PERFETTO TABLE ${tableName} AS ${sqlSource};`,
        );
        return CounterTrack.create({
          trace: this.props.trace,
          uri,
          sqlSource: tableName,
        });
      },
      (filterClause) => this.getCounterTrackSortOrder(filterClause),
    );
  }

  private async createTrackNode(
    name: string,
    filters: Filter[],
    createTrack: (uri: string, filtersClause: string) => TrackRenderer,
    getSortOrder?: (filterClause: string) => Promise<number>,
  ) {
    const filtersClause =
      filters.length > 0 ? `\nWHERE ${buildFilterSqlClause(filters)}` : '';
    const uri = `${this.uri}_${uuidv4()}`;

    const renderer = createTrack(uri, filtersClause);

    this.props.trace.tracks.registerTrack({
      uri,
      renderer,
      ...(filters.length === 0 &&
        this.props.description && {description: this.props.description}),
    });

    let sortOrder: number | undefined;
    if (this.props.sortTracks) {
      sortOrder = await getSortOrder?.(filtersClause);
    }

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
  if (input === undefined) {
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
