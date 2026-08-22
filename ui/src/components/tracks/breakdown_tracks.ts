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

import {assertUnreachable} from '../../base/assert';
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
   * aggregation counter track. Required for MAX / SUM aggregation; ignored for
   * COUNT.
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

/**
 * BreakdownTracks builds a hierarchy of tracks from a single interval table:
 *
 *   aggregation columns  -> counter tracks   (overlap COUNT / MAX / SUM)
 *   slice columns        -> slice tracks
 *   pivot columns        -> slice tracks (sourced via an optional join)
 *
 * All counters are driven by a small set of per-level segments tables built
 * once per instance by the C++ `_interval_self_intersect_count` /
 * `_interval_self_intersect_agg` macros: the table for hierarchy depth d
 * partitions the intervals by (k0..k{d-1}) and carries the COUNT / SUM / MAX
 * over each partition's atomic overlap segments, pre-aggregated during the
 * sweep. A node at depth d pins all of its level's partition columns, so its
 * counter query is a plain filter over its own partition's rows — no JOIN, no
 * GROUP BY, and an output size bounded by 2x the interval count per level
 * regardless of overlap depth. Counter renderers stay lazy
 * (CounterTrack.create), so render work scales with what the user views.
 */
export class BreakdownTracks {
  private readonly props: BreakdownTrackProps;
  private readonly uri: string;

  // Precomputed SQL fragments, assembled once in the constructor: the
  // `INCLUDE PERFETTO MODULE` preamble, and the LEFT JOIN clauses attaching the
  // slice / pivot tables to the projection (each empty when not configured).
  private readonly modulesClause: string;
  private readonly sliceJoinClause: string;
  private readonly pivotJoinClause: string;

  // The tables built once per BreakdownTracks instance (see buildTables);
  // each name gets a UUID suffix so independent grids — e.g. the binder server
  // and client trees — never collide on a table name:
  //   intervals       one row per source interval (no joins).
  //   segments (per level d in 0..aggColNames.length)  pre-aggregated atomic
  //                   overlap segments partitioned by (k0..k{d-1}); see
  //                   segmentsTable().
  //   projected       source rows + slice/pivot joins (may fan out 1:N) +
  //                   ts/dur.
  private readonly intervalsTableName: string;
  private readonly segmentsTableBaseName: string;
  private readonly projectedTableName: string;

  // The breakdown / slice / pivot columns the caller supplies are arbitrary SQL
  // *expressions* (e.g. `IFNULL(interface, 'unknown')`). We evaluate each once
  // when building the tables and alias it to a stable plain column name, so
  // every later GROUP BY / filter is a cheap raw-column reference (k0 = 'x')
  // rather than re-evaluating the whole expression each time. k* are the
  // breakdown (aggregation) columns, s* the slice columns, p* the pivot columns.
  private readonly aggColNames: readonly string[]; // k0..kN
  private readonly sliceColNames: readonly string[]; // s0..sM
  private readonly pivotColNames: readonly string[]; // p0..pP

  constructor(props: BreakdownTrackProps) {
    if (
      props.aggregationType !== BreakdownTrackAggType.COUNT &&
      props.aggregation.valueCol === undefined
    ) {
      throw new Error(
        `BreakdownTracks: aggregation.valueCol is required for ` +
          `${props.aggregationType} aggregation`,
      );
    }

    this.props = props;
    this.uri = `/breakdown_tracks_${props.aggregation.tableName}`;

    const unique = uuidv4().replace(/-/g, '_');
    this.intervalsTableName = `_breakdown_intervals_${unique}`;
    this.segmentsTableBaseName = `_breakdown_segments_${unique}`;
    this.projectedTableName = `_breakdown_projected_${unique}`;

    this.aggColNames = props.aggregation.columns.map((_, i) => `k${i}`);
    this.sliceColNames = (props.slice?.columns ?? []).map((_, i) => `s${i}`);
    this.pivotColNames = (props.pivots?.columns ?? []).map((_, i) => `p${i}`);

    this.modulesClause = [
      ...(props.modules ?? []).map((m) => `INCLUDE PERFETTO MODULE ${m};`),
      'INCLUDE PERFETTO MODULE intervals.self_intersect;',
    ].join('\n');

    this.sliceJoinClause = props.slice?.joins
      ? this.getJoinClause(props.slice.joins)
      : '';
    this.pivotJoinClause = props.pivots?.joins
      ? this.getJoinClause(props.pivots.joins)
      : '';
  }

