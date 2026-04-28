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

import {Row as DataGridRow} from '../../trace_processor/query_result';
import {SettingFilter} from '../settings/settings_types';

export class HttpDataSource {
  private static readonly DEFAULT_LIMIT = 1000000;

  private endpoint: string;
  private baseQuery: string;
  private limit: number;
  private settings: SettingFilter[];
  private cachedData: DataGridRow[] | null = null;
  private fetchPromise: Promise<DataGridRow[]> | null = null;
  private abortController: AbortController | null = null;

  constructor(
    endpoint: string,
    baseQuery: string,
    limit = HttpDataSource.DEFAULT_LIMIT,
    settings: SettingFilter[],
  ) {
    this.endpoint = endpoint;
    this.baseQuery = baseQuery;
    this.limit = limit;
    this.settings = settings;
  }

  private async fetchData(forceRefresh = false): Promise<DataGridRow[]> {
    if (forceRefresh) {
      this.cachedData = null;
      this.fetchPromise = null;
    }

    if (this.cachedData !== null) {
      return this.cachedData;
    }

    if (this.fetchPromise !== null) {
      return this.fetchPromise;
    }

    this.fetchPromise = this.performFetch();
    try {
      this.cachedData = await this.fetchPromise;
      return this.cachedData;
    } finally {
      this.fetchPromise = null;
    }
  }

  private async performFetch(): Promise<DataGridRow[]> {
    const url = `${this.endpoint}/execute_bigtrace_query`;

    const serializedSettings = this.settings.map((s) => ({
      setting_id: s.settingId,
      values: s.values,
      category: s.category,
    }));

    const data = {
      limit: this.limit,
      perfetto_sql: this.baseQuery,
      settings: serializedSettings,
    };

    this.abortController = new AbortController();

    try {
      const response = await fetch(url, {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
        },
        body: JSON.stringify(data),
        credentials: 'include',
        mode: 'cors',
        signal: this.abortController.signal,
      });

      if (!response.ok) {
        let errorText = '';
        try {
          errorText = await response.text();
        } catch (e) {
          errorText = 'Could not read error body';
        }
        if (response.status === 403) {
          throw new Error(
            `HTTP error! status: ${response.status}. This might be an authentication issue. Please make sure you are logged in to the correct Google account. Backend says: ${errorText}`,
          );
        }
        throw new Error(
          `HTTP error! status: ${response.status}, backend says: ${errorText}`,
        );
      }

      const result = await response.json();

      if (
        result.columnNames !== undefined &&
        result.columnNames !== null &&
        result.rows !== undefined &&
        result.rows !== null
      ) {
        return result.rows.map(
          (row: {values: Array<string | number | null>}) => {
            const rowObject: DataGridRow = {};
            result.columnNames.forEach((header: string, index: number) => {
              if (header === null) return;
              const value = row.values[index];
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
      if (error instanceof DOMException && error.name === 'AbortError') {
        throw new Error('Query was cancelled.');
      }
      throw error;
    }
  }

  abort(): void {
    this.abortController?.abort();
  }

  async query(forceRefresh = false): Promise<DataGridRow[]> {
    return this.fetchData(forceRefresh);
  }

  clearCache(): void {
    this.cachedData = null;
    this.fetchPromise = null;
  }
}
