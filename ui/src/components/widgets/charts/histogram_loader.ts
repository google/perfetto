// Copyright (C) 2026 The Android Open Source Project
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

import {QuerySlot, SerialTaskQueue} from '../../../base/query_slot';
import {Engine} from '../../../trace_processor/engine';
import {NUM} from '../../../trace_processor/query_result';

/**
 * A single bucket in the histogram.
 */
export interface HistogramBucket {
  /** Start value of the bucket (inclusive) */
  readonly start: number;
  /** End value of the bucket (exclusive) */
  readonly end: number;
  /** Count of values in this bucket */
  readonly count: number;
}

/**
 * Data provided by a histogram data source.
 */
export interface HistogramData {
  /** The computed buckets */
  readonly buckets: readonly HistogramBucket[];
  /** Minimum value in the data */
  readonly min: number;
  /** Maximum value in the data */
  readonly max: number;
  /** Total count of all values */
  readonly totalCount: number;
  /** Count of null values (excluded from histogram) */
  readonly nullCount: number;
  /** Count of non-numeric values (excluded from histogram) */
  readonly nonNumericCount: number;
}

/**
 * Configuration for histogram bucket computation.
 */
export interface HistogramConfig {
  /**
   * Fixed bucket size. If provided, bucketCount is ignored.
   * The number of buckets will be determined by (max - min) / bucketSize.
   */
  readonly bucketSize?: number;

  /**
   * Number of buckets to create. Ignored if bucketSize is provided.
   * Defaults to automatic calculation using Terrell-Scott rule.
   */
  readonly bucketCount?: number;

  /**
   * Minimum value for the histogram range.
   * If not provided, will be computed from the data.
   */
  readonly minValue?: number;

  /**
   * Maximum value for the histogram range.
   * If not provided, will be computed from the data.
   */
  readonly maxValue?: number;

  /**
   * When true, indicates that the data contains only integer values.
   * Bucket boundaries will be snapped to integer values (bucket size
   * is ceiled to the nearest integer).
   */
  readonly integer?: boolean;
}

/**
 * Computes histogram data from an array of numbers.
 * This is a utility function for use by parent components.
 */
export function computeHistogram(
  values: readonly (number | null | undefined)[],
  config: HistogramConfig = {},
): HistogramData {
  // Filter to only valid numbers
  const validValues = values.filter(
    (v): v is number => typeof v === 'number' && !isNaN(v) && isFinite(v),
  );

  const nullCount = values.filter((v) => v === null || v === undefined).length;
  const nonNumericCount = values.length - validValues.length - nullCount;

  if (validValues.length === 0) {
    return {
      buckets: [],
      min: 0,
      max: 0,
      totalCount: 0,
      nullCount,
      nonNumericCount,
    };
  }

  // Compute min/max from data or use config values
  const dataMin = Math.min(...validValues);
  const dataMax = Math.max(...validValues);
  const min = config.minValue ?? dataMin;
  const max = config.maxValue ?? dataMax;

  // Handle edge case where all values are the same
  if (min === max) {
    return {
      buckets: [{start: min, end: min + 1, count: validValues.length}],
      min,
      max,
      totalCount: validValues.length,
      nullCount,
      nonNumericCount,
    };
  }

  // Determine bucket size
  let bucketSize: number;
  let bucketCount: number;

  if (config.bucketSize !== undefined) {
    bucketSize = config.bucketSize;
    if (config.integer) {
      bucketSize = Math.max(1, Math.ceil(bucketSize));
    }
    bucketCount = Math.ceil((max - min) / bucketSize);
  } else if (config.bucketCount !== undefined) {
    bucketCount = config.bucketCount;
    bucketSize = (max - min) / bucketCount;
    if (config.integer) {
      bucketSize = Math.max(1, Math.ceil(bucketSize));
      bucketCount = Math.ceil((max - min) / bucketSize);
    }
  } else {
    // Terrell-Scott rule: k = (2 * n)^(1/3)
    bucketCount = Math.max(
      5,
      Math.min(100, Math.ceil(Math.pow(2 * validValues.length, 1 / 3))),
    );
    bucketSize = (max - min) / bucketCount;
    if (config.integer) {
      bucketSize = Math.max(1, Math.ceil(bucketSize));
      bucketCount = Math.ceil((max - min) / bucketSize);
    }
  }

  // Initialize buckets
  const buckets: HistogramBucket[] = [];
  for (let i = 0; i < bucketCount; i++) {
    buckets.push({
      start: min + i * bucketSize,
      end: min + (i + 1) * bucketSize,
      count: 0,
    });
  }

  // Count values into buckets
  for (const value of validValues) {
    if (value < min || value > max) continue;

    let bucketIndex = Math.floor((value - min) / bucketSize);
    // Handle edge case where value === max
    if (bucketIndex >= bucketCount) {
      bucketIndex = bucketCount - 1;
    }

    const bucket = buckets[bucketIndex];
    buckets[bucketIndex] = {...bucket, count: bucket.count + 1};
  }

  return {
    buckets,
    min,
    max,
    totalCount: validValues.length,
    nullCount,
    nonNumericCount,
  };
}

