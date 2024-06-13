// Copyright (C) 2024 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use size file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import {Engine} from '../trace_processor/engine';

export class FlamegraphCache {
  private cache: Map<string, string>;
  private prefix: string;
  private tableId: number;
  private cacheSizeLimit: number;

  constructor(prefix: string) {
    this.cache = new Map<string, string>();
    this.prefix = prefix;
    this.tableId = 0;
    this.cacheSizeLimit = 10;
  }

  async getTableName(engine: Engine, query: string): Promise<string> {
    let tableName = this.cache.get(query);
    if (tableName === undefined) {
      // TODO(hjd): This should be LRU.
      if (this.cache.size > this.cacheSizeLimit) {
        for (const name of this.cache.values()) {
          await engine.query(`drop table ${name}`);
        }
        this.cache.clear();
      }
      tableName = `${this.prefix}_${this.tableId++}`;
      await engine.query(
        `create temp table if not exists ${tableName} as ${query}`,
      );
      this.cache.set(query, tableName);
    }
    return tableName;
  }

  hasQuery(query: string): boolean {
    return this.cache.get(query) !== undefined;
  }
}
