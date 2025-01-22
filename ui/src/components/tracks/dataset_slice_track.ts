// Copyright (C) 2023 The Android Open Source Project
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
import {ColorScheme} from '../../base/color_scheme';
import {assertIsInstance, assertTrue} from '../../base/logging';
import {Time} from '../../base/time';
import {TraceImpl} from '../../core/trace_impl';
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {TrackEventDetails, TrackEventSelection} from '../../public/selection';
import {Trace} from '../../public/trace';
import {Slice} from '../../public/track';
import {DatasetSchema, SourceDataset} from '../../trace_processor/dataset';
import {ColumnType, LONG, NUM} from '../../trace_processor/query_result';
import {getColorForSlice} from '../colorizer';
import {ThreadSliceDetailsPanel} from '../details/thread_slice_details_tab';
import {generateSqlWithInternalLayout} from '../sql_utils/layout';
import {formatDuration} from '../time_utils';
import {
  BASE_ROW,
  BaseRow,
  BaseSliceTrack,
  OnSliceOverArgs,
  SLICE_FLAGS_INCOMPLETE,
  SLICE_FLAGS_INSTANT,
} from './base_slice_track';
import {SLICE_LAYOUT_FIT_CONTENT_DEFAULTS} from './slice_layout';

export type DepthProvider = (dataset: SourceDataset) => string;

export interface DatasetSliceTrackAttrs<T extends DatasetSchema> {
  /**
   * The trace object used by the track for accessing the query engine and other
   * trace-related resources.
   */
  readonly trace: Trace;

  /**
   * The URI of this track, which must match the URI specified in the track
   * descriptor.
   *
   * TODO(stevegolton): Merge `TrackDescriptor` and `Track` into one entity to
   * avoid duplication.
   */
  readonly uri: string;

  /**
   * The source dataset defining the content of this track.
   *
   * A source dataset consists of a SQL select statement or table name with a
   * column schema and optional filtering information. It represents a set of
   * instructions to extract slice-like rows from trace processor that
   * represents the content of this track, which avoids the need to materialize
   * all slices into JavaScript beforehand. This approach minimizes memory usage
   * and improves performance by only materializing the necessary rows on
   * demand.
   *
   * Required columns:
   * - `id` (NUM): Unique identifier for slices in the track.
   * - `ts` (LONG): Timestamp of each event (in nanoseconds). Serves as the
   *   start time for slices with a `dur` column or the instant time otherwise.
   *
   * Optional columns:
   * - `dur` (LONG): Duration of each event (in nanoseconds). Without this
   *   column, all slices are treated as instant events and rendered as
   *   chevrons. With this column, each slice is rendered as a box where the
   *   width corresponds to the duration of the slice.
   * - `depth` (NUM): Depth of each event, used for vertical arrangement. Higher
   *   depth values are rendered lower down on the track.
   */
  readonly dataset: SourceDataset<T>;

  /**
   * An optional initial estimate for the maximum depth value. Helps minimize
   * flickering while scrolling by stabilizing the track height before all
   * slices are loaded. Even without this value, the height of the track still
   * adjusts dynamically as slices are loaded to accommodate the highest depth
   * value.
   */
  readonly initialMaxDepth?: number;

  /**
   * An optional root table name for the track's data source.
   *
   * This typically represents a well-known table name and serves as the root
   * `id` namespace for the track. It is primarily used for resolving events
   * with a combination of table name and `id`.
   *
   * TODO(stevegolton): Consider moving this to dataset.
   */
  readonly rootTableName?: string;

  /**
   * This is a function that, given a dataset, returns a query that definitely
   * contains a non-null depth column.
   */
  readonly depthProvider?: DepthProvider;

  /**
   * An optional function to override the color scheme for each event.
   * If omitted, the default slice color scheme is used.
   */
  colorizer?(row: T): ColorScheme;

  /**
   * An optional function to override the text displayed on each event. If
   * omitted, the value in the `name` column from the dataset is used, otherwise
   * the slice is left blank.
   */
  sliceName?(row: T): string;

  /**
   * An optional function to override the tooltip content for each event. If
   * omitted, the title & slice duration will be used.
   */
  tooltip?(row: T): string[];

  /**
   * An optional callback to customize the details panel for events on this
   * track. Called whenever an event is selected.
   */
  detailsPanel?(row: T): TrackEventDetailsPanel;

  /**
   * An optional callback to define the fill ratio for slices. The fill ratio is
   * an extra bit of information that can be rendered on each slice, where the
   * slice essentially contains a single horizontal bar chart. The value
   * returned can be a figure between 0.0 and 1.0 where 0 is empty and 1 is
   * full. If omitted, all slices will be rendered with their fill ratios set to
   * 'full'.
   */
  fillRatio?(row: T): number;

  /**
   * An optional function to define buttons which are displayed on the track
   * shell. This function is called every Mithril render cycle.
   */
  shellButtons?(): m.Children;
}

const rowSchema = {
  id: NUM,
  ts: LONG,
};

/**
 * Pre-canned depth provider that lays out slices automatically to minimize
 * depth while avoiding overlaps. The source dataset requires ts and dur
 * columns.
 */
export function internalLayoutDepthProvider(dataset: SourceDataset) {
  return generateSqlWithInternalLayout({
    columns: ['*'],
    source: dataset.query(),
    ts: 'ts',
    dur: 'dur',
    orderByClause: 'ts',
  });
}

/**
 * Simple flat layout provider that just lays out all slices in one flat layer.
 */
