// Copyright (C) 2024 The Android Open Source Project
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

import {assertUnreachable} from '../base/logging';
import {getOrCreate} from '../base/utils';
import {checkExtends, SqlValue, unionTypes} from './query_result';
import {sqlValueToSqliteString} from './sql_utils';

/**
 * A dataset defines a set of rows in TraceProcessor and a schema of the
 * resultant columns. Dataset implementations describe how to get the data in
 * different ways - e.g. 'source' datasets define a dataset as a table name (or
 * select statement) + filters, whereas a 'union' dataset defines a dataset as
 * the union of other datasets.
 *
 * The idea is that users can build arbitrarily complex trees of datasets, then
 * at any point call `optimize()` to create the smallest possible tree that
 * represents the same dataset, and `query()` which produces a select statement
 * for the resultant dataset.
 *
 * Users can also use the `schema` property and `implements()` to get and test
 * the schema of a given dataset.
 */
export interface Dataset<T extends DatasetSchema = DatasetSchema> {
  /**
   * Get or calculate the resultant schema of this dataset.
   */
  readonly schema: T;

  /**
   * Produce a query for this dataset.
   *
   * @param schema - The schema to use for extracting columns - if undefined,
   * the most specific possible schema is evaluated from the dataset first and
   * used instead.
   */
  query(schema?: DatasetSchema): string;

  /**
   * Optimizes a dataset into the smallest possible expression.
   *
   * For example by combining elements of union data sets that have the same src
   * and similar filters into a single set.
   *
   * For example, the following 'union' dataset...
   *
   * ```
   * {
   *   union: [
   *     {
   *       src: 'foo',
   *       schema: {
   *         'a': NUM,
   *         'b': NUM,
   *       },
   *       filter: {col: 'a', eq: 1},
   *     },
   *     {
   *       src: 'foo',
   *       schema: {
   *         'a': NUM,
   *         'b': NUM,
   *       },
   *       filter: {col: 'a', eq: 2},
   *     },
   *   ]
   * }
   * ```
   *
   * ...will be combined into a single 'source' dataset...
   *
   * ```
   * {
   *   src: 'foo',
   *   schema: {
   *     'a': NUM,
   *     'b': NUM,
   *   },
   *   filter: {col: 'a', in: [1, 2]},
   * },
   * ```
   */
  optimize(): Dataset<T>;

  /**
   * Returns true if this dataset implements a given schema.
   *
   * @param schema - The schema to test against.
   */
  implements<T extends DatasetSchema>(schema: T): this is Dataset<T>;
}

/**
 * Defines a list of columns and types that define the shape of the data
 * represented by a dataset.
 */
export type DatasetSchema = Readonly<Record<string, SqlValue>>;

/**
 * A filter used to express that a column must equal a value.
 */
interface EqFilter {
  readonly col: string;
  readonly eq: SqlValue;
}

/**
 * A filter used to express that column must be one of a set of values.
 */
interface InFilter {
  readonly col: string;
  readonly in: ReadonlyArray<SqlValue>;
}

/**
 * Union of all filter types.
 */
type Filter = EqFilter | InFilter;

/**
 * Named arguments for a SourceDataset.
 */
interface SourceDatasetConfig<T extends DatasetSchema> {
  readonly src: string;
  readonly schema: T;
  readonly filter?: Filter;
}

/**
 * Defines a dataset with a source SQL select statement of table name, a
 * schema describing the columns, and an optional filter.
 */
export class SourceDataset<T extends DatasetSchema = DatasetSchema>
  implements Dataset<T>
{
  readonly src: string;
  readonly schema: T;
  readonly filter?: Filter;

  constructor(config: SourceDatasetConfig<T>) {
    this.src = config.src;
    this.schema = config.schema;
    this.filter = config.filter;
  }

  query(schema?: DatasetSchema) {
    schema = schema ?? this.schema;
    const cols = Object.keys(schema);
    const selectSql = `SELECT ${cols.join(', ')} FROM (${this.src})`;
    const filterSql = this.filterQuery();
    if (filterSql === undefined) {
      return selectSql;
    }
    return `${selectSql} WHERE ${filterSql}`;
  }

  optimize() {
    // Cannot optimize SourceDataset
    return this;
  }

  implements<T extends DatasetSchema>(required: T): this is Dataset<T> {
    return Object.entries(required).every(([name, required]) => {
      return name in this.schema && checkExtends(required, this.schema[name]);
    });
  }

  // Convert filter to a SQL expression (without the where clause), or undefined
  // if we have no filter.
  private filterQuery() {
    if (!this.filter) return undefined;

    if ('eq' in this.filter) {
      return `${this.filter.col} = ${sqlValueToSqliteString(this.filter.eq)}`;
    } else if ('in' in this.filter) {
      return `${this.filter.col} IN (${sqlValueToSqliteString(this.filter.in)})`;
    } else {
      assertUnreachable(this.filter);
    }
  }
}

/**
 * Maximum number of sub-queries to include in a single union statement
 * to avoid hitting SQLite limits.
 * See: https://www.sqlite.org/limits.html#max_compound_select
 */
