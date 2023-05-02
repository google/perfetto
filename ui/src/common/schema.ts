// Copyright (C) 2023 The Android Open Source Project
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

import {EngineProxy} from './engine';
import {STR} from './query_result';

const CACHED_SCHEMAS = new WeakMap<EngineProxy, DatabaseSchema>();

export class SchemaError extends Error {
  constructor(message: string) {
    super(message);
  }
}

// POJO representing the table structure of trace_processor.
// Exposed for testing.
export interface DatabaseInfo {
  tables: TableInfo[];
}

interface TableInfo {
  name: string;
  parent?: TableInfo;
  columns: ColumnInfo[];
}

interface ColumnInfo {
  name: string;
}

async function getColumns(
    engine: EngineProxy, table: string): Promise<ColumnInfo[]> {
  const result = await engine.query(`PRAGMA table_info(${table});`);
  const it = result.iter({
    name: STR,
  });
  const columns = [];
  for (; it.valid(); it.next()) {
    columns.push({name: it['name']});
  }
  return columns;
}

// Opinionated view on the schema of the given trace_processor instance
// suitable for EventSets to use for query generation.
export class DatabaseSchema {
  private tableToKeys: Map<string, Set<string>>;

  constructor(info: DatabaseInfo) {
    this.tableToKeys = new Map();
    for (const tableInfo of info.tables) {
      const columns = new Set(tableInfo.columns.map((c) => c.name));
      this.tableToKeys.set(tableInfo.name, columns);
    }
  }

  // Return all the EventSet keys available for a given table. This
  // includes the direct columns on the table (and all parent tables)
  // as well as all direct and indirect joinable tables where the join
  // is N:1 or 1:1. e.g. for the table thread_slice we also include
  // the columns from thread, process, thread_track etc.
  getKeys(tableName: string): Set<string> {
    const columns = this.tableToKeys.get(tableName);
    if (columns === undefined) {
      throw new SchemaError(`Unknown table '${tableName}'`);
    }
    return columns;
  }
}

// Deliberately not exported. Users should call getSchema below and
// participate in cacheing.
async function createSchema(engine: EngineProxy): Promise<DatabaseSchema> {
  const tables: TableInfo[] = [];
  const result = await engine.query(`SELECT name from perfetto_tables;`);
  const it = result.iter({
    name: STR,
  });
  for (; it.valid(); it.next()) {
    const name = it['name'];
    tables.push({
      name,
      columns: await getColumns(engine, name),
    });
  }

  const database: DatabaseInfo = {
    tables,
  };

  return new DatabaseSchema(database);
}

// Get the schema for the given engine (from the cache if possible).
// The schemas are per-engine (i.e. they can't be statically determined
// at build time) since we might be in httpd mode and not-running
// against the version of trace_processor we build with.
export async function getSchema(engine: EngineProxy): Promise<DatabaseSchema> {
  const schema = CACHED_SCHEMAS.get(engine);
  if (schema === undefined) {
    const newSchema = await createSchema(engine);
    CACHED_SCHEMAS.set(engine, newSchema);
    return newSchema;
  }
  return schema;
}
