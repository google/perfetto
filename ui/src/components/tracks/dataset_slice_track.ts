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
import {assertTrue} from '../../base/logging';
import {Time} from '../../base/time';
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {TrackEventDetails, TrackEventSelection} from '../../public/selection';
import {Trace} from '../../public/trace';
import {Slice} from '../../public/track';
import {DatasetSchema, SourceDataset} from '../../trace_processor/dataset';
import {ColumnType, LONG, NUM} from '../../trace_processor/query_result';
import {getColorForSlice} from '../colorizer';
import {generateSqlWithInternalLayout} from '../sql_utils/layout';
import {formatDuration} from '../time_utils';
import {
  BASE_ROW,
  BaseRow,
  BaseSliceTrack,
  SLICE_FLAGS_INCOMPLETE,
  SLICE_FLAGS_INSTANT,
  SliceLayout,
} from './base_slice_track';
import {Point2D, Size2D} from '../../base/geom';
import {exists} from '../../base/utils';

export interface InstantStyle {
  /**
   * Defines the width of an instant event. This, combined with the row height,
   * defines the event's hitbox. This width is forwarded to the render function.
   */
  readonly width: number;

  /**
   * Customize how instant events are rendered.
   *
   * @param ctx - CanvasRenderingContext to draw to.
   * @param rect - Position of the TL corner & size of the instant event's
   * bounding box.
   */
  render(ctx: CanvasRenderingContext2D, rect: Size2D & Point2D): void;
}

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
   * TODO(stevegolton): Sort out `Track` and `TrackRenderer` to avoid
   * duplication.
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
   * Override the default geometry and layout of the slices rendered on the
   * track.
   */
  readonly sliceLayout?: Partial<SliceLayout>;

  /**
   * Override the appearance of instant events.
   */
  readonly instantStyle?: InstantStyle;

  /**
   * This function can optionally be used to override the query that is
   * generated for querying the slices rendered on the track. This is typically
   * used to provide a non-standard depth value, but can be used as an escape
   * hatch to completely override the query if required.
   *
   * The returned query must be in the form of a select statement or table name
   * with the following columns:
   * - id: NUM
   * - ts: LONG
   * - dur: LONG
   * - depth: NUM
   */
  queryGenerator?(dataset: SourceDataset): string;

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
   * omitted, the title will be used instead.
   */
  tooltip?(slice: SliceWithRow<T>): m.Children;

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
    super(
      attrs.trace,
      attrs.uri,
      {...BASE_ROW, ...attrs.dataset.schema},
      attrs.sliceLayout,
      attrs.initialMaxDepth,
      attrs.instantStyle?.width,
    );
    const {dataset, queryGenerator} = attrs;

    // This is the minimum viable implementation that the source dataset must
    // implement for the track to work properly. Typescript should enforce this
    // now, but typescript can be worked around, and checking it is cheap.
    // Better to error out early.
    assertTrue(this.attrs.dataset.implements(rowSchema));

    this.sqlSource =
      queryGenerator?.(dataset) ?? this.generateRenderQuery(dataset);
    this.rootTableName = attrs.rootTableName;
  }

  override rowToSlice(row: BaseRow & T): SliceWithRow<T> {
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

  // Generate a query to use for generating slices to be rendered
  private generateRenderQuery(dataset: SourceDataset<T>) {
    if (dataset.implements({dur: LONG, depth: NUM})) {
      // Both depth and dur provided, we can use the dataset as-is.
      return dataset.query();
    } else if (dataset.implements({depth: NUM})) {
      // Depth provided but no dur, assume each event is an instant event by
      // hard coding dur to 0.
      return `select 0 as dur, * from (${dataset.query()})`;
    } else if (dataset.implements({dur: LONG})) {
      // Dur provided but no depth, automatically calculate the depth using
      // internal_layout().
      return generateSqlWithInternalLayout({
        columns: ['*'],
        source: dataset.query(),
        ts: 'ts',
        dur: 'dur',
        orderByClause: 'ts',
      });
    } else {
      // No depth nor dur provided, use 0 for both.
      return `select 0 as dur, 0 as depth, * from (${dataset.query()})`;
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

  override getSqlSource(): string {
    return this.sqlSource;
  }

  override getJoinSqlSource(): string {
    // This is a little performance optimization. Internally BST joins the
    // results of the mipmap table query with the sqlSource in order to get the
    // original ts, dur and id. However this sqlSource can sometimes be a
    // contrived, slow query, usually to calculate the depth (e.g. something
    // based on experimental_slice_layout).
    //
    // We don't actually need a depth value at this point, so calculating it is
    // worthless. We only need ts, id, and dur. We don't even need this query to
    // be correctly filtered, as we are merely joining on this table. We do
    // however need it to be fast.
    //
    // In conclusion, if the dataset source has a dur column present (ts, and id
    // are mandatory), then we can take a shortcut and just use this much
    // simpler query to join on.
    if (this.attrs.dataset.implements({dur: LONG})) {
      return this.attrs.dataset.src;
    } else {
      return this.sqlSource;
    }
  }

  getDataset() {
    return this.attrs.dataset;
  }

  detailsPanel(sel: TrackEventSelection): TrackEventDetailsPanel | undefined {
    if (this.attrs.detailsPanel) {
      // This type assertion is required as a temporary patch while the
      // specifics of selection details are being worked out. Eventually we will
      // change the selection details to be purely based on dataset, but there
      // are currently some use cases preventing us from doing so. For now, this
      // type assertion is safe as we know we just returned the entire row from
      // from getSelectionDetails() so we know it must at least implement the
      // row's type `T`.
      return this.attrs.detailsPanel(sel as unknown as T);
    } else {
      return undefined;
    }
  }

  async getSelectionDetails(
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

  override renderTooltipForSlice(slice: SliceWithRow<T>): m.Children {
    return this.attrs.tooltip?.(slice) ?? renderTooltip(this.trace, slice);
  }

  protected override drawChevron(
    ctx: CanvasRenderingContext2D,
    x: number,
    y: number,
    h: number,
  ) {
    if (this.attrs.instantStyle?.render) {
      this.attrs.instantStyle.render(ctx, {
        x,
        y,
        height: h,
        width: this.attrs.instantStyle.width,
      });
    } else {
      super.drawChevron(ctx, x, y, h);
    }
  }
}

// Most tooltips follow a predictable formula. This function extracts the
// duration and title from the slice and formats them in a standard way,
// allowing some optional overrides to be passed.
export function renderTooltip<T>(
  trace: Trace,
  slice: SliceWithRow<T>,
  opts: {readonly title?: string; readonly extras?: m.Children} = {},
) {
  const durationFormatted = formatDurationForTooltip(trace, slice);
  const {title = slice.title, extras} = opts;

  return [
    m('', exists(durationFormatted) && m('b', durationFormatted), ' ', title),
    extras,
  ];
}

// Given a slice, format the duration of the slice for a tooltip.
function formatDurationForTooltip(trace: Trace, slice: Slice) {
  const {dur, flags} = slice;
  if (flags & SLICE_FLAGS_INCOMPLETE) {
    return '[Incomplete]';
  } else if (flags & SLICE_FLAGS_INSTANT) {
    return undefined;
  } else {
    return formatDuration(trace, dur);
  }
}
