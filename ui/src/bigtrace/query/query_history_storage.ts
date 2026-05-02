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

import {endpointStorage} from '../settings/endpoint_storage';
import {QueryExecution} from './query_store';

export interface RawQueryExecution {
  queryUuid?: string;
  status?: string;
  startTime?: string;
  endTime?: string;
  processedRows?: number;
  processedTraces?: number;
  totalTraces?: number;
  error?: string;
  errorMessage?: string;
  perfettoSql?: string;
  limit?: string | number;
  materialized?: boolean;
  tableName?: string;
  tableLink?: string;
}

export class QueryHistoryStorage {
  async getAllHistory(): Promise<QueryExecution[]> {
    return this.fetchHistory();
  }

  async getMaterializedHistory(): Promise<QueryExecution[]> {
    const all = await this.getAllHistory();
    return all.filter((item) => item.materialized === true);
  }

  async getNonMaterializedHistory(): Promise<QueryExecution[]> {
    const all = await this.getAllHistory();
    return all.filter((item) => item.materialized !== true);
  }

  async deleteQuery(uuid: string): Promise<void> {
    const endpointSetting = endpointStorage.get('bigtraceEndpoint');
    const endpoint = endpointSetting ? (endpointSetting.get() as string) : '';
    const url = `${endpoint}/query_executions/${uuid}`;
    const response = await fetch(url, {
      method: 'DELETE',
      credentials: 'include',
      mode: 'cors',
    });
    if (!response.ok) {
      throw new Error(`HTTP error! status: ${response.status}`);
    }
  }

  private async fetchHistory(): Promise<QueryExecution[]> {
    const endpointSetting = endpointStorage.get('bigtraceEndpoint');
    const endpoint = endpointSetting ? (endpointSetting.get() as string) : '';
    const url = `${endpoint}/query_executions`;
    const response = await fetch(url, {
      method: 'GET',
      credentials: 'include',
      mode: 'cors',
    });
    if (!response.ok) {
      throw new Error(`HTTP error! status: ${response.status}`);
    }
    const result = (await response.json()) as {
      queryExecutions?: RawQueryExecution[];
    };
    const list =
      result.queryExecutions !== undefined ? result.queryExecutions : [];

    const mappedList: QueryExecution[] = list.map((raw) => ({
      uuid: raw.queryUuid || '',
      status: raw.status || 'UNKNOWN',
      startTime:
        raw.startTime !== undefined
          ? new Date(raw.startTime).getTime()
          : undefined,
      endTime:
        raw.endTime !== undefined ? new Date(raw.endTime).getTime() : undefined,
      processedRows: raw.processedRows !== undefined ? raw.processedRows : 0,
      processedTraces:
        raw.processedTraces !== undefined ? raw.processedTraces : 0,
      totalTraces: raw.totalTraces !== undefined ? raw.totalTraces : 0,
      error: raw.error || raw.errorMessage,
      perfettoSql: raw.perfettoSql,
      limit: raw.limit !== undefined ? Number(raw.limit) : undefined,
      materialized: raw.materialized,
      tableName: raw.tableName,
      tableLink: raw.tableLink,
    }));

    mappedList.sort((a, b) => {
      const timeA = a.startTime !== undefined ? a.startTime : 0;
      const timeB = b.startTime !== undefined ? b.startTime : 0;
      return timeB - timeA;
    });
    return mappedList;
  }
}

export const queryHistoryStorage = new QueryHistoryStorage();
