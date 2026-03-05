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

import {z} from 'zod';

const RECENT_QUERIES_KEY = 'recentBigTraceQueries';

const RECENT_QUERY_ENTRY_SCHEMA = z.object({
  query: z.string(),
  timestamp: z.number(),
});

export type RecentQueryEntry = z.infer<typeof RECENT_QUERY_ENTRY_SCHEMA>;

const RECENT_QUERIES_SCHEMA = z.array(RECENT_QUERY_ENTRY_SCHEMA);

export type RecentQueries = z.infer<typeof RECENT_QUERIES_SCHEMA>;

export class RecentQueriesStorage {
  private _data: RecentQueries;
  maxItems = 15;

  constructor() {
    this._data = this.load();
  }

  get data(): RecentQueries {
    return this._data;
  }

  saveQuery(query: string): void {
    // Remove duplicates
    this._data = this._data.filter((entry) => entry.query !== query);

    this._data.unshift({
      query,
      timestamp: Date.now(),
    });

    // Enforce maxItems limit
    if (this._data.length > this.maxItems) {
      this._data.pop();
    }

    this.save();
  }

  private load(): RecentQueries {
    const value = window.localStorage.getItem(RECENT_QUERIES_KEY);
    if (value === null) {
      return [];
    }
    try {
      const res = RECENT_QUERIES_SCHEMA.safeParse(JSON.parse(value));
      return res.success ? res.data : [];
    } catch {
      return [];
    }
  }

  private save(): void {
    try {
      window.localStorage.setItem(
          RECENT_QUERIES_KEY,
          JSON.stringify(this._data),
      );
    } catch (e) {
      console.warn('Failed to save recent queries to localStorage:', e);
    }
  }
}

export const recentQueriesStorage = new RecentQueriesStorage();
