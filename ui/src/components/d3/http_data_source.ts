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

import {DataSource} from '../../widgets/charts/d3/data/source';
import {MemorySource} from '../../widgets/charts/d3/data/memory_source';
import {ChartSpec, Filter, Row} from '../../widgets/charts/d3/data/types';

/**
 * DataSource implementation that queries the Brush backend API.
 * Currently fetches all data once and applies filters/aggregations in-memory.
 * This makes it easy to switch to server-side filtering when backend support is added.
 */
export class HttpDataSource implements DataSource {
  private static readonly BRUSH_API_URL =
    'https://brush-googleapis.corp.google.com/v1/bigtrace/query';
  private static readonly DEFAULT_LIMIT = 10000;

  private baseQuery: string;
  private traceAddress: string;
  private limit: number;
  private cachedData: Row[] | null = null;
  private fetchPromise: Promise<Row[]> | null = null;

  constructor(
    baseQuery: string,
    traceAddress = 'android_telemetry.field_trace_summaries_prod.last30days',
    limit = HttpDataSource.DEFAULT_LIMIT,
  ) {
    this.baseQuery = baseQuery;
    this.traceAddress = traceAddress;
    this.limit = limit;
  }

  /**
   * Fetches data from the Brush backend API.
   * Results are cached to avoid redundant network calls.
   * @param forceRefresh If true, bypasses cache and fetches fresh data
   */
  private async fetchData(forceRefresh = false): Promise<Row[]> {
    // Clear cache if refresh is requested
    if (forceRefresh) {
      this.cachedData = null;
      this.fetchPromise = null;
    }

    // Return cached data if available
    if (this.cachedData !== null) {
      return this.cachedData;
    }

    // If a fetch is already in progress, wait for it
    if (this.fetchPromise !== null) {
      return this.fetchPromise;
    }

    // Start a new fetch
    this.fetchPromise = this.performFetch();
    try {
      this.cachedData = await this.fetchPromise;
      return this.cachedData;
    } finally {
      this.fetchPromise = null;
    }
  }

  /**
   * Performs the actual HTTP request to the Brush backend.
   */
  private async performFetch(): Promise<Row[]> {
    const url = HttpDataSource.BRUSH_API_URL;

    // Construct the JSON payload matching the QueryRequest proto structure
    const data = {
      trace_address: this.traceAddress,
      limit: this.limit,
      perfetto_sql: this.baseQuery,
    };

    try {
      const response = await fetch(url, {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
        },
        body: JSON.stringify(data),
        credentials: 'include', // needed for UberProxy authentication cookies
        mode: 'cors',
      });

      if (!response.ok) {
        throw new Error(`HTTP error! status: ${response.status}`);
      }

      const result = await response.json();

      // Convert Brush response to Row[]
      if (
        result.columnNames !== undefined &&
        result.columnNames !== null &&
        result.rows !== undefined &&
        result.rows !== null
      ) {
        return result.rows.map(
          (row: {values: Array<string | number | null>}) => {
            const rowObject: Row = {};
            result.columnNames.forEach((header: string, index: number) => {
              if (header === null) return;
              const value = row.values[index];
              // Attempt to convert to number, keep as string if NaN
              const numValue = Number(value);
              rowObject[header] =
                value === null || value === 'NULL' || isNaN(numValue)
                  ? value
                  : numValue;
            });
            return rowObject;
          },
        );
      }

      return [];
    } catch (error) {
      console.error('Brush query error:', error);
      throw error;
    }
  }

  /**
   * Query method that fetches data and applies filters/aggregations in-memory.
   * When backend filter support is added, replace the in-memory filtering
   * with server-side filtering by passing filters to the API.
   * @param filters Filters to apply to the data
   * @param spec Chart specification for aggregation
   * @param forceRefresh If true, bypasses cache and fetches fresh data from backend
   */
  async query(
    filters: Filter[],
    spec: ChartSpec,
    forceRefresh = false,
  ): Promise<Row[]> {
    // Fetch all data (cached after first call unless forceRefresh is true)
    const allData = await this.fetchData(forceRefresh);

    // Use MemorySource until Brush API supports server-side aggregations
    const memorySource = new MemorySource(allData);
    return memorySource.query(filters, spec);
  }

  /**
   * Clears the cached data, forcing a fresh fetch on the next query.
   */
  clearCache(): void {
    this.cachedData = null;
    this.fetchPromise = null;
  }
}
