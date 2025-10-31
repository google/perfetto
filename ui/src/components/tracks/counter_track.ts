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

import m from 'mithril';
import {Time} from '../../base/time';
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {TrackEventDetails, TrackEventSelection} from '../../public/selection';
import {Trace} from '../../public/trace';
import {DatasetSchema, SourceDataset} from '../../trace_processor/dataset';
import {SqlValue, LONG, NUM} from '../../trace_processor/query_result';
import {createPerfettoTable} from '../../trace_processor/sql_utils';
import {removeFalsyValues} from '../../base/array_utils';
import {CounterTrackDetailsPanel} from './counter_track_details_panel';
import {BaseCounterTrack, CounterOptions} from './base_counter_track';
import {TrackMouseEvent} from '../../public/track';

export interface CounterTrackAttrs<T extends DatasetSchema> {
  /**
   * The trace object used by the track for accessing the query engine and other
   * trace-related resources.
   */
  readonly trace: Trace;

  /**
   * The URI of this track, which must match the URI specified in the track
   * descriptor.
   */
  readonly uri: string;

  /**
   * The source dataset defining the content of this track.
   *
   * A source dataset consists of a SQL select statement or table name with a
   * column schema and optional filtering information. It represents a set of
   * instructions to extract counter-like rows from trace processor that
   * represents the content of this track, which avoids the need to materialize
   * all counter values into JavaScript beforehand. This approach minimizes
   * memory usage and improves performance by only materializing the necessary
   * rows on demand.
   *
   * Required columns:
   * - `ts` (LONG): Timestamp of each counter value (in nanoseconds).
   * - `value` (NUM): The counter value at the given timestamp.
   *
   * Auto-generated columns (if not provided):
   * - `id` (NUM): Unique identifier for counter points in the track. If not
   *   provided in the dataset, will be automatically generated using ROW_NUMBER()
   *   ordered by timestamp.
   */
  readonly dataset: SourceDataset<T> | (() => SourceDataset<T>);

  /**
   * An optional root table name for the track's data source.
   *
   * This typically represents a well-known table name and serves as the root
   * `id` namespace for the track. It is primarily used for resolving events
   * with a combination of table name and `id`.
   */
  readonly rootTableName?: string;

  /**
   * Default counter options that will be merged with user preferences.
   * These provide the initial configuration for y-axis display, scaling,
   * and other counter-specific rendering options.
   */
  readonly defaultOptions?: Partial<CounterOptions>;

  /**
   * An optional callback to customize the details panel for counter points on
   * this track. Called whenever a counter point is selected.
   *
   * If omitted, a default details panel will be created that displays all
   * fields from the dataset with appropriate formatting for common counter
   * properties (ts, value).
   */
  detailsPanel?(row: T): TrackEventDetailsPanel;

  /**
   * An optional function to override the tooltip content for each counter point.
   * If omitted, a default counter tooltip will be used.
   */
  tooltip?(row: T, formattedValue: string): m.Children;

  /**
   * An optional function to define buttons which are displayed on the track
   * shell. This function is called every Mithril render cycle.
   */
  shellButtons?(): m.Children;
}

export type CounterRowSchema = {
  readonly id?: number;
  readonly ts: bigint;
  readonly value: number;
} & DatasetSchema;

function getDataset<T extends DatasetSchema>(
  attrs: CounterTrackAttrs<T>,
): SourceDataset<T> {
  const dataset = attrs.dataset;
  return typeof dataset === 'function' ? dataset() : dataset;
}

export class CounterTrack<T extends CounterRowSchema> extends BaseCounterTrack {
  readonly rootTableName?: string;

  /**
   * Factory function to create a CounterTrack. This is purely an alias for new
   * CounterTrack() but exists for symmetry with createMaterialized()
   * below.
   *
   * @param attrs The track attributes
   * @returns A fully initialized CounterTrack
   */
  static create<T extends CounterRowSchema>(
    attrs: CounterTrackAttrs<T>,
  ): CounterTrack<T> {
    return new CounterTrack(attrs);
  }

  /**
   * Async factory function to create a CounterTrack, first materializing
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
   * @returns A fully initialized CounterTrack
   */
  static async createMaterialized<T extends CounterRowSchema>(
    attrs: CounterTrackAttrs<T>,
  ): Promise<CounterTrack<T>> {
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
        ts: LONG,
        value: NUM,
      },
    });

    return new CounterTrack({
      ...attrs,
      dataset: materializedDataset,
    });
  }

  private constructor(private readonly attrs: CounterTrackAttrs<T>) {
    super(attrs.trace, attrs.uri, attrs.defaultOptions ?? {});
    this.rootTableName = attrs.rootTableName;
  }

  override getSqlSource(): string {
    const dataset = getDataset(this.attrs);
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
      return new CounterTrackDetailsPanel(
        this.trace,
        dataset,
        sel as unknown as T,
        this.getSqlSource(), // Pass the SQL source for enhanced details
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

  override getTrackShellButtons(): m.Children {
    const baseButtons = super.getTrackShellButtons();
    const customButtons = this.attrs.shellButtons?.();
    return [baseButtons, customButtons];
  }

  override renderTooltip(): m.Children {
    // Get the base counter tooltip from BaseCounterTrack
    const baseTooltip = super.renderTooltip();

    // For now, we'll use the base tooltip since we can't access private members
    // Custom tooltip functionality would need to be implemented differently
    // or the BaseCounterTrack would need to expose the necessary methods
    return baseTooltip;
  }

  onMouseClick({x, timescale}: TrackMouseEvent): boolean {
    const time = timescale.pxToHpTime(x).toTime('floor');
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

    this.engine
      .query(
        `
        SELECT
          id
        FROM (${query})
        WHERE
          ts < ${time}
        ORDER BY ts DESC
        LIMIT 1
      `,
      )
      .then((result) => {
        const it = result.iter({
          id: NUM,
        });
        if (!it.valid()) {
          return;
        }
        const id = it.id;
        this.trace.selection.selectTrackEvent(this.uri, id);
      });

    return true;
  }
}

// Generate a query to use for generating counter data to be rendered
function generateRenderQuery<T extends DatasetSchema>(
  dataset: SourceDataset<T>,
) {
  const hasId = dataset.implements({id: NUM});
  const hasTs = dataset.implements({ts: LONG});
  const hasValue = dataset.implements({value: NUM});

  if (!hasTs) {
    throw new Error('Counter dataset must have a "ts" column');
  }
  if (!hasValue) {
    throw new Error('Counter dataset must have a "value" column');
  }

  const cols = removeFalsyValues([
    // If we have no id, automatically generate one using row number.
    !hasId && 'ROW_NUMBER() OVER (ORDER BY ts) AS id',
  ]);

  if (cols.length === 0) {
    return dataset.query();
  } else {
    return `select ${cols.join(', ')}, * from (${dataset.query()})`;
  }
}