/**
 * Configuration for histogram loaders.
 */
export interface HistogramLoaderConfig {
  /**
   * Fixed bucket size. If provided, bucketCount is ignored.
   */
  readonly bucketSize?: number;

  /**
   * Number of buckets to create. Ignored if bucketSize is provided.
   */
  readonly bucketCount?: number;

  /**
   * Minimum value for the histogram range.
   */
  readonly minValue?: number;

  /**
   * Maximum value for the histogram range.
   */
  readonly maxValue?: number;

  /**
   * When true, indicates that the data contains only integer values.
   * Filter bounds will be snapped: min is floored, max is ceiled.
   * Bucket boundaries will also snap to integers.
   */
  readonly integer?: boolean;

  /**
   * Range filter to apply to the data (e.g., from brush selection).
   * Only values within [min, max] are included.
   */
  readonly filter?: {
    readonly min: number;
    readonly max: number;
  };
}

/**
 * Result returned by histogram loaders.
 */
export interface HistogramLoaderResult {
  /**
   * The computed histogram data, or undefined if loading.
   */
  readonly data: HistogramData | undefined;

  /**
   * Whether a query is currently pending.
   */
  readonly isPending: boolean;
}

/**
 * Loader interface for histogram data.
 */
export interface HistogramLoader {
  use(config: HistogramLoaderConfig): HistogramLoaderResult;
  dispose(): void;
}

/**
 * In-memory histogram loader for static datasets.
 *
 * Takes values in the constructor and computes histograms synchronously.
 * Caches the result when config hasn't changed.
 *
 * Usage:
 * ```typescript
 * class MyComponent {
 *   private loader = new InMemoryHistogramLoader(myStaticValues);
 *
 *   view() {
 *     const {data} = this.loader.use({bucketCount: 20});
 *     return m(Histogram, {data});
 *   }
 * }
 * ```
 */
export class InMemoryHistogramLoader implements HistogramLoader {
  private readonly values: readonly number[];
  private cachedConfig?: string;
  private cachedData?: HistogramData;

  constructor(values: readonly (number | null | undefined)[]) {
    // Pre-filter to valid numbers
    this.values = values.filter(
      (v): v is number => typeof v === 'number' && !isNaN(v) && isFinite(v),
    );
  }

  use(config: HistogramLoaderConfig): HistogramLoaderResult {
    const configKey = JSON.stringify(config);

    // Return cached result if config unchanged
    if (this.cachedConfig === configKey && this.cachedData !== undefined) {
      return {data: this.cachedData, isPending: false};
    }

    // Apply filter if provided, snapping bounds for integer data
    let filteredValues = this.values;
    if (config.filter) {
      const filterMin = config.integer
        ? Math.floor(config.filter.min)
        : config.filter.min;
      const filterMax = config.integer
        ? Math.ceil(config.filter.max)
        : config.filter.max;
      filteredValues = this.values.filter(
        (v) => v >= filterMin && v <= filterMax,
      );
    }

    // Compute histogram
    const data = computeHistogram(filteredValues, {
      bucketSize: config.bucketSize,
      bucketCount: config.bucketCount,
      minValue: config.minValue,
      maxValue: config.maxValue,
      integer: config.integer,
    });

    // Cache result
    this.cachedConfig = configKey;
    this.cachedData = data;

    return {data, isPending: false};
  }

  dispose(): void {
    // No-op for in-memory loader, but keeps interface consistent
    this.cachedConfig = undefined;
    this.cachedData = undefined;
  }
}

/**
 * Configuration for SQLHistogramLoader.
 */
export interface SQLHistogramLoaderOpts {
  /**
   * The trace processor engine to run queries against.
   */
  readonly engine: Engine;

  /**
   * SQL query that returns numeric values for the histogram.
   * The query should include the valueColumn in its output.
   */
  readonly query: string;

  /**
   * Column name to use for histogram values.
   * This column must exist in the query output.
   */
  readonly valueColumn: string;
}

