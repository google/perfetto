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

import {Pagination} from '../data_source';
import {Column, Filter} from '../model';
import {SQLSchemaRegistry, SQLSchemaResolver} from '../sql_schema';
import {filterToSql, toAlias} from '../sql_utils';
import {NormalizedQueryModel, SortSpec} from './model';

export function buildQuery(
  sqlSchema: SQLSchemaRegistry,
  rootSchemaName: string,
  req: NormalizedQueryModel,
): string {
  const {columns, filters = [], sort, pagination} = req;

  const resolver = new SQLSchemaResolver(sqlSchema, rootSchemaName);

  const selectExprs = buildSelectExprs(resolver, columns);
  const filterClause = buildFilterClause(resolver, filters);
  const orderByClause = buildOrderByClause(resolver, sort);
  const paginationClause = buildPaginationClause(pagination);

  const baseTable = resolver.getBaseTable();
  const baseAlias = resolver.getBaseAlias();
  const joinClauses = resolver.buildJoinClauses();

  const parts = [
    `SELECT ${selectExprs.join(',\n       ')}`,
    `FROM ${baseTable} AS ${baseAlias}`,
    joinClauses || undefined,
    filterClause,
    orderByClause,
    paginationClause,
  ];

  return parts.filter((p) => p !== undefined).join('\n');
}

export function buildSelectExprs(
  resolver: SQLSchemaResolver,
  columns: readonly Column[],
): string[] {
  const selectExprs: string[] = [];

  for (const col of columns) {
    const sqlExpr = resolver.resolveColumnPath(col.field);
    if (sqlExpr) {
      const alias = toAlias(col.id);
      selectExprs.push(`${sqlExpr} AS ${alias}`);
    }
  }

  if (selectExprs.length === 0) {
    selectExprs.push(`${resolver.getBaseAlias()}.*`);
  }

  return selectExprs;
}

export function buildFilterClause(
  resolver: SQLSchemaResolver,
  filters: readonly Filter[],
): string | undefined {
  if (filters.length > 0) {
    const whereConditions = filters.map((filter) => {
      const sqlExpr = resolver.resolveColumnPath(filter.field);
      return filterToSql(filter, sqlExpr ?? filter.field);
    });
    return `WHERE ${whereConditions.join(' AND ')}`;
  }
  return undefined;
}

export function buildOrderByClause(
  resolver: SQLSchemaResolver,
  sort?: SortSpec,
): string | undefined {
  if (sort) {
    const sqlExpr = resolver.resolveColumnPath(sort.field);
    if (sqlExpr) {
      return `ORDER BY ${sqlExpr} ${sort.direction}`;
    }
  }
  return undefined;
}

export function buildPaginationClause(
  pagination: Pagination | undefined,
): string | undefined {
  if (pagination) {
    return `LIMIT ${pagination.limit} OFFSET ${pagination.offset}`;
  }
  return undefined;
}