const MAX_SUBQUERIES_PER_UNION = 500;

/**
 * Classes are useless in TypeScript so we need to provide a factory function
 * helper which provides the correct typing for the resultant union dataset
 * based on the input datasets.
 *
 * @param datasets - The datasets to union together.
 * @returns - A new union dataset representing the union of the input datasets.
 */
export function createUnionDataset<T extends readonly Dataset[]>(
  datasets: T,
): UnionDataset<T[number]['schema']> {
  return new UnionDataset(datasets);
}

/**
 * A dataset that represents the union of multiple datasets.
 */
export class UnionDataset<T extends DatasetSchema = DatasetSchema>
  implements Dataset<T>
{
  constructor(readonly union: ReadonlyArray<Dataset>) {}

  get schema(): T {
    // Find the minimal set of columns that are supported by all datasets of
    // the union
    let unionSchema: Record<string, SqlValue> | undefined = undefined;
    this.union.forEach((ds) => {
      const dsSchema = ds.schema;
      if (unionSchema === undefined) {
        // First time just use this one
        unionSchema = dsSchema;
      } else {
        const newSch: Record<string, SqlValue> = {};
        for (const [key, value] of Object.entries(unionSchema)) {
          if (key in dsSchema) {
            const commonType = unionTypes(value, dsSchema[key]);
            if (commonType !== undefined) {
              newSch[key] = commonType;
            }
          }
        }
        unionSchema = newSch;
      }
    });
    const result = unionSchema ?? {};
    return result as T;
  }

  query(schema?: DatasetSchema): string {
    schema = schema ?? this.schema;
    const subQueries = this.union.map((dataset) => dataset.query(schema));

    // If we have a small number of sub-queries, just use a single union all.
    if (subQueries.length <= MAX_SUBQUERIES_PER_UNION) {
      return subQueries.join('\nunion all\n');
    }

    // Handle large number of sub-queries by batching into multiple CTEs.
    let sql = 'with\n';
    const cteNames: string[] = [];

    // Create CTEs for batches of sub-queries
    for (let i = 0; i < subQueries.length; i += MAX_SUBQUERIES_PER_UNION) {
      const batch = subQueries.slice(i, i + MAX_SUBQUERIES_PER_UNION);
      const cteName = `union_batch_${Math.floor(i / MAX_SUBQUERIES_PER_UNION)}`;
      cteNames.push(cteName);

      sql += `${cteName} as (\n${batch.join('\nunion all\n')}\n)`;

      // Add comma unless this is the last CTE.
      if (i + MAX_SUBQUERIES_PER_UNION < subQueries.length) {
        sql += ',\n';
      }
    }

    const cols = Object.keys(schema);

    // Union all the CTEs together in the final query.
    sql += '\n';
    sql += cteNames
      .map((name) => `select ${cols.join(',')} from ${name}`)
      .join('\nunion all\n');

    return sql;
  }

  optimize(): Dataset<T> {
    // Recursively optimize each dataset of this union
    const optimizedUnion = this.union.map((ds) => ds.optimize());

    // Find all source datasets and combine then based on src
    const combinedSrcSets = new Map<string, SourceDataset[]>();
    const otherDatasets: Dataset[] = [];
    for (const e of optimizedUnion) {
      if (e instanceof SourceDataset) {
        const set = getOrCreate(combinedSrcSets, e.src, () => []);
        set.push(e);
      } else {
        otherDatasets.push(e);
      }
    }

    const mergedSrcSets = Array.from(combinedSrcSets.values()).map(
      (srcGroup) => {
        if (srcGroup.length === 1) return srcGroup[0];

        // Combine schema across all members in the union
        const combinedSchema = srcGroup.reduce((acc, e) => {
          Object.assign(acc, e.schema);
          return acc;
        }, {} as DatasetSchema);

        // Merge filters for the same src
        const inFilters: InFilter[] = [];
        for (const {filter} of srcGroup) {
          if (filter) {
            if ('eq' in filter) {
              inFilters.push({col: filter.col, in: [filter.eq]});
            } else {
              inFilters.push(filter);
            }
          }
        }

        const mergedFilter = mergeFilters(inFilters);
        return new SourceDataset({
          src: srcGroup[0].src,
          schema: combinedSchema,
          filter: mergedFilter,
        });
      },
    );

    const finalUnion = [...mergedSrcSets, ...otherDatasets];

    if (finalUnion.length === 1) {
      return finalUnion[0] as Dataset<T>;
    } else {
      return new UnionDataset(finalUnion);
    }
  }

  implements<T extends DatasetSchema>(required: T): this is Dataset<T> {
    return Object.entries(required).every(([name, required]) => {
      return name in this.schema && checkExtends(required, this.schema[name]);
    });
  }
}

function mergeFilters(filters: InFilter[]): InFilter | undefined {
  if (filters.length === 0) return undefined;
  const col = filters[0].col;
  const values = new Set(filters.flatMap((filter) => filter.in));
  return {col, in: Array.from(values)};
}