/**
 * SQL-based histogram loader with async loading and caching.
 *
 * Performs histogram aggregation directly in SQL for efficiency with large
 * datasets. Uses QuerySlot internally for caching and request deduplication.
 *
 * Usage:
 * ```typescript
 * class SQLHistogramPanel {
 *   private loader: SQLHistogramLoader;
 *   private brushRange?: {min: number; max: number};
 *
 *   constructor(engine: Engine) {
 *     this.loader = new SQLHistogramLoader({
 *       engine,
 *       query: 'SELECT dur FROM slice WHERE dur > 0',
 *       valueColumn: 'dur',
 *     });
 *   }
 *
 *   view() {
 *     const {data, isPending} = this.loader.use({
 *       bucketCount: 20,
 *       filter: this.brushRange,
 *     });
 *     return m(Histogram, {
 *       data,
 *       onBrush: (range) => {
 *         this.brushRange = range ? {min: range.start, max: range.end} : undefined;
 *       },
 *     });
 *   }
 *
 *   onremove() {
 *     this.loader.dispose();
 *   }
 * }
 * ```
 */
// Default bucket count for SQL aggregation
const DEFAULT_BUCKET_COUNT = 20;

export class SQLHistogramLoader implements HistogramLoader {
  private readonly engine: Engine;
  private readonly baseQuery: string;
  private readonly valueColumn: string;
  private readonly taskQueue = new SerialTaskQueue();
  private readonly querySlot = new QuerySlot<HistogramData>(this.taskQueue);

  /**
   * Creates a new SQL histogram loader.
   */
  constructor(opts: SQLHistogramLoaderOpts) {
    this.engine = opts.engine;
    this.baseQuery = opts.query;
    this.valueColumn = opts.valueColumn;
  }

  use(config: HistogramLoaderConfig): HistogramLoaderResult {
    const bucketCount = config.bucketCount ?? DEFAULT_BUCKET_COUNT;

    // Snap filter bounds for integer data
    const filter = config.filter
      ? config.integer
        ? {
            min: Math.floor(config.filter.min),
            max: Math.ceil(config.filter.max),
          }
        : config.filter
      : undefined;

    const result = this.querySlot.use({
      key: {
        baseQuery: this.baseQuery,
        valueColumn: this.valueColumn,
        filter,
        bucketCount,
        integer: config.integer,
      },
      queryFn: async () => {
        const col = this.valueColumn;
        const filterClause = filter
          ? `WHERE ${col} >= ${filter.min} AND ${col} <= ${filter.max}`
          : '';

        // SQL query that computes histogram buckets in the database
        // Uses scalar subqueries for bounds (SQLite caches these) to avoid
        // expensive cross joins
        const sql = `
          WITH _data AS (
            SELECT ${col} AS _value
            FROM (${this.baseQuery})
            ${filterClause}
          )
          SELECT
            (SELECT MIN(_value) FROM _data) AS _min,
            (SELECT MAX(_value) FROM _data) AS _max,
            (SELECT COUNT(*) FROM _data) AS _total,
            CASE
              WHEN (SELECT MAX(_value) FROM _data) = (SELECT MIN(_value) FROM _data) THEN 0
              WHEN _value = (SELECT MAX(_value) FROM _data) THEN ${bucketCount - 1}
              ELSE MIN(${bucketCount - 1}, CAST(
                (_value - (SELECT MIN(_value) FROM _data)) /
                (((SELECT MAX(_value) FROM _data) - (SELECT MIN(_value) FROM _data)) / ${bucketCount}.0)
              AS INT))
            END AS _bucket_idx,
            COUNT(*) AS _count
          FROM _data
          GROUP BY _bucket_idx
          ORDER BY _bucket_idx
        `;

        const queryResult = await this.engine.query(sql);

        // Parse results into HistogramData
        let min = 0;
        let max = 0;
        let totalCount = 0;
        const bucketCounts = new Map<number, number>();

        const iter = queryResult.iter({
          _min: NUM,
          _max: NUM,
          _total: NUM,
          _bucket_idx: NUM,
          _count: NUM,
        });
        for (; iter.valid(); iter.next()) {
          min = iter._min;
          max = iter._max;
          totalCount = iter._total;
          bucketCounts.set(iter._bucket_idx, iter._count);
        }

        // Handle empty data
        if (totalCount === 0) {
          return {
            buckets: [],
            min: 0,
            max: 0,
            totalCount: 0,
            nullCount: 0,
            nonNumericCount: 0,
          };
        }

        // Build bucket array (including empty buckets)
        let bucketSize = (max - min) / bucketCount;
        if (config.integer) {
          bucketSize = Math.max(1, Math.ceil(bucketSize));
        }
        const buckets: HistogramBucket[] = [];
        for (let i = 0; i < bucketCount; i++) {
          buckets.push({
            start: min + i * bucketSize,
            end: min + (i + 1) * bucketSize,
            count: bucketCounts.get(i) ?? 0,
          });
        }

        return {
          buckets,
          min,
          max,
          totalCount,
          nullCount: 0,
          nonNumericCount: 0,
        };
      },
    });

    return {
      data: result.data,
      isPending: result.isPending,
    };
  }

  dispose(): void {
    this.querySlot.dispose();
  }
}