  // The segments table serving hierarchy depth `depth`: its rows are the
  // atomic overlap segments of the intervals partitioned by the first `depth`
  // breakdown columns, with cnt / sum_value / max_value pre-aggregated over
  // each segment's active intervals by the C++ sweep. Depth 0 (the root) is a
  // single unpartitioned series.
  private segmentsTable(depth: number): string {
    return `${this.segmentsTableBaseName}_l${depth}`;
  }

  // The pre-aggregated column matching this instance's aggregation type.
  // Segments with no active interval carry cnt = 0 / sum_value = 0 /
  // max_value = NULL, so the counter naturally drops where cover ends (MAX
  // uses NULL rather than a sentinel like 0 because a numeric sentinel could
  // wrongly render above genuinely low values, e.g. when all are negative).
  private aggValueColumn(): string {
    switch (this.props.aggregationType) {
      case BreakdownTrackAggType.MAX:
        return 'max_value';
      case BreakdownTrackAggType.SUM:
        return 'sum_value';
      case BreakdownTrackAggType.COUNT:
        return 'cnt';
    }
    assertUnreachable(this.props.aggregationType);
  }

  private getAggregationQuery(filtersClause: string, depth: number): string {
    // A node at depth `depth` pins every partition column of its level's
    // segments table (k0..k{depth-1}), so the filter selects exactly one
    // partition's pre-aggregated rows. Filters are raw equality on the
    // pre-projected k* columns.
    return `
      SELECT ts, ${this.aggValueColumn()} AS value
      FROM ${this.segmentsTable(depth)}
      ${filtersClause}
      ORDER BY ts
    `;
  }

  private getJoinClause(joins: BreakdownTrackJoins[]): string {
    // LEFT join so source rows with no join match (e.g. a binder txn with no
    // breakdown row) are kept: the counter (sourced from the un-joined
    // intervals table) and the slice tracks must still see them. A pivot track
    // filters on its own column value, so the NULL-valued unmatched rows are
    // naturally excluded from pivots.
    return joins
      .map(
        ({joinTableName, joinColumns}) =>
          `LEFT JOIN ${joinTableName} USING(${joinColumns.join(', ')})`,
      )
      .join('\n');
  }

  // Projects a column group's (slice or pivot) ts/dur plus its column
  // expressions into stable names (e.g. slice_ts, slice_dur, s0..). Empty when
  // the column group is absent.
  private projectColumnGroup(
    info: BreakdownTrackSqlInfo | undefined,
    tsAlias: string,
    colPrefix: string,
  ): string[] {
    if (info === undefined) return [];
    return [
      `${info.tsCol ?? 'ts'} AS ${tsAlias}_ts`,
      `${info.durCol ?? 'dur'} AS ${tsAlias}_dur`,
      ...info.columns.map((col, i) => `${col} AS ${colPrefix}${i}`),
    ];
  }

