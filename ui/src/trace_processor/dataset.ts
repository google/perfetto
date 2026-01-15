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
 * call `query()` which automatically optimizes the dataset and produces an
 * optimal select statement. The `optimize()` method can also be called manually
 * if you need a reference to the optimized dataset.
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
   * Produce an optimized query for this dataset.
   *
   * This automatically optimizes the dataset before generating the query to
   * ensure the most efficient SQL is produced.
   *
   * @param schema - The schema to use for extracting columns - if undefined,
   * the most specific possible schema is evaluated from the dataset first and
   * used instead.
   */
  query(schema?: DatasetSchema): string;

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
 * Defines a join to be applied to the dataset.
 */
interface Join {
  /**
   * The SQL join expression, including the table name and join condition.
   * Example: 'thread USING (utid)' or 'process ON slice.upid = process.id'
   */
  readonly from: string;
  /**
   * Optional hint indicating this is a unique (1:1) join. This can be used
   * for query optimization.
   */
  readonly unique?: boolean;
}

/**
 * Defines a column selection with an optional join reference.
 */
interface SelectColumn {
  /**
   * The SQL expression or column name to select.
   */
  readonly expr: string;
  /**
   * Optional join identifier that this column references.
   * This helps track which join a column comes from for optimization.
   */
  readonly join?: string;
}

/**
 * Column selection can be either a simple string (expression/column name)
 * or an object with expr and optional join reference.
 */
type SelectValue = string | SelectColumn;

/**
 * Named arguments for a SourceDataset.
 */
interface SourceDatasetConfig<T extends DatasetSchema> {
  readonly src: string;
  readonly schema: T;
  readonly filter?: Filter;
  /**
   * Optional column mappings from schema column names to source expressions.
   * Each value can be:
   * - A string: simple column name or expression
   * - An object: {expr: 'column', join: 'join-id'} to reference a specific join
   * Example: {id: 'id', name: {expr: 'name', join: 'thread'}}
   */
  readonly select?: Readonly<Record<keyof T, SelectValue>>;
  /**
   * Optional joins to apply to the dataset. Each join is identified by a
   * unique key and contains the join expression and optional optimization hints.
   * Example: {thread: {from: 'thread USING (utid)', unique: true}}
   */
  readonly joins?: Readonly<Record<string, Join>>;
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
  readonly select?: Readonly<Record<keyof T, SelectValue>>;
  readonly joins?: Readonly<Record<string, Join>>;

  constructor(config: SourceDatasetConfig<T>) {
    this.src = config.src;
    this.schema = config.schema;
    this.filter = config.filter;
    this.select = config.select;
    this.joins = config.joins;
  }

