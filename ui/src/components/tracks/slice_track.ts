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
import {Time} from '../../base/time';
import {CachedResolver} from './cached_resolver';
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {TrackEventDetails, TrackEventSelection} from '../../public/selection';
import {Trace} from '../../public/trace';
import {Slice} from '../../public/track';
import {DatasetSchema, SourceDataset} from '../../trace_processor/dataset';
import {
  SqlValue,
  LONG,
  NUM,
  LONG_NULL,
} from '../../trace_processor/query_result';
import {createPerfettoTable} from '../../trace_processor/sql_utils';
import {getColorForSlice} from '../colorizer';
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
import {SliceTrackDetailsPanel} from './slice_track_details_panel';

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

export interface SliceTrackAttrs<
  T extends DatasetSchema,
  K extends keyof T = keyof T,
> {
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
   * - `ts` (LONG): Timestamp of each event (in nanoseconds). Serves as the
   *   start time for slices with a `dur` column or the instant time otherwise.
   *
   * Auto-generated columns (if not provided):
   * - `id` (NUM): Unique identifier for slices in the track. If not provided
   *   in the dataset, will be automatically generated using ROW_NUMBER()
   *   ordered by timestamp.
   *
   * Optional columns:
   * - `dur` (LONG): Duration of each event (in nanoseconds). Without this
   *   column, all slices are treated as instant events and rendered as
   *   chevrons. With this column, each slice is rendered as a box where the
   *   width corresponds to the duration of the slice.
   * - `depth` (NUM): Depth of each event, used for vertical arrangement. Higher
   *   depth values are rendered lower down on the track.
   * - `layer` (NUM): This layer value influences the mipmap function. Slices in
   *   different layers will be mipmapped independency of each other, and the
   *   buckets of higher layers will be rendered on top of lower layers.
   */
  readonly dataset: SourceDataset<T> | (() => SourceDataset<T>);

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
   * Events are usually rendered in color order for performance. However for
   * tracks that have a lot of overlapping event such as those full of instant
   * events, this can look odd, so this setting forces events to be rendered in
   * timestamp order, potentially at the cost of a bit of performance.
   */
  readonly forceTsRenderOrder?: boolean;

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
  tooltip?(slice: SliceWithRow<T, K>): m.Children;

  /**
   * An optional callback to customize the details panel for events on this
   * track. Called whenever an event is selected.
   *
   * If omitted, a default details panel will be created that displays all
   * fields from the dataset with appropriate formatting for common slice
   * properties (name, ts, dur).
   */
  detailsPanel?(row: Pick<T, K>): TrackEventDetailsPanel;

  /**
   * An optional callback to define the fill ratio for slices. The fill ratio is
   * an extra bit of information that can be rendered on each slice, where the
   * slice essentially contains a single horizontal bar chart. The value
   * returned can be a figure between 0.0 and 1.0 where 0 is empty and 1 is
   * full. If omitted, all slices will be rendered with their fill ratios set to
   * 'full'.
   */
  fillRatio?(row: T): number;

  // NEW: Cached resolvers. Declare which row properties you depend on, and
  // the result is cached by those values. Takes precedence over legacy lambdas.

  /**
   * Cached color resolver. Declare dependencies and compute function.
   * Results are cached by the dependency values.
   */
  readonly color?: {
    readonly keys: readonly (keyof T)[];
    readonly fn: (row: T) => ColorScheme;
  };

  /**
   * Cached title resolver. Declare dependencies and compute function.
   * Results are cached by the dependency values.
   */
  readonly title?: {
    readonly keys: readonly (keyof T)[];
    readonly fn: (row: T) => string | undefined;
  };

  /**
   * Cached fill ratio resolver. Declare dependencies and compute function.
   * Results are cached by the dependency values.
   */
  readonly fill?: {
    readonly keys: readonly (keyof T)[];
    readonly fn: (row: T) => number;
  };

  /**
   * Explicitly declare which row properties to keep on the slice.
   * Only these properties will be copied to slice.row, reducing memory and
   * making dependencies explicit. If not specified, all schema columns are
   * copied (legacy behavior).
   */
  readonly keep?: readonly K[];

  /**
   * An optional function to define buttons which are displayed on the track
   * shell. This function is called every Mithril render cycle.
   */
  shellButtons?(): m.Children;
}

export type RowSchema = {
  readonly id?: number;
  readonly ts: bigint;
  readonly dur?: bigint | null;
  readonly depth?: number;
  readonly layer?: number;
} & DatasetSchema;

