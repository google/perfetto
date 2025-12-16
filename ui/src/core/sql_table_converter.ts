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

import m from 'mithril';
import {Time} from '../base/time';
import {Timestamp} from '../components/widgets/timestamp';
import {Trace} from '../public/trace';
import {SqlTable} from '../public/sql_modules';
import {
  SQLSchemaRegistry,
  SQLColumnDef,
  SQLJoinDef,
  SQLExpressionDef,
  SQLTableSchema,
} from '../components/widgets/datagrid/sql_schema';
import {
  SchemaRegistry,
  ColumnDef,
  ParameterizedColumnDef,
} from '../components/widgets/datagrid/datagrid_schema';
import {DurationWidget} from '../components/widgets/duration';
import {Anchor} from '../widgets/anchor';
import {Icons} from '../base/semantic_icons';

const SUPPORTED_LINKTO_TABLES = ['slice', 'thread_state', 'sched'];

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
export function sqlTablesToSchemas(
  tables: readonly SqlTable[],
  trace: Trace,
): {
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
      const kind = col.type?.kind;

      if (kind === 'joinid' && col.type) {
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
      } else if (kind === 'arg_set_id') {
        // arg_set_id becomes a parameterized column for args
        // Use 'args' as the parameterized column name
        const argColName = 'args';
        const allArgsColName = 'all_args';

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
          titleString: 'Arg',
        };

        // SQL schema: Expression to get all args as JSON
        sqlColumns[allArgsColName] = <SQLExpressionDef>{
          expression: (alias) => {
            return `(
              SELECT json_group_object(args.key, args.display_value)
              FROM args
              WHERE args.arg_set_id = ${alias}.${colName}
            )`;
          },
        };

        // Display schema: All args column with custom renderer
        displayColumns[allArgsColName] = <ColumnDef>{
          title: 'Args',
          titleString: 'Args',
          columnType: 'text',
          cellRenderer: (value) => {
            if (value === null || value === undefined) {
              return m('span.pf-null-value', 'NULL');
            }
            try {
              const parsed =
                typeof value === 'string' ? JSON.parse(value) : value;
              if (typeof parsed !== 'object' || parsed === null) {
                return String(value);
              }
              const entries = Object.entries(parsed);
              if (entries.length === 0) {
                return m('span.pf-empty-value', '{}');
              }
              return m(
                'span.pf-args-list',
                '{',
                entries.map(([key, val], i) => [
                  i > 0 ? ', ' : '',
                  m('b', key),
                  ': ',
                  String(val),
                ]),
                '}',
              );
            } catch {
              return String(value);
            }
          },
        };

        // Also keep the raw arg_set_id column
        sqlColumns[colName] = <SQLColumnDef>{};
        displayColumns[colName] = <ColumnDef>{
          title: colName,
          titleString: colName,
          filterType: 'numeric',
        };
      } else if (kind === 'int' || kind === 'double') {
        sqlColumns[colName] = <SQLColumnDef>{};
        displayColumns[colName] = {
          title: colName,
          titleString: colName,
          columnType: 'quantitative',
        };
      } else if (kind === 'string') {
        sqlColumns[colName] = <SQLColumnDef>{};
        displayColumns[colName] = {
          title: colName,
          titleString: colName,
          columnType: 'text',
        };
      } else if (kind === 'boolean') {
        sqlColumns[colName] = <SQLColumnDef>{};
        displayColumns[colName] = {
          title: colName,
          titleString: colName,
          columnType: 'identifier', // Not really an identifier, but we need the same filter types.
        };
      } else if (kind === 'id') {
        sqlColumns[colName] = <SQLColumnDef>{};
        displayColumns[colName] = {
          title: colName,
          titleString: colName,
          columnType: 'identifier',
          cellRenderer: (value) => {
            function getTableName() {
              if (col.type?.kind === 'id') {
                return col.type.source.table;
              } else {
                return undefined;
              }
            }
            const tableName = getTableName();

            if (
              tableName &&
              SUPPORTED_LINKTO_TABLES.includes(tableName) &&
              (typeof value === 'number' || typeof value === 'bigint')
            ) {
              // Return an identifier-styled cell - is is an Anchor widget with a
              // link to the slice in the timeline.
              return m(
                Anchor,
                {
                  onclick: () => {
                    trace.selection.selectSqlEvent(tableName, Number(value), {
                      scrollToSelection: true,
                      switchToCurrentSelectionTab: false,
                    });
                  },
                  icon: Icons.UpdateSelection,
                  title: 'Show event in timeline',
                },
                String(value),
              );
            } else {
              return String(value);
            }
          },
        };
      } else if (kind === 'bytes') {
        sqlColumns[colName] = <SQLColumnDef>{};
        displayColumns[colName] = {
          title: colName,
          titleString: colName,
          columnType: 'text',
        };
      } else if (kind === 'duration') {
        sqlColumns[colName] = <SQLColumnDef>{};
        displayColumns[colName] = {
          title: colName,
          titleString: colName,
          columnType: 'quantitative',
          cellRenderer: (value) => {
            if (typeof value === 'number') {
              value = BigInt(Math.round(value));
            }
            if (typeof value !== 'bigint') {
              return String(value);
            }
            return m(DurationWidget, {
              trace,
              dur: value,
            });
          },
        };
      } else if (kind === 'timestamp') {
        sqlColumns[colName] = <SQLColumnDef>{};
        displayColumns[colName] = {
          title: colName,
          titleString: colName,
          columnType: 'quantitative',
          cellRenderer: (value) => {
            if (typeof value === 'number') {
              value = BigInt(Math.round(value));
            }
            if (typeof value !== 'bigint') {
              return String(value);
            }
            return m(Timestamp, {
              trace,
              ts: Time.fromRaw(value),
            });
          },
        };
      } else {
        // For undefined and any other unhandled types
        sqlColumns[colName] = <SQLColumnDef>{};
        displayColumns[colName] = {
          title: colName,
          titleString: colName,
        };
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
