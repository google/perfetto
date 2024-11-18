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
import {ColumnType, SqlValue} from './query_result';

export namespace Ds {
  export type Dataset = UnionDataset | SourceDataset;
  export type Schema = Record<string, ColumnType>;

  /**
   * Defines a dataset with a source SQL select statement of table name, a
   * schema describing the columns, and an optional filter.
   */
  export interface SourceDataset {
    readonly src: string;
    readonly schema: Schema;
    readonly filter?: EqFilter | InFilter;
  }

  /**
   * A dataset that represents the union of multiple datasets.
   */
  export interface UnionDataset {
    readonly union: ReadonlyArray<Dataset>;
  }

  /**
   * Generic filter type.
   */
  export type Filter = EqFilter | InFilter;

  /**
   * A filter used to express that a column must equal a value.
   */
  export interface EqFilter {
    readonly col: string;
    readonly eq: SqlValue;
  }

  /**
   * A filter used to express that column must be one of a set of values.
   */
  export interface InFilter {
    readonly col: string;
    readonly in: ReadonlyArray<SqlValue>;
  }

  /**
   * Returns true if the dataset implements a given schema.
   *
   * Note: `implements` is a reserved keyword in TS so we can't call this
   * function `implements`.
   *
   * @param dataset - The dataset to test.
   * @param testSchema - The schema to test against.
   */
  export function doesImplement(dataset: Dataset, testSchema: Schema): boolean {
    const datasetSchema = schema(dataset);
    return Object.entries(testSchema).every(([name, kind]) => {
      return name in datasetSchema && datasetSchema[name] === kind;
    });
  }

  /**
   * This function optimizes a dataset into the smallest possible expression.
   *
   * For example by combining elements of union data sets that have the same src
   * and similar filters into a single set.
   *
   * For example, the following union data set...
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
   * ...will be combined into a single set...
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
   *
   * @param dataset - The dataset to optimize.
   */
  export function optimize(dataset: Dataset): Dataset {
    if ('src' in dataset) {
      // No optimization possible for individual datasets
      return dataset;
    } else if ('union' in dataset) {
      // Recursively optimize each dataset of this union
      const optimizedUnion = dataset.union.map(optimize);

      // Find all source datasets and combine then based on src
      const combinedSrcSets = new Map<string, SourceDataset[]>();
      const otherDatasets: Dataset[] = [];
      for (const e of optimizedUnion) {
        if ('src' in e) {
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
          }, {} as Schema);

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
          return {
            src: srcGroup[0].src,
            schema: combinedSchema,
            filter: mergedFilter,
          };
        },
      );

      const finalUnion = [...mergedSrcSets, ...otherDatasets];

      if (finalUnion.length === 1) {
        return finalUnion[0];
      } else {
        return {union: finalUnion};
      }
    } else {
      assertUnreachable(dataset);
    }
  }

  function mergeFilters(filters: InFilter[]): InFilter | undefined {
    if (filters.length === 0) return undefined;
    const col = filters[0].col;
    const values = new Set(filters.flatMap((filter) => filter.in));
    return {col, in: Array.from(values)};
  }

  /**
   * Get the schema of an dataset.
   *
   * @param dataset - The dataset to get the schema of.
   */
  export function schema(dataset: Dataset): Schema {
    if ('src' in dataset) {
      return dataset.schema;
    } else if ('union' in dataset) {
      // Find the minimal set of columns that are supported by all datasets of
      // the union
      let sch: Record<string, ColumnType> | undefined = undefined;
      dataset.union.forEach((e) => {
        const eSchema = schema(e);
        if (sch === undefined) {
          // First time just use this one
          sch = eSchema;
        } else {
          const newSch: Record<string, ColumnType> = {};
          for (const [key, kind] of Object.entries(sch)) {
            if (key in eSchema && eSchema[key] === kind) {
              newSch[key] = kind;
            }
          }
          sch = newSch;
        }
      });
      return sch ?? {};
    } else {
      assertUnreachable(dataset);
    }
  }

  /**
   * Produce a query for this dataset.
   *
   * @param dataset - The dataset to get the query for.
   * @param sch - The schema to use for extracting columns - if undefined, the
   * most specific possible schema is evaluated from the dataset first and used
   * instead.
   */
  export function query(dataset: Dataset, sch?: Schema): string {
    function filterToQuery(filter: Filter) {
      if ('eq' in filter) {
        return `where ${filter.col} = ${filter.eq}`;
      } else if ('in' in filter) {
        return `where ${filter.col} in (${filter.in.join(',')})`;
      } else {
        assertUnreachable(filter);
      }
    }

    sch = sch ?? schema(dataset);
    if ('src' in dataset) {
      const whereClause = dataset.filter ? filterToQuery(dataset.filter) : '';
      const cols = Object.keys(sch);
      return `select ${cols.join(', ')} from (${dataset.src}) ${whereClause}`.trim();
    } else if ('union' in dataset) {
      return dataset.union
        .map((dataset) => query(dataset, sch))
        .join(' union all ');
    } else {
      assertUnreachable(dataset);
    }
  }
}