// Slice has id, ts, dur, depth directly (shared with row data).
// Other Slice properties use _ prefix. Extra row properties from K are added.
type SliceWithRow<T, K extends keyof T = keyof T> = Slice & Pick<T, K>;

function getDataset<T extends DatasetSchema, K extends keyof T>(
  attrs: SliceTrackAttrs<T, K>,
): SourceDataset<T> {
  const dataset = attrs.dataset;
  return typeof dataset === 'function' ? dataset() : dataset;
}

export class SliceTrack<
  T extends RowSchema,
  K extends keyof T = keyof T,
> extends BaseSliceTrack<SliceWithRow<T, K>, BaseRow & T> {
  readonly rootTableName?: string;

  // Keys to copy from row to slice.row (from attrs.keep or all schema keys)
  private readonly keepKeys: readonly string[];

  // Cached resolvers (created from attrs config)
  private readonly colorResolver?: CachedResolver<T, keyof T, ColorScheme>;
  private readonly titleResolver?: CachedResolver<T, keyof T, string | undefined>;
  private readonly fillResolver?: CachedResolver<T, keyof T, number>;

  /**
   * Factory function to create a SliceTrack. This is purely an alias for new
   * SliceTrack() but exists for symmetry with createMaterialized()
   * below.
   *
   * @param attrs The track attributes
   * @returns A fully initialized SliceTrack
   */
  static create<T extends RowSchema, K extends keyof T = keyof T>(
    attrs: SliceTrackAttrs<T, K>,
  ): SliceTrack<T, K> {
    return new SliceTrack(attrs);
  }

  /**
   * Async factory function to create a SliceTrack, first materializing
   * the dataset into a perfetto table. This can be more efficient if for
   * example the dataset is a complex query with multiple joins or window
   * functions, so materializing it up front can improve rendering performance,
   * for a one-time cost.
   *
   * However, it does have some downsides:
   * - You're front loading the cost of materialization, which can slow down
   *   trace load times.
   * - It uses more memory, as the entire dataset is materialized in memory as a
   *   new table.
   * - It means that this dataset track has a new root source table, which makes
   *   it impossible to combine with other tracks for the purposes of bulk
   *   operations such as aggregations or search.
   *
   * @param attrs The track attributes
   * @returns A fully initialized SliceTrack
   */
  static async createMaterialized<T extends RowSchema, K extends keyof T = keyof T>(
    attrs: SliceTrackAttrs<T, K>,
  ): Promise<SliceTrack<T, K>> {
    const originalDataset = getDataset(attrs);
    // Create materialized table from the render query - we might as well
    // materialize the calculated columns that are missing from the source
    // dataset while we're here as this will improve performance at runtime.
    const materializedTable = await createPerfettoTable({
      engine: attrs.trace.engine,
      as: generateRenderQuery(originalDataset),
    });

    // Create a new dataset that queries the materialized table
    const materializedDataset = new SourceDataset({
      src: materializedTable.name,
      schema: {
        ...originalDataset.schema,

        // We know we must have these columns now as they are injected in
        // generateRenderQuery(), so we can add them to the schema to avoid the
        // DST from adding them again.
        id: NUM,
        layer: NUM,
        depth: NUM,
        dur: LONG,
      },
    });

    return new SliceTrack({
      ...attrs,
      dataset: materializedDataset,
    });
  }

  private constructor(private readonly attrs: SliceTrackAttrs<T, K>) {
    const dataset = getDataset(attrs);
    super(
      attrs.trace,
      attrs.uri,
      {...BASE_ROW, ...dataset.schema},
      attrs.sliceLayout,
      attrs.initialMaxDepth,
      attrs.instantStyle?.width,
      attrs.forceTsRenderOrder ?? false,
    );
    this.rootTableName = attrs.rootTableName;

    // Keys to copy from row - filter out properties already on Slice
    // (id, ts, dur, depth are already stored as _id, _ts, _dur, _depth)
    const sliceProps = new Set(['id', 'ts', 'dur', 'depth']);
    const allKeys = attrs.keep
      ? (attrs.keep as readonly string[])
      : Object.keys(dataset.schema);
    this.keepKeys = allKeys.filter((k) => !sliceProps.has(k));

    // Create cached resolvers from config
    if (attrs.color) {
      this.colorResolver = new CachedResolver(attrs.color.keys, attrs.color.fn);
    }
    if (attrs.title) {
      this.titleResolver = new CachedResolver(attrs.title.keys, attrs.title.fn);
    }
    if (attrs.fill) {
      this.fillResolver = new CachedResolver(attrs.fill.keys, attrs.fill.fn);
    }
  }

  // Flags are derived from dur, compute once
  private computeFlags(dur: bigint): number {
    if (dur === -1n) return SLICE_FLAGS_INCOMPLETE;
    if (dur === 0n) return SLICE_FLAGS_INSTANT;
    return 0;
  }

  // Copy kept row properties (no prefix - row data is primary)
  private copyKeptPropsInto(src: T, dest: Record<string, SqlValue>): void {
    const keys = this.keepKeys;
    for (let i = 0; i < keys.length; i++) {
      const k = keys[i];
      dest[k] = (src as Record<string, SqlValue>)[k];
    }
  }

  // Create a new slice object (used when buffer grows)
  protected override createSlice(row: BaseRow & T): SliceWithRow<T, K> {
    const title = this.resolveTitle(row);
    const slice: Record<string, unknown> = {
      // Row properties (no prefix, shared)
      id: row.id,
      ts: Time.fromRaw(row.ts),
      dur: row.dur,
      depth: row.depth,
      // Slice-specific computed properties (use _ prefix, Q = quantized)
      _startNsQ: Time.fromRaw(row.tsQ),
      _endNsQ: Time.fromRaw(row.tsQ + row.durQ),
      _durNsQ: row.durQ,
      _count: row.count,
      _flags: this.computeFlags(row.dur),
      _fillRatio: this.resolveFillRatio(row),
      _subTitle: '',
      _isHighlighted: false,
      _title: title,
      _colorScheme: this.resolveColor(row, title),
    };
    // Add extra kept row properties (no prefix)
    this.copyKeptPropsInto(row, slice as Record<string, SqlValue>);
    return slice as SliceWithRow<T, K>;
  }

  // Update existing slice in-place (zero allocations)
  protected override updateSlice(
    row: BaseRow & T,
    slice: SliceWithRow<T, K>,
  ): void {
    const title = this.resolveTitle(row);
    const s = slice as Record<string, unknown>;
    // Row properties (no prefix, shared)
    s.id = row.id;
    s.ts = Time.fromRaw(row.ts);
    s.dur = row.dur;
    s.depth = row.depth;
    // Slice-specific computed properties (use _ prefix, Q = quantized)
    s._startNsQ = Time.fromRaw(row.tsQ);
    s._endNsQ = Time.fromRaw(row.tsQ + row.durQ);
    s._durNsQ = row.durQ;
    s._count = row.count;
    s._flags = this.computeFlags(row.dur);
    s._fillRatio = this.resolveFillRatio(row);
    s._subTitle = '';
    s._isHighlighted = false;
    s._title = title;
    s._colorScheme = this.resolveColor(row, title);
    // Update extra kept row properties (no prefix)
    this.copyKeptPropsInto(row, s as Record<string, SqlValue>);
  }

  // Resolution methods: cached resolvers take precedence over legacy lambdas.

  private resolveTitle(row: T): string | undefined {
    // Cached resolver takes precedence
    if (this.titleResolver) {
      return this.titleResolver.get(row);
    }
    // Legacy API (no caching)
    if (this.attrs.sliceName) {
      return this.attrs.sliceName(row);
    }
    // Default: use 'name' column if present
    if ('name' in row && typeof row.name === 'string') {
      return row.name;
    }
    return undefined;
  }

  private resolveColor(row: T, title: string | undefined): ColorScheme {
    // Cached resolver takes precedence
    if (this.colorResolver) {
      return this.colorResolver.get(row);
    }
    // Legacy API (no caching)
    if (this.attrs.colorizer) {
      return this.attrs.colorizer(row);
    }
    // Default: color by title or id
    if (title) return getColorForSlice(title);
    return getColorForSlice(`${row.id}`);
  }

  private resolveFillRatio(row: T): number {
    // Cached resolver takes precedence
    if (this.fillResolver) {
      return this.fillResolver.get(row);
    }
    // Legacy API (no caching)
    if (this.attrs.fillRatio) {
      return this.attrs.fillRatio(row);
    }
    return 1;
  }

  override getSqlSource(): string {
    const dataset =
      typeof this.attrs.dataset === 'function'
        ? this.attrs.dataset()
        : this.attrs.dataset;
    return generateRenderQuery(dataset);
  }

  getDataset() {
    return getDataset(this.attrs);
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
      // Provide a default details panel that shows all dataset fields
      const dataset = getDataset(this.attrs);
      return new SliceTrackDetailsPanel(
        this.trace,
        dataset,
        sel as unknown as T,
      );
    }
  }

  async getSelectionDetails(
    id: number,
  ): Promise<TrackEventDetails | undefined> {
    const {trace} = this.attrs;
    const dataset = getDataset(this.attrs);

    // If our dataset already has an id column, we can use it directly,
    // otherwise we need to generate one using row number.
    const query = (function () {
      if (dataset.implements({id: NUM})) {
        return dataset.query();
      } else {
        return `
          SELECT
            ROW_NUMBER() OVER (ORDER BY ts) AS id,
            *
          FROM (${dataset.query()})
        `;
      }
    })();

    const result = await trace.engine.query(`
      SELECT *
      FROM (${query})
      WHERE id = ${id}
    `);

    const row = result.iter(dataset.schema);
    if (!row.valid()) return undefined;

    // Pull the fields out from the results
    const data: {[key: string]: SqlValue} = {};
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
      slice._isHighlighted = slice === this.hoveredSlice;
    }
  }

  getTrackShellButtons() {
    return this.attrs.shellButtons?.();
  }

  override renderTooltipForSlice(slice: SliceWithRow<T, K>): m.Children {
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
export function renderTooltip<T, K extends keyof T = keyof T>(
  trace: Trace,
  slice: SliceWithRow<T, K>,
  opts: {readonly title?: string; readonly extras?: m.Children} = {},
) {
  const durationFormatted = formatDurationForTooltip(trace, slice);
  const {title = slice._title, extras} = opts;
  return [
    m('', exists(durationFormatted) && m('b', durationFormatted), ' ', title),
    extras,
    slice._count > 1 && m('div', `and ${slice._count - 1} other events`),
  ];
}

// Given a slice, format the duration of the slice for a tooltip.
function formatDurationForTooltip(trace: Trace, slice: Slice) {
  const {dur, _flags} = slice;
  if (_flags & SLICE_FLAGS_INCOMPLETE) {
    return '[Incomplete]';
  } else if (_flags & SLICE_FLAGS_INSTANT) {
    return undefined;
  } else {
    return formatDuration(trace, dur);
  }
}

// Generate a query to use for generating slices to be rendered
export function generateRenderQuery<T extends DatasetSchema>(
  dataset: SourceDataset<T>,
) {
  const hasId = dataset.implements({id: NUM});
  const hasLayer = dataset.implements({layer: NUM});

  const extraCols = Object.fromEntries(
    Object.keys(dataset.schema).map((key) => [key, key]),
  );

  const cols = {
    ...extraCols,
    // If we have no id, automatically generate one using row number.
    id: hasId ? 'id' : 'ROW_NUMBER() OVER (ORDER BY ts)',
    ts: 'ts',
    layer: hasLayer ? 'layer' : 0, // If we have no layer, assume flat layering.
    depth: getDepthExpression(dataset),
    dur: getDurExpression(dataset),
  } as const;

  return `SELECT ${Object.entries(cols)
    .map(([key, value]) => `${value} AS ${key}`)
    .join(', ')} FROM (${dataset.query()})`;
}

function getDepthExpression<T extends DatasetSchema>(
  dataset: SourceDataset<T>,
): string {
  const hasDepth = dataset.implements({depth: NUM});
  const hasDur = dataset.implements({dur: LONG});
  const hasNullableDur = dataset.implements({dur: LONG_NULL});

  if (hasDepth) {
    return 'depth';
  } else if (hasDur) {
    return `internal_layout(ts, dur) OVER (ORDER BY ts ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW)`;
  } else if (hasNullableDur) {
    return `internal_layout(ts, COALESCE(dur, -1)) OVER (ORDER BY ts ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW)`;
  } else {
    return '0';
  }
}

function getDurExpression<T extends DatasetSchema>(
  dataset: SourceDataset<T>,
): string | undefined {
  const hasDur = dataset.implements({dur: LONG});
  const hasNullableDur = dataset.implements({dur: LONG_NULL});

  if (hasDur) {
    return 'dur';
  } else if (hasNullableDur) {
    return 'COALESCE(dur, -1)';
  } else {
    // Assume instants
    return '0';
  }
}
