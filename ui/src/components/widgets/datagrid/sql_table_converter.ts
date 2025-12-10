// Copyright (C) 2025 The Android Open Source Project
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

/**
 * Converter utilities for migrating from old SqlTable format to new DataGrid schema format.
 */

import {SqlTable} from '../../../plugins/dev.perfetto.SqlModules/sql_modules';
import {
  SQLSchemaRegistry,
  SQLColumnDef,
  SQLJoinDef,
  SQLExpressionDef,
  SQLTableSchema,
} from './sql_schema';
import {
  SchemaRegistry,
  ColumnDef,
  ParameterizedColumnDef,
} from './datagrid_schema';

/**
 * Converts multiple SqlTables into unified SQL and display schemas.
 * Automatically detects relationships based on id/joinid column types.
 *
 * This function analyzes all tables and creates:
 * 1. SQLSchemaRegistry - For SQLDataSource to build JOINs automatically
 * 2. SchemaRegistry - For DataGrid to display nested column paths
 *
 * Relationships are detected from PerfettoSqlType:
 * - 'joinid' types create schema refs and SQL JOINs
 * - 'id' types mark primary keys
 * - Supports self-references (e.g., slice.parent → slice)
 *
 * Example:
 * ```typescript
 * const tables = sqlModules.listTables();
 * const {sqlSchema, displaySchema} = sqlTablesToSchemas(tables);
 *
 * // Now you can query across relationships:
 * // 'thread.process.name' - automatically generates JOINs
 * ```
 *
 * @param tables Array of SqlTables to convert
 * @returns Complete schemas with all tables and relationships
 */
export function sqlTablesToSchemas(tables: readonly SqlTable[]): {
  sqlSchema: SQLSchemaRegistry;
  displaySchema: SchemaRegistry;
} {
  const sqlSchema: SQLSchemaRegistry = {};
  const displaySchema: SchemaRegistry = {};

  // First pass: Create basic table definitions
  for (const table of tables) {
    const tableName = table.name;
    const sqlColumns: SQLTableSchema['columns'] = {};
    const displayColumns: SchemaRegistry[string] = {};

    for (const col of table.columns) {
      const colName = col.name;

      // Handle special column types
      if (col.type?.kind === 'joinid') {
        // This is a foreign key to another table
        const targetTable = col.type.source.table;
        const foreignKey = colName;

        // Add as SQL JOIN definition
        sqlColumns[colName] = <SQLJoinDef>{
          ref: targetTable,
          foreignKey: foreignKey,
          innerJoin: false,
        };

        // Add as display schema reference
        // Use the column name (e.g., 'parent_id') as the title
        displayColumns[colName] = {
          ref: targetTable,
          title: colName,
          columnType: 'quantitative',
        };
      } else if (col.type?.kind === 'arg_set_id') {
        // arg_set_id becomes a parameterized column for args
        // Use 'args' as the parameterized column name
        const argColName = 'args';

        // SQL schema: Expression to extract arg by key
        sqlColumns[argColName] = <SQLExpressionDef>{
          expression: (alias, key) => {
            if (key) {
              return `extract_arg(${alias}.${colName}, '${key}')`;
            }
            return `${alias}.${colName}`;
          },
          parameterized: true,
          parameterKeysQuery: (baseTable, baseAlias) => `
            SELECT DISTINCT args.key
            FROM ${baseTable} AS ${baseAlias}
            JOIN args ON args.arg_set_id = ${baseAlias}.${colName}
            WHERE args.key IS NOT NULL
            ORDER BY args.key
            LIMIT 1000
          `,
        };

        // Display schema: Parameterized column
        displayColumns[argColName] = <ParameterizedColumnDef>{
          parameterized: true,
          title: (key) => `Arg: ${key}`,
          titleString: 'Args',
        };

        // Also keep the raw arg_set_id column
        sqlColumns[colName] = <SQLColumnDef>{};
        displayColumns[colName] = <ColumnDef>{
          title: colName,
          titleString: colName,
          filterType: 'numeric',
        };
      } else {
        // Regular column
        sqlColumns[colName] = <SQLColumnDef>{};

        displayColumns[colName] = <ColumnDef>{
          title: colName,
          titleString: colName,
          filterType:
            col.type?.kind === 'int' ||
            col.type?.kind === 'double' ||
            col.type?.kind === 'timestamp' ||
            col.type?.kind === 'duration'
              ? 'numeric'
              : col.type?.kind === 'string'
                ? 'string'
                : undefined,
        };

        // TODO: Add cellRenderers for special types (timestamp, duration)
      }
    }

    sqlSchema[tableName] = {
      table: tableName,
      columns: sqlColumns,
    };

    displaySchema[tableName] = displayColumns;
  }

  return {sqlSchema, displaySchema};
}

/**
 * Converts a single SqlTable to schemas (simpler version for single-table use).
 *
 * @param table The SqlTable from SqlModules
 * @returns SQL schema registry with single table entry
 */
export function sqlTableToSQLSchema(table: SqlTable): {
  sqlSchema: SQLSchemaRegistry;
  displaySchema: SchemaRegistry;
  rootSchema: string;
} {
  const {sqlSchema, displaySchema} = sqlTablesToSchemas([table]);

  return {
    sqlSchema,
    displaySchema,
    rootSchema: table.name,
  };
}
