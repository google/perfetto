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
import {
  BigtraceQueryClient,
  type DuckdbTableDescriptor,
} from './bigtrace_query_client';
import {getBigtraceEndpoint} from '../settings/endpoint_storage';

export class DuckdbTablesStore {
  private tables: ReadonlyArray<DuckdbTableDescriptor> = [];
  private lastEndpoint = '';
  private isFetching = false;
  private hasFetched = false;

  getTables(): ReadonlyArray<DuckdbTableDescriptor> {
    return this.tables;
  }

  hasLoaded(): boolean {
    return this.hasFetched;
  }

  async fetchIfNeeded(): Promise<void> {
    const endpoint = getBigtraceEndpoint().trim();
    if (!endpoint) return;
    if (endpoint !== this.lastEndpoint || !this.hasFetched) {
      await this.refresh();
    }
  }

  async refresh(): Promise<void> {
    const endpoint = getBigtraceEndpoint().trim();
    if (!endpoint || this.isFetching) return;
    this.isFetching = true;
    this.lastEndpoint = endpoint;
    const client = new BigtraceQueryClient(endpoint);
    try {
      this.tables = await client.listDuckdbTables();
      this.hasFetched = true;
    } catch {
      // Retry once after 500ms on failure
      await new Promise((r) => setTimeout(r, 500));
      try {
        this.tables = await client.listDuckdbTables();
        this.hasFetched = true;
      } catch {
        // Keep previous tables
      }
    } finally {
      this.isFetching = false;
      m.redraw();
    }
  }

  invalidate(): void {
    this.tables = [];
    this.hasFetched = false;
    this.lastEndpoint = '';
  }
}

export const duckdbTablesStore = new DuckdbTablesStore();
