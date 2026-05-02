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

import m from 'mithril';

export interface QueryExecution {
  uuid: string;
  status: string;
  startTime?: number;
  endTime?: number;
  processedRows: number;
  processedTraces: number;
  totalTraces: number;
  error?: string;
  perfettoSql?: string;
  limit?: number;
  materialized?: boolean;
  tableName?: string;
  tableLink?: string;
}

class QueryStore {
  private queries = new Map<string, QueryExecution>();

  getOrCreate(
    uuid: string,
    initialData?: Partial<QueryExecution>,
  ): QueryExecution {
    if (!this.queries.has(uuid)) {
      this.queries.set(uuid, {
        uuid,
        status: 'UNKNOWN',
        processedRows: 0,
        processedTraces: 0,
        totalTraces: 0,
        ...initialData,
      });
    }
    const obj = this.queries.get(uuid)!;
    if (initialData) {
      // Handle date strings to timestamps conversion if needed
      const dataToMerge = {...initialData};
      if (typeof dataToMerge.startTime === 'string') {
        dataToMerge.startTime = new Date(dataToMerge.startTime).getTime();
      }
      if (typeof dataToMerge.endTime === 'string') {
        dataToMerge.endTime = new Date(dataToMerge.endTime).getTime();
      }
      Object.assign(obj, dataToMerge);
    }
    return obj;
  }

  update(uuid: string, updates: Partial<QueryExecution>): void {
    const obj = this.queries.get(uuid);
    if (obj) {
      // Handle date strings to timestamps conversion if needed
      const dataToMerge = {...updates};
      if (typeof dataToMerge.startTime === 'string') {
        dataToMerge.startTime = new Date(dataToMerge.startTime).getTime();
      }
      if (typeof dataToMerge.endTime === 'string') {
        dataToMerge.endTime = new Date(dataToMerge.endTime).getTime();
      }
      Object.assign(obj, dataToMerge);
      m.redraw();
    }
  }

  getAll(): QueryExecution[] {
    return Array.from(this.queries.values());
  }
}

export const queryStore = new QueryStore();
