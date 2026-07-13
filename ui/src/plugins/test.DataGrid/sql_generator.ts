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
  readonly colId: string;
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
  pivot?: unknown,
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
  
  const innerSchema = (sqlSchema && typeof sqlSchema === 'object') 
    ? (sqlSchema as {schema?: unknown}).schema 
    : undefined;
    
  const schemaRecord = (innerSchema && typeof innerSchema === 'object') 
    ? (innerSchema as Record<string, unknown>) 
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
      
      if (!entry || typeof entry !== 'object') {
        throw new Error(`No SQL schema entry defined for path segment: "${prefixKey}" required by column "${col.key}"`);
      }
      
      const entryObj = entry as {join?: (ctx: JoinContext) => unknown; select?: (param: string, ctx: JoinContext) => string};
      const isImmediateParent = (len === path.length - 1);
      const hasJoin = typeof entryObj.join === 'function';
      const hasSelect = typeof entryObj.select === 'function';
      
      if (!hasJoin && !(isImmediateParent && hasSelect)) {
        throw new Error(`No join or select resolver defined for path segment: "${prefixKey}" required by column "${col.key}"`);
      }
      
      if (hasJoin) {
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
          console.error('Error executing join function for path:', prefixKey, err);
          throw err;
        }
      }
    }
  }
  
  const colExpressions = new Map<string, string>();
  
  for (const col of columns) {
    const path = col.path;
    
    let customSelectStr: string | undefined = undefined;
    let selectSchema = schemaRecord;
    
    for (let len = 1; len < path.length; len++) {
      const prefixPath = path.slice(0, len);
      const segment = prefixPath[prefixPath.length - 1];
      
      if (selectSchema && typeof selectSchema === 'object') {
        const entry = selectSchema[segment];
        if (entry && typeof entry === 'object') {
          const entryObj = entry as {select?: (param: string, ctx: JoinContext) => string; schema?: unknown};
          
          if (typeof entryObj.select === 'function' && len === path.length - 1) {
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
              console.error('Error executing select function for path:', prefixKey, err);
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
    
    let expr = '';
    if (customSelectStr !== undefined) {
      expr = customSelectStr;
    } else if (path.length > 1) {
      const prefixKey = path.slice(0, -1).join('.');
      const tableAlias = getAlias(prefixKey);
      const leaf = path[path.length - 1];
      expr = `${tableAlias}.${leaf}`;
    } else {
      expr = `base.${path[0]}`;
    }
    colExpressions.set(col.colId, expr);
  }
  
  let selectList = '';
  let groupByClause = '';
  
  const pivotConfig = (pivot && typeof pivot === 'object') 
    ? (pivot as {groupby?: unknown; aggregate?: unknown}) 
    : undefined;
    
  if (pivotConfig && (Array.isArray(pivotConfig.groupby) || Array.isArray(pivotConfig.aggregate))) {
    const groupby = Array.isArray(pivotConfig.groupby) ? pivotConfig.groupby as string[] : [];
    const aggregate = Array.isArray(pivotConfig.aggregate) 
      ? pivotConfig.aggregate as Array<{colId?: unknown; func?: unknown}> 
      : [];
      
    const selectParts: string[] = [];
    const groupbyParts: string[] = [];
    
    for (const groupbyId of groupby) {
      if (typeof groupbyId !== 'string') continue;
      const expr = colExpressions.get(groupbyId);
      if (!expr) {
        throw new Error(`Group by column "${groupbyId}" is not present in columns list (cols)`);
      }
      selectParts.push(`  ${expr} AS \`${groupbyId}\``);
      groupbyParts.push(expr);
    }
    
    for (const agg of aggregate) {
      if (!agg || typeof agg !== 'object') continue;
      const colId = agg.colId;
      const func = agg.func;
      if (typeof colId !== 'string' || typeof func !== 'string') continue;
      
      const expr = colExpressions.get(colId);
      if (!expr) {
        throw new Error(`Aggregate column "${colId}" is not present in columns list (cols)`);
      }
      selectParts.push(`  ${func.toUpperCase()}(${expr}) AS \`${colId}\``);
    }
    
    selectList = selectParts.join(',\n');
    if (groupbyParts.length > 0) {
      groupByClause = `\nGROUP BY ${groupbyParts.join(', ')}`;
    }
  } else {
    selectList = columns
      .map((col) => {
        const expr = colExpressions.get(col.colId)!;
        return `  ${expr} AS \`${col.colId}\``;
      })
      .join(',\n');
  }
  
  const joinBlock = joinClauses.length > 0 
    ? '\n' + joinClauses.map((clause) => `${clause}`).join('\n') 
    : '';
    
  return `SELECT\n${selectList}\nFROM (\n  ${trimmedBaseSql.replace(/\n/g, '\n  ')}\n) AS base${joinBlock}${groupByClause}`;
}
