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

import {assertFalse, assertTrue, assertUnreachable} from '../base/logging';
import {getOrCreate} from '../base/utils';
import {ColumnType, SqlValue, checkExtends, unionTypes} from './query_result';
import {sqlValueToSqliteString} from './sql_utils'; // Import the helper

export type DatasetSchema = Record<string, ColumnType>;

export interface InPartition {
  readonly col: string;
  readonly in: ReadonlyArray<SqlValue>;
}

export interface EqPartition {
  readonly col: string;
  readonly eq: SqlValue;
}

export type Partition = InPartition | EqPartition;

// Convert filter to a SQL expression (without the where clause), or undefined
// if we have no filter.
function partitionToWhereClause(partition: Partition): string {
  if ('eq' in partition) {
    return `${partition.col} = ${sqlValueToSqliteString(partition.eq)}`;
  } else if ('in' in partition) {
    if (partition.in.length === 0) {
      return '0';
    } else {
      return `${partition.col} IN (${sqlValueToSqliteString(partition.in)})`;
    }
  } else {
    assertUnreachable(partition);
  }
}

// Base interface defining the common methods for all dataset types
export interface Dataset<Schema extends DatasetSchema = DatasetSchema> {
  readonly schema: Schema;
  // Generates the SQL query string for this dataset view.
  // Optionally accepts a sub-schema to select only specific columns.
  query(querySchema?: Partial<Schema>): string;
  // Checks if this dataset's schema includes the required schema.
  implements(required: DatasetSchema): boolean;
}

// Represents a dataset directly from a source (table or subquery)
export class SourceDataset<SchemaType extends DatasetSchema = DatasetSchema>
  implements Dataset<SchemaType>
{
  readonly src: string;
  readonly schema: SchemaType;

  constructor(args: {src: string; schema: SchemaType}) {
    this.src = args.src;
    this.schema = args.schema;
  }

  query(querySchema?: Partial<SchemaType>): string {
    const schema = querySchema ?? this.schema;
    const selectCols = Object.keys(schema);
    assertFalse(selectCols.length === 0, 'Schema cannot be empty');
    return `SELECT ${selectCols.join(', ')} FROM (${this.src})`;
  }

  implements(required: DatasetSchema) {
    return Object.entries(required).every(([name, required]) => {
      return name in this.schema && checkExtends(required, this.schema[name]);
    });
  }
}

// Represents a partitioned view of a base dataset. The idea is that
// BaseSchemaType is more specific that SchemaType, so if BaseSchemaType is the
// most wide open (DatasetSchema) then it cannot really extend it our more
// narrow type.

// So what do we want to specify when we define a PartitionedDataaset in our
// dataset slice tracks? We really just want to inject in a dataset that defines
// the correct output type for the sake of typing the rows of the track, we
// don't care about the type of the base dataset, we trust that the constructor
// confirmed this relationship.

// So if we define an interface PartitionedDataset where the base dataset is
// just the wide open dataset, and define that instead.
export class PartitionedDataset<
  SchemaType extends DatasetSchema = DatasetSchema,
  BaseSchemaType extends SchemaType = SchemaType,
> implements Dataset<SchemaType>
{
  readonly base: SourceDataset<BaseSchemaType>;
  readonly partition: Partition;
  readonly schema: SchemaType; // This schema can be a subset (projection)

  constructor(args: {
    base: SourceDataset<BaseSchemaType>;
    partition: Partition;
    schema: SchemaType;
  }) {
    this.base = args.base;
    this.partition = args.partition;
    this.schema = args.schema;
  }

  query(overrideSchema?: Partial<SchemaType>): string {
    const schema = overrideSchema ?? this.schema;
    const selectCols = Object.keys(schema);
    assertFalse(selectCols.length === 0, 'Schema cannot be empty');
    return `
      SELECT ${selectCols.join(', ')}
      FROM (${this.base.query()})
      WHERE ${partitionToWhereClause(this.partition)}
    `;
  }

  implements(required: DatasetSchema) {
    return Object.entries(required).every(([name, required]) => {
      return name in this.schema && checkExtends(required, this.schema[name]);
    });
  }
}