  // Builds the tables that drive every track:
  //   _breakdown_intervals: one row per source interval (ts, dur, the
  //     breakdown columns, [agg_value]) from the aggregation table ONLY — no
  //     slice/pivot joins, so the overlap count is never inflated by a 1:N join.
  //   _breakdown_segments_l<d> (one per hierarchy depth d): pre-aggregated
  //     atomic overlap segments from _interval_self_intersect_count/_agg over
  //     the intervals, partitioned by (k0..k{d-1}). Each table is at most
  //     ~2x the interval count regardless of overlap depth, and per-node
  //     counter queries are a plain filter — no JOIN back, no GROUP BY.
  //   _breakdown_projected: one row per source row WITH the slice/pivot joins
  //     applied (so it may fan out 1:N) plus the slice/pivot ts/dur. Drives the
  //     hierarchy enumeration and the slice/pivot tracks.
  private async buildTables(): Promise<void> {
    const agg = this.props.aggregation;
    const aggTs = agg.tsCol ?? 'ts';
    const aggDur = agg.durCol ?? 'dur';
    const hasValue = agg.valueCol !== undefined;

    const intervalCols = [
      `${aggTs} AS ts`,
      `${aggDur} AS dur`,
      ...agg.columns.map((col, i) => `${col} AS k${i}`),
      ...(hasValue ? [`${agg.valueCol} AS agg_value`] : []),
    ].join(', ');

    // Drop intervals that can't carry a count: a NULL id (e.g. binder_reply_id
    // on oneway transactions) or a negative dur (dur = -1 marks an incomplete
    // slice). The id itself is not projected — the self-intersect macros
    // treat every row as one interval — so the check reads the raw column.
    const idCheck = this.props.sliceIdColumn
      ? `${this.props.sliceIdColumn} IS NOT NULL AND `
      : '';

    // One segments table per hierarchy depth: depth d partitions by the
    // first d breakdown columns (depth 0 = the unpartitioned root series).
    // The COUNT flavor skips the value plumbing entirely; MAX/SUM ship
    // agg_value through the sweep.
    const segmentTables = this.aggColNames.map((_, i) => i + 1);
    segmentTables.unshift(0);
    const segmentsDdl = segmentTables
      .map((depth) => {
        const keys = this.aggColNames.slice(0, depth).join(', ');
        const keyCols = keys.length > 0 ? `, ${keys}` : '';
        const src =
          this.props.aggregationType === BreakdownTrackAggType.COUNT
            ? `_interval_self_intersect_count!((
                 SELECT ts, dur${keyCols} FROM ${this.intervalsTableName}
               ), (${keys}))`
            : `_interval_self_intersect_agg!((
                 SELECT ts, dur, agg_value${keyCols}
                 FROM ${this.intervalsTableName}
               ), agg_value, (${keys}))`;
        return `
          CREATE PERFETTO TABLE ${this.segmentsTable(depth)} AS
          SELECT * FROM ${src};`;
      })
      .join('\n');

    // The projection applies the slice/pivot joins, so a plain id column would
    // be ambiguous if a join table shares the name — e.g. the binder breakdown
    // tables also carry binder_reply_id, which the client perspective uses as
    // its id. Qualify it with the base table. The ROW_NUMBER fallback needs no
    // qualification and can't be ambiguous. (The intervals table has no joins,
    // so it keeps the unqualified idExpr.)
    const projectedIdExpr = this.props.sliceIdColumn
      ? `${agg.tableName}.${this.props.sliceIdColumn}`
      : 'ROW_NUMBER() OVER ()';

    const projectedCols = [
      `${projectedIdExpr} AS id`,
      ...agg.columns.map((col, i) => `${col} AS k${i}`),
      ...this.projectColumnGroup(this.props.slice, 'slice', 's'),
      ...this.projectColumnGroup(this.props.pivots, 'pivot', 'p'),
    ].join(', ');

    await this.props.trace.engine.query(`
      CREATE PERFETTO TABLE ${this.intervalsTableName} AS
      SELECT ${intervalCols}
      FROM ${agg.tableName}
      WHERE ${idCheck}${aggTs} IS NOT NULL AND ${aggDur} >= 0;
      ${segmentsDdl}

      CREATE PERFETTO TABLE ${this.projectedTableName} AS
      SELECT ${projectedCols}
      FROM ${agg.tableName}
      ${this.sliceJoinClause}
      ${this.pivotJoinClause};
    `);
  }

  async createTracks(): Promise<TrackNode> {
    await this.props.trace.engine.query(this.modulesClause);
    await this.buildTables();

    const rootTrackNode = await this.createCounterTrackNode(
      this.props.trackTitle,
      [],
    );

    // Enumerate the distinct (k*, s*, p*) tuples in one query and build the tree
    // in memory. Reads from the (possibly fanned) projection so distinct pivot
    // values are enumerated; the GROUP BY collapses the fan-out.
    const allColNames = [
      ...this.aggColNames,
      ...this.sliceColNames,
      ...this.pivotColNames,
    ];
    const res = await this.props.trace.engine.query(`
      SELECT ${allColNames.join(', ')}
      FROM ${this.projectedTableName}
      GROUP BY ${allColNames.join(', ')}
    `);

    await this.createBreakdownHierarchy(
      rootTrackNode,
      res,
      this.aggColNames,
      this.sliceColNames,
      this.pivotColNames,
    );

    return rootTrackNode;
  }

