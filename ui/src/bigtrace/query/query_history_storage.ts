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

import {LocalStorage} from '../../core/local_storage';
import {BIGTRACE_SETTINGS_STORAGE_KEY} from '../settings/settings_storage';

const QUERY_HISTORY_ENTRY_SCHEMA = z.object({
  query: z.string(),
  timestamp: z.number(),
  starred: z.boolean().default(false),
});

export type QueryHistoryEntry = z.infer<typeof QUERY_HISTORY_ENTRY_SCHEMA>;

const QUERY_HISTORY_SCHEMA = z.array(QUERY_HISTORY_ENTRY_SCHEMA);

export type QueryHistory = z.infer<typeof QUERY_HISTORY_SCHEMA>;

export class QueryHistoryStorage {
  private _data: QueryHistory;
  maxItems = 50;
  private storage: LocalStorage;

  constructor() {
    this.storage = new LocalStorage(BIGTRACE_SETTINGS_STORAGE_KEY);
    this._data = this.load();
  }

  get data(): QueryHistory {
    return this._data;
  }

  saveQuery(query: string): void {
    // If query already exists, move it to the front preserving starred status
    const existingIndex = this._data.findIndex(
      (entry) => entry.query === query,
    );
    if (existingIndex !== -1) {
      const existing = this._data[existingIndex];
      this._data.splice(existingIndex, 1);
      this._data.unshift({
        query,
        timestamp: Date.now(),
        starred: existing.starred,
      });
      this.save();
      return;
    }

    // Count unstarred items and find the oldest one
    let lastUnstarredIndex = -1;
    let countUnstarred = 0;
    for (let i = 0; i < this._data.length; i++) {
      if (!this._data[i].starred) {
        countUnstarred++;
        lastUnstarredIndex = i;
      }
    }

    // Remove oldest unstarred if at capacity
    if (countUnstarred >= this.maxItems && lastUnstarredIndex !== -1) {
      this._data.splice(lastUnstarredIndex, 1);
    }

    this._data.unshift({
      query,
      timestamp: Date.now(),
      starred: false,
    });

    this.save();
  }

  setStarred(index: number, starred: boolean): void {
    if (index >= 0 && index < this._data.length) {
      this._data[index].starred = starred;
      this.save();
    }
  }

  remove(index: number): void {
    if (index >= 0 && index < this._data.length) {
      this._data.splice(index, 1);
      this.save();
    }
  }

  private load(): QueryHistory {
    const value = this.storage.load()['queries'];
    if (value === undefined) {
      return [];
    }
    const res = QUERY_HISTORY_SCHEMA.safeParse(value);
    return res.success ? res.data : [];
  }

  private save(): void {
    try {
      const data = this.storage.load();
      data['queries'] = this._data;
      this.storage.save(data);
    } catch (e) {
      console.warn('Failed to save query history to localStorage:', e);
    }
  }
}

export const queryHistoryStorage = new QueryHistoryStorage();