export function flatDepthProvider(dataset: SourceDataset) {
  return `select 0 as depth, * from (${dataset.query()})`;
}

export type ROW_SCHEMA = typeof rowSchema;

// We attach a copy of our rows to each slice, so that the tooltip can be
// resolved properly.
type SliceWithRow<T> = Slice & {row: T};

export class DatasetSliceTrack<T extends ROW_SCHEMA> extends BaseSliceTrack<
  SliceWithRow<T>,
  BaseRow & T
> {
  protected readonly sqlSource: string;
  readonly rootTableName?: string;

  constructor(private readonly attrs: DatasetSliceTrackAttrs<T>) {
    super(attrs.trace, attrs.uri, {...BASE_ROW, ...attrs.dataset.schema});
    const {dataset, depthProvider} = attrs;

    // This is the minimum viable implementation that the source dataset must
    // implement for the track to work properly. Typescript should enforce this
    // now, but typescript can be worked around, and checking it is cheap.
    // Better to error out early.
    assertTrue(this.attrs.dataset.implements(rowSchema));

    const sqlSource = depthProvider?.(dataset) ?? this.getDepthSource(dataset);
    if (dataset.implements({dur: LONG})) {
      this.sqlSource = sqlSource;
    } else {
      this.sqlSource = `select 0 as dur, * from (${sqlSource})`;
    }
    this.rootTableName = attrs.rootTableName;

    this.sliceLayout = {
      ...SLICE_LAYOUT_FIT_CONTENT_DEFAULTS,
      depthGuess: attrs.initialMaxDepth,
    };
  }

  rowToSlice(row: BaseRow & T): SliceWithRow<T> {
    const slice = this.rowToSliceBase(row);
    const title = this.getTitle(row);
    const color = this.getColor(row, title);

    // Take a copy of the row, only copying the keys listed in the schema.
    const cols = Object.keys(this.attrs.dataset.schema);
    const clonedRow = Object.fromEntries(
      Object.entries(row).filter(([key]) => cols.includes(key)),
    ) as T;

    return {
      ...slice,
      title,
      colorScheme: color,
      fillRatio: this.attrs.fillRatio?.(row) ?? slice.fillRatio,
      row: clonedRow,
    };
  }

  private getDepthSource(dataset: SourceDataset<T>) {
    if (dataset.implements({depth: NUM})) {
      return dataset.query();
    } else if (dataset.implements({dur: LONG})) {
      return internalLayoutDepthProvider(dataset);
    } else {
      return flatDepthProvider(dataset);
    }
  }

  private getTitle(row: T) {
    if (this.attrs.sliceName) return this.attrs.sliceName(row);
    if ('name' in row && typeof row.name === 'string') return row.name;
    return undefined;
  }

  private getColor(row: T, title: string | undefined) {
    if (this.attrs.colorizer) return this.attrs.colorizer(row);
    if (title) return getColorForSlice(title);
    return getColorForSlice(`${row.id}`);
  }

  getSqlSource(): string {
    return this.sqlSource;
  }

  getDataset() {
    return this.attrs.dataset;
  }

  detailsPanel(sel: TrackEventSelection): TrackEventDetailsPanel | undefined {
    // This type assertion is required as a temporary patch while the specifics
    // of selection details are being worked out. Eventually we will change the
    // selection details to be purely based on dataset, but there are currently
    // some use cases preventing us from doing so. For now, this type assertion
    // is safe as we know we just returned the entire row from from
    // getSelectionDetails() so we know it must at least implement the row's
    // type `T`.

    if (this.attrs.detailsPanel) {
      return this.attrs.detailsPanel(sel as unknown as T);
    } else {
      // Rationale for the assertIsInstance: ThreadSliceDetailsPanel requires a
      // TraceImpl (because of flows) but here we must take a Trace interface,
      // because this class is exposed to plugins (which see only Trace).
      return new ThreadSliceDetailsPanel(
        assertIsInstance(this.trace, TraceImpl),
      );
    }
  }

  override async getSelectionDetails(
    id: number,
  ): Promise<TrackEventDetails | undefined> {
    const {trace, dataset} = this.attrs;
    const result = await trace.engine.query(`
      SELECT *
      FROM (${dataset.query()})
      WHERE id = ${id}
    `);

    const row = result.iter(dataset.schema);
    if (!row.valid()) return undefined;

    // Pull the fields out from the results
    const data: {[key: string]: ColumnType} = {};
    for (const col of result.columns()) {
      data[col] = row.get(col);
    }

    return {
      ...data,
      ts: Time.fromRaw(row.ts),
    };
  }

  override onUpdatedSlices(slices: Slice[]) {
    for (const slice of slices) {
      slice.isHighlighted = slice === this.hoveredSlice;
    }
  }

  getTrackShellButtons() {
    return this.attrs.shellButtons?.();
  }

  onSliceOver(args: OnSliceOverArgs<SliceWithRow<T>>) {
    const {title, dur, flags} = args.slice;
    let duration;
    if (flags & SLICE_FLAGS_INCOMPLETE) {
      duration = 'Incomplete';
    } else if (flags & SLICE_FLAGS_INSTANT) {
      duration = 'Instant';
    } else {
      duration = formatDuration(this.trace, dur);
    }
    if (title) {
      args.tooltip = [`${title} - [${duration}]`];
    } else {
      args.tooltip = [`[${duration}]`];
    }

    args.tooltip = this.attrs.tooltip?.(args.slice.row) ?? args.tooltip;
  }
}