const MAX_UNION_ALL_STATEMENTS = 500; // To avoid hitting SQL limits

export class UnionDataset implements Dataset<DatasetSchema> {
  private readonly datasets: ReadonlyArray<Dataset>;

  constructor(datasets: ReadonlyArray<Dataset>) {
    assertTrue(
      datasets.length > 0,
      'UnionDataset requires at least one dataset',
    );
    this.datasets = datasets;
  }

  get schema(): DatasetSchema {
    // Find the minimal set of columns that are supported by all datasets of
    // the union
    let unionSchema: Record<string, ColumnType> | undefined = undefined;
    this.datasets.forEach((ds) => {
      const dsSchema = ds.schema;
      if (unionSchema === undefined) {
        // First time just use this one
        unionSchema = dsSchema;
      } else {
        const newSch: Record<string, ColumnType> = {};
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
    return unionSchema ?? {};
  }

  implements(required: DatasetSchema): boolean {
    // Check against the computed common schema
    const thisSchema = this.schema;
    return Object.entries(required).every(([name, requiredType]) => {
      const actualType = thisSchema[name];
      return name in thisSchema && checkExtends(requiredType, actualType);
    });
  }

  // Generates an optimized SQL query to fetch this union.
  query(overrideSchema?: DatasetSchema): string {
    const effectiveSchema = overrideSchema ?? this.schema;
    const selectCols = Object.keys(effectiveSchema);
    assertFalse(selectCols.length === 0, 'Schema cannot be empty');

    // Group partitioned datasets by their base dataset
    const partitionGroups = new Map<SourceDataset, Partition[]>();
    const otherDatasets: Dataset[] = [];

    for (const ds of this.datasets) {
      if (ds instanceof PartitionedDataset) {
        getOrCreate(partitionGroups, ds.base, () => []).push(ds.partition);
      } else {
        otherDatasets.push(ds);
      }
    }

    const selectStatements: string[] = [];
    for (const [base, partitions] of partitionGroups) {
      const whereClause = combinePartitions(partitions)
        .map(partitionToWhereClause)
        .join(' OR ');
      selectStatements.push(`
        SELECT ${selectCols.join(', ')}
        FROM (${base.query()})
        WHERE ${whereClause}
      `);
    }
    for (const dataset of otherDatasets) {
      selectStatements.push(dataset.query());
    }

    // Batch UNION ALL statements using CTEs if there are too many
    if (selectStatements.length > MAX_UNION_ALL_STATEMENTS) {
      const batchSize = MAX_UNION_ALL_STATEMENTS;
      const ctes: string[] = [];
      for (let i = 0; i < selectStatements.length; i += batchSize) {
        const batch = selectStatements.slice(i, i + batchSize);
        ctes.push(
          `union_batch_${i / batchSize} AS (\n  ${batch.join(
            '\n  UNION ALL\n  ',
          )}\n)`,
        );
      }
      const finalSelects = ctes
        .map((_, i) => `SELECT ${selectCols.join(', ')} FROM union_batch_${i}`)
        .join('\nUNION ALL\n');
      return `WITH ${ctes.join(',\n')}\n${finalSelects}`;
    } else {
      // Simple UNION ALL for fewer datasets
      return selectStatements.join('\nUNION ALL\n');
    }
  }
}

function combinePartitions(partitions: Partition[]): Partition[] {
  // Combine partitions into a single where clause.
  // Create a map to store partitions by column name.
  const valuesByCol = new Map<string, Set<SqlValue>>();
  for (const partition of partitions) {
    const group = getOrCreate(valuesByCol, partition.col, () => new Set());
    if ('in' in partition) {
      partition.in.forEach((x) => group.add(x));
    } else {
      group.add(partition.eq);
    }
  }

  return Array.from(valuesByCol.entries()).map(([col, values]) => {
    const arrayOfValues = Array.from(values);
    if (arrayOfValues.length === 1) {
      return {col, eq: arrayOfValues[0]};
    } else {
      return {col, in: arrayOfValues};
    }
  });
}
