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
import {RawQueryExecution} from './query_history_storage';

interface QueryResponsePayload {
  columnNames?: string[];
  rows?: Array<{values: Array<string | number | null>}>;
}

export class HttpDataSource {
  private static readonly DEFAULT_LIMIT = 1000000;

  private endpoint: string;
  private baseQuery: string;
  private limit: number;
  private settings: SettingFilter[];
  private cachedData: {rows: DataGridRow[]; columns: string[]} | null = null;
  private fetchPromise: Promise<{
    rows: DataGridRow[];
    columns: string[];
  }> | null = null;
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

  private async fetchData(
    forceRefresh = false,
  ): Promise<{rows: DataGridRow[]; columns: string[]}> {
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

  private async performFetch(
    urlPath = '/execute_bigtrace_query',
  ): Promise<{rows: DataGridRow[]; columns: string[]}> {
    const url = `${this.endpoint}${urlPath}`;

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
            `HTTP error! status: ${response.status}. This might be an authentication issue. Please ensure you are logged in with the correct credentials. Backend says: ${errorText}`,
          );
        }
        throw new Error(
          `HTTP error! status: ${response.status}, backend says: ${errorText}`,
        );
      }

      const result = await response.json();
      return this.parseResponse(result);
    } catch (error) {
      if (error instanceof DOMException && error.name === 'AbortError') {
        throw new Error('Query was cancelled.');
      }
      throw error;
    }
  }

  private parseResponse(result: QueryResponsePayload): {
    rows: DataGridRow[];
    columns: string[];
  } {
    const columns: string[] = [];
    const colNames = result.columnNames;
    if (
      colNames !== undefined &&
      colNames !== null &&
      result.rows !== undefined &&
      result.rows !== null
    ) {
      colNames.forEach((header: string) => {
        if (header !== null) columns.push(header);
      });

      const rows = result.rows.map(
        (row: {values: Array<string | number | null>}) => {
          const rowObject: DataGridRow = {};
          colNames.forEach((header: string, index: number) => {
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
      return {rows, columns};
    }
    return {rows: [], columns: []};
  }

  abort(): void {
    this.abortController?.abort();
  }

  async query(
    forceRefresh = false,
  ): Promise<{rows: DataGridRow[]; columns: string[]}> {
    return this.fetchData(forceRefresh);
  }

  async executeAsync(): Promise<{rows: DataGridRow[]; columns: string[]}> {
    return this.performFetch('/execute_bigtrace_query_async');
  }

  async getStatus(uuid: string): Promise<RawQueryExecution> {
    const url = `${this.endpoint}/query_executions/${uuid}:status`;
    const response = await fetch(url, {
      method: 'GET',
      credentials: 'include',
      mode: 'cors',
    });
    if (!response.ok) {
      let errorText = '';
      try {
        errorText = await response.text();
      } catch (e) {
        errorText = 'Failed to read response body';
      }
      throw new Error(
        `HTTP error! status: ${response.status}, message: ${errorText}`,
      );
    }
    return response.json() as Promise<RawQueryExecution>;
  }

  async getQueryExecution(uuid: string): Promise<RawQueryExecution> {
    const url = `${this.endpoint}/query_executions/${uuid}`;
    const response = await fetch(url, {
      method: 'GET',
      credentials: 'include',
      mode: 'cors',
    });
    if (!response.ok) {
      let errorText = '';
      try {
        errorText = await response.text();
      } catch (e) {
        errorText = 'Failed to read response body';
      }
      throw new Error(
        `HTTP error! status: ${response.status}, message: ${errorText}`,
      );
    }
    return response.json() as Promise<RawQueryExecution>;
  }

  async fetchResults(
    uuid: string,
    limit: number,
    offset: number,
  ): Promise<{rows: DataGridRow[]; columns: string[]}> {
    const url = new URL(
      `${this.endpoint}/query_executions/${uuid}:fetch_results`,
    );
    url.searchParams.append('limit', limit.toString());
    url.searchParams.append('offset', offset.toString());

    const response = await fetch(url.toString(), {
      method: 'GET',
      credentials: 'include',
      mode: 'cors',
    });

    if (!response.ok) {
      let errorText = '';
      try {
        errorText = await response.text();
      } catch (e) {
        errorText = 'Failed to read response body';
      }
      throw new Error(
        `HTTP error! status: ${response.status}, message: ${errorText}`,
      );
    }

    const result = await response.json();
    console.log('fetchResults raw response:', JSON.stringify(result));
    return this.parseResponse(result);
  }

  async cancelQuery(uuid: string): Promise<void> {
    const url = `${this.endpoint}/query_executions/${uuid}:cancel`;
    const response = await fetch(url, {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
      },
      body: JSON.stringify({}),
      credentials: 'include',
      mode: 'cors',
    });
    if (!response.ok) {
      let errorText = '';
      try {
        errorText = await response.text();
      } catch (e) {
        errorText = 'Failed to read response body';
      }
      throw new Error(
        `HTTP error! status: ${response.status}, message: ${errorText}`,
      );
    }
  }

  async listQueryExecutions(): Promise<RawQueryExecution[]> {
    const url = `${this.endpoint}/query_executions`;
    const response = await fetch(url, {
      method: 'GET',
      credentials: 'include',
      mode: 'cors',
    });
    if (!response.ok) {
      let errorText = '';
      try {
        errorText = await response.text();
      } catch (e) {
        errorText = 'Failed to read response body';
      }
      throw new Error(
        `HTTP error! status: ${response.status}, message: ${errorText}`,
      );
    }
    const result = (await response.json()) as {
      queryExecutions?: RawQueryExecution[];
    };
    return result.queryExecutions !== undefined ? result.queryExecutions : [];
  }

  clearCache(): void {
    this.cachedData = null;
    this.fetchPromise = null;
  }
}