  query(schema?: DatasetSchema) {
    schema = schema ?? this.schema;
    const cols = Object.keys(schema);

    // Track which joins are referenced in select statements
    const referencedJoins = new Set<string>();

    // Build the SELECT clause with column mappings if provided
    const selectCols = cols.map((col) => {
      const selectValue = this.select?.[col as keyof T];
      if (selectValue === undefined) {
        return col;
      }

      // Extract the expression and join reference
      let expr: string;
      if (typeof selectValue === 'string') {
        expr = selectValue;
      } else {
        expr = selectValue.expr;
        // Track the join reference if specified
        if (selectValue.join) {
          referencedJoins.add(selectValue.join);
        }
      }

      // Only add AS clause if the expression differs from the column name
      if (expr !== col) {
        return `${expr} AS ${col}`;
      }
      return col;
    });

    // Build the FROM clause with joins if provided
    // Only include joins that are either:
    // 1. Referenced in a select statement, OR
    // 2. Not marked as unique (i.e., unique is false or undefined)
    let fromClause = `(${this.src})`;
    if (this.joins) {
      for (const [joinId, join] of Object.entries(this.joins)) {
        // Skip unique joins that aren't referenced
        if (join.unique && !referencedJoins.has(joinId)) {
          continue;
        }

        // Insert the alias (joinId) after the table name
        // e.g., "thread USING (utid)" becomes "thread AS thread USING (utid)"
        const spaceIndex = join.from.indexOf(' ');
        let joinClause: string;
        if (spaceIndex === -1) {
          // No space found, just the table name
          joinClause = `${join.from} AS ${joinId}`;
        } else {
          // Insert alias after first word (table name)
          const tableName = join.from.substring(0, spaceIndex);
          const rest = join.from.substring(spaceIndex);
          joinClause = `${tableName} AS ${joinId}${rest}`;
        }
        fromClause += ` JOIN ${joinClause}`;
      }
    }

    const selectSql = `SELECT ${selectCols.join(', ')} FROM ${fromClause}`;
    const filterSql = this.filterQuery();
    if (filterSql === undefined) {
      return selectSql;
    }
    return `${selectSql} WHERE ${filterSql}`;
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
 * A dataset that represents the union of multiple datasets.
 */
export class UnionDataset<T extends DatasetSchema = DatasetSchema>
  implements Dataset<T>
{
  /**
   * This factory method creates a new union dataset but retains the specific
   * types of the input datasets. It's a factory function because it's not
   * possible to do this with a constructor.
   *
   * @param datasets - The datasets to union together.
   * @returns - A new union dataset representing the union of the input
   * datasets.
   */
  static create<T extends readonly Dataset[]>(
    datasets: T,
  ): UnionDataset<T[number]['schema']> {
    return new UnionDataset(datasets);
  }

  private constructor(readonly union: ReadonlyArray<Dataset>) {}

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
    const querySchema = schema ?? this.schema;

    // Flatten the entire union tree and extract all datasets
    const allDatasets = this.flattenUnion();

    // Group SourceDatasets by src and merge them
    const combinedSrcSets = new Map<string, SourceDataset[]>();
    const otherDatasets: Dataset[] = [];

    for (const dataset of allDatasets) {
      if (dataset instanceof SourceDataset) {
        const set = getOrCreate(combinedSrcSets, dataset.src, () => []);
        set.push(dataset);
      } else {
        // Non-source datasets (shouldn't happen after flattening, but handle it)
        otherDatasets.push(dataset);
      }
    }

    // Merge SourceDatasets with the same src
    const mergedDatasets: Dataset[] = [];

    for (const srcGroup of combinedSrcSets.values()) {
      if (srcGroup.length === 1) {
        mergedDatasets.push(srcGroup[0]);
      } else {
        // Combine schema across all members in the union
        const combinedSchema = srcGroup.reduce((acc, e) => {
          Object.assign(acc, e.schema);
          return acc;
        }, {} as DatasetSchema);

        // Merge select mappings - take the first one we find for each column
        const combinedSelect: Record<string, SelectValue> = {};
        for (const dataset of srcGroup) {
          if (dataset.select) {
            for (const [col, selectValue] of Object.entries(dataset.select)) {
              if (!(col in combinedSelect)) {
                combinedSelect[col] = selectValue;
              }
            }
          }
        }

        // Merge joins - collect all unique joins
        const combinedJoins: Record<string, Join> = {};
        for (const dataset of srcGroup) {
          if (dataset.joins) {
            Object.assign(combinedJoins, dataset.joins);
          }
        }

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
        mergedDatasets.push(
          new SourceDataset({
            src: srcGroup[0].src,
            schema: combinedSchema,
            filter: mergedFilter,
            select:
              Object.keys(combinedSelect).length > 0
                ? combinedSelect
                : undefined,
            joins:
              Object.keys(combinedJoins).length > 0 ? combinedJoins : undefined,
          }),
        );
      }
    }

    mergedDatasets.push(...otherDatasets);

    // If we merged everything into a single dataset, use its query directly
    // Pass the querySchema to enable column and join elimination
    if (mergedDatasets.length === 1) {
      return mergedDatasets[0].query(querySchema);
    }

    // Generate union query from merged datasets
    // Pass querySchema to each dataset to enable column and join elimination
    const subQueries = mergedDatasets.map((dataset) =>
      dataset.query(querySchema),
    );

    // If we have a small number of sub-queries, just use a single union all.
    if (subQueries.length <= MAX_SUBQUERIES_PER_UNION) {
      return subQueries.join('\nUNION ALL\n');
    }

    // Handle large number of sub-queries by batching into multiple CTEs.
    let sql = 'with\n';
    const cteNames: string[] = [];

    // Create CTEs for batches of sub-queries
    for (let i = 0; i < subQueries.length; i += MAX_SUBQUERIES_PER_UNION) {
      const batch = subQueries.slice(i, i + MAX_SUBQUERIES_PER_UNION);
      const cteName = `union_batch_${Math.floor(i / MAX_SUBQUERIES_PER_UNION)}`;
      cteNames.push(cteName);

      sql += `${cteName} as (\n${batch.join('\nUNION ALL\n')}\n)`;

      // Add comma unless this is the last CTE.
      if (i + MAX_SUBQUERIES_PER_UNION < subQueries.length) {
        sql += ',\n';
      }
    }

    const cols = Object.keys(querySchema);

    // Union all the CTEs together in the final query.
    sql += '\n';
    sql += cteNames
      .map((name) => `SELECT ${cols.join(', ')} FROM ${name}`)
      .join('\nUNION ALL\n');

    return sql;
  }

  /**
   * Recursively flatten this union tree, extracting all leaf datasets.
   * Nested UnionDatasets are recursively flattened.
   */
  private flattenUnion(): Dataset[] {
    const result: Dataset[] = [];

    for (const dataset of this.union) {
      if (dataset instanceof UnionDataset) {
        // Recursively flatten nested unions
        result.push(...dataset.flattenUnion());
      } else {
        // Leaf dataset (SourceDataset or other)
        result.push(dataset);
      }
    }

    return result;
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