  private async createBreakdownHierarchy(
    rootNode: TrackNode,
    queryResult: QueryResult,
    aggColumns: ReadonlyArray<string>,
    sliceColumns: ReadonlyArray<string>,
    pivotColumns: ReadonlyArray<string>,
  ): Promise<void> {
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
          this.createSliceTrackNode(name, filters, colIndex, false),
      );

      state = await this.processRowForColumns(
        iter,
        state,
        pivotColumns,
        cache,
        (name, filters, colIndex) =>
          this.createSliceTrackNode(name, filters, colIndex, true),
      );
    }
  }

  private async processRowForColumns(
    iter: {get: (col: string) => SqlValue},
    {
      currentNode,
      currentFilters,
    }: {currentNode: TrackNode; currentFilters: Filter[]},
    columns: ReadonlyArray<string>,
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
    isPivot: boolean,
  ): Promise<TrackNode> {
    return this.createTrackNode(title, newFilters, (uri, filtersClause) => {
      // Pivot tracks render the joined sub-intervals (one slice per joined row)
      // on the pivot's own timeline; each fanned row is a distinct slice, so a
      // synthetic row id is used. Slice tracks render the source intervals once,
      // collapsing the pivot join's fan-out in the shared projection. When a real
      // sliceIdColumn is supplied the fan-out collapses via DISTINCT keyed on
      // that id (kept so the details panel can resolve the slice); otherwise the
      // id is a per-row ROW_NUMBER that would defeat DISTINCT, so dedupe on the
      // displayed columns and assign the synthetic id afterwards.
      const sliceCol = this.sliceColNames[columnIndex];
      const src = isPivot
        ? `
            SELECT ROW_NUMBER() OVER () AS id, pivot_ts AS ts, pivot_dur AS dur,
                   ${this.pivotColNames[columnIndex]} AS name
            FROM ${this.projectedTableName}
            ${filtersClause}
          `
        : this.props.sliceIdColumn !== undefined
          ? `
              SELECT DISTINCT id, slice_ts AS ts, slice_dur AS dur,
                     ${sliceCol} AS name
              FROM ${this.projectedTableName}
              ${filtersClause}
            `
          : `
              SELECT ROW_NUMBER() OVER () AS id, ts, dur, name
              FROM (
                SELECT DISTINCT slice_ts AS ts, slice_dur AS dur,
                       ${sliceCol} AS name
                FROM ${this.projectedTableName}
                ${filtersClause}
              )
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
    });
  }

  private async getCounterTrackSortOrder(
    filtersClause: string,
    depth: number,
  ): Promise<number> {
    const aggregationQuery = this.getAggregationQuery(filtersClause, depth);
    const result = await this.props.trace.engine.query(`
      SELECT MAX(value) as max_value FROM (${aggregationQuery})
    `);
    const maxValue = result.firstRow({max_value: NUM_NULL}).max_value;
    return maxValue === null ? 0 : maxValue;
  }

  private async createCounterTrackNode(
    name: string,
    newFilters: Filter[],
  ): Promise<TrackNode> {
    // Counter nodes only ever filter on breakdown (k*) columns, so the
    // filter count IS the hierarchy depth, which picks the segments table
    // partitioned by exactly those columns.
    const depth = newFilters.length;
    return this.createTrackNode(
      name,
      newFilters,
      (uri, filtersClause) =>
        // Lazy: getAggregationQuery is a plain filter over this depth's
        // pre-aggregated segments table, and CounterTrack's useData fires it
        // only on render — nothing is materialized per node at trace load.
        CounterTrack.create({
          trace: this.props.trace,
          uri,
          sqlSource: `
            SELECT ts, value
            FROM (${this.getAggregationQuery(filtersClause, depth)})
          `,
        }),
      (filtersClause) => this.getCounterTrackSortOrder(filtersClause, depth),
    );
  }

  private async createTrackNode(
    name: string,
    filters: Filter[],
    createTrack: (uri: string, filtersClause: string) => TrackRenderer,
    getSortOrder?: (filterClause: string) => Promise<number>,
  ): Promise<TrackNode> {
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
