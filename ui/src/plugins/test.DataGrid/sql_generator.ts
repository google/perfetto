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

export interface ColumnMetadata {
  readonly key: string;
  readonly path: string[];
  readonly displayNameParts: string[];
}

export interface JoinContext {
  readonly path: string[];
  readonly tableAlias: string;
  readonly parentAlias: string;
}

export function generateSqlQuery(
  baseSql: string,
  columns: ReadonlyArray<ColumnMetadata>,
  sqlSchema: unknown,
): string {
  const trimmedBaseSql = baseSql.trim();
  if (!trimmedBaseSql) return '';

  if (columns.length === 0) {
    return trimmedBaseSql;
  }

  const aliasMap = new Map<string, string>();
  let aliasIndex = 0;

  function getAlias(prefixKey: string): string {
    if (!prefixKey) return 'base';
    if (!aliasMap.has(prefixKey)) {
      const char = String.fromCharCode('a'.charCodeAt(0) + (aliasIndex % 26));
      const loops = Math.floor(aliasIndex / 26);
      const alias = char + (loops > 0 ? loops : '');
      aliasMap.set(prefixKey, alias);
      aliasIndex++;
    }
    return aliasMap.get(prefixKey)!;
  }

  const joinClauses: string[] = [];
  const processedPrefixes = new Set<string>();

  const schemaRecord =
    sqlSchema && typeof sqlSchema === 'object'
      ? (sqlSchema as Record<string, unknown>)
      : {};

  // Pre-register all aliases in prefix order to guarantee nice alphabetical ordering
  for (const col of columns) {
    const path = col.path;
    for (let len = 1; len < path.length; len++) {
      const prefixKey = path.slice(0, len).join('.');
      getAlias(prefixKey);
    }
  }

  for (const col of columns) {
    const path = col.path;

    for (let len = 1; len < path.length; len++) {
      const prefixPath = path.slice(0, len);
      const prefixKey = prefixPath.join('.');

      if (processedPrefixes.has(prefixKey)) continue;
      processedPrefixes.add(prefixKey);

      let currentSchema: Record<string, unknown> = schemaRecord;
      let entry: unknown = undefined;

      for (let i = 0; i < prefixPath.length; i++) {
        const segment = prefixPath[i];
        if (!currentSchema || typeof currentSchema !== 'object') {
          entry = undefined;
          break;
        }
        entry = currentSchema[segment];
        if (entry && typeof entry === 'object') {
          const nestedSchema = (entry as {schema?: unknown}).schema;
          if (nestedSchema && typeof nestedSchema === 'object') {
            currentSchema = nestedSchema as Record<string, unknown>;
          } else {
            currentSchema = {};
          }
        } else {
          currentSchema = {};
        }
      }

      if (entry && typeof entry === 'object') {
        const entryObj = entry as {join?: (ctx: JoinContext) => unknown};
        if (typeof entryObj.join === 'function') {
          const tableAlias = getAlias(prefixKey);
          const parentPrefixKey = prefixPath.slice(0, -1).join('.');
          const parentAlias = getAlias(parentPrefixKey);
          const ctx: JoinContext = {
            path: prefixPath,
            tableAlias,
            parentAlias,
          };
          try {
            const joinStr = entryObj.join(ctx);
            if (typeof joinStr === 'string') {
              joinClauses.push(joinStr.trim());
            }
          } catch (err) {
            console.error(
              'Error executing join function for path:',
              prefixKey,
              err,
            );
          }
        }
      }
    }
  }

  const selectList = columns
    .map((col) => {
      const path = col.path;

      let customSelectStr: string | undefined = undefined;
      let selectSchema = schemaRecord;

      for (let len = 1; len < path.length; len++) {
        const prefixPath = path.slice(0, len);
        const segment = prefixPath[prefixPath.length - 1];

        if (selectSchema && typeof selectSchema === 'object') {
          const entry = selectSchema[segment];
          if (entry && typeof entry === 'object') {
            const entryObj = entry as {
              select?: (param: string, ctx: JoinContext) => string;
              schema?: unknown;
            };

            if (
              typeof entryObj.select === 'function' &&
              len === path.length - 1
            ) {
              const param = path[path.length - 1].replace(/'/g, "''");
              const prefixKey = prefixPath.join('.');
              const tableAlias = getAlias(prefixKey);
              const parentPrefixKey = prefixPath.slice(0, -1).join('.');
              const parentAlias = getAlias(parentPrefixKey);
              const ctx: JoinContext = {
                path: prefixPath,
                tableAlias,
                parentAlias,
              };
              try {
                customSelectStr = entryObj.select(param, ctx);
              } catch (err) {
                console.error(
                  'Error executing select function for path:',
                  prefixKey,
                  err,
                );
              }
              break;
            }

            const nestedSchema = entryObj.schema;
            if (nestedSchema && typeof nestedSchema === 'object') {
              selectSchema = nestedSchema as Record<string, unknown>;
            } else {
              selectSchema = {};
            }
          } else {
            selectSchema = {};
          }
        } else {
          selectSchema = {};
        }
      }

      if (customSelectStr !== undefined) {
        return `  ${customSelectStr} AS \`${col.key}\``;
      }

      if (path.length > 1) {
        const prefixKey = path.slice(0, -1).join('.');
        const tableAlias = getAlias(prefixKey);
        const leaf = path[path.length - 1];
        return `  ${tableAlias}.${leaf} AS \`${col.key}\``;
      } else {
        return `  base.${col.key} AS \`${col.key}\``;
      }
    })
    .join(',\n');

  const joinBlock =
    joinClauses.length > 0
      ? '\n' + joinClauses.map((clause) => `${clause}`).join('\n')
      : '';

  return `SELECT\n${selectList}\nFROM (\n  ${trimmedBaseSql.replace(/\n/g, '\n  ')}\n) AS base${joinBlock}`;
}
