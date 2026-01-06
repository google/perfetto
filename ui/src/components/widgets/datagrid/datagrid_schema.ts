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

import m from 'mithril';
import {maybeUndefined} from '../../../base/utils';
import {Row, SqlValue} from '../../../trace_processor/query_result';

/**
 * Rich return type for cell renderers that need to control alignment/styling.
 * Renderers can return this instead of plain m.Children to override defaults.
 */
export interface CellRenderResult {
  readonly content: m.Children;
  readonly align?: 'left' | 'right' | 'center';
  readonly nullish?: boolean;
}

/**
 * Type guard to check if a cell renderer result is a CellRenderResult object.
 */
export function isCellRenderResult(
  result: m.Children | CellRenderResult,
): result is CellRenderResult {
  return (
    result !== null &&
    typeof result === 'object' &&
    !Array.isArray(result) &&
    'content' in result
  );
}

export type CellRenderer = (
  value: SqlValue,
  row: Row,
) => m.Children | CellRenderResult;
export type CellFormatter = (value: SqlValue, row: Row) => string;

/**
 * A registry of named schemas that can reference each other.
 * This allows defining complex relational schemas with self-references
 * (e.g., parent.parent.parent.name) and cross-references between tables.
 */
export interface SchemaRegistry {
  [schemaName: string]: ColumnSchema;
}

/**
 * A schema defining the available columns in a data source.
 * Each key is a column name, and the value describes how to render it
 * or references another schema for nested data.
 */
export type ColumnSchema = {
  [columnName: string]: ColumnDef | SchemaRef | ParameterizedColumnDef;
};

// Allowed data types for columns, used for filtering and rendering.
// - 'text': string data (contains, glob) with distinct value picker
// - 'quantitative': numeric comparisons (=, !=, <, <=, >, >=), no distinct values, no text filters
// - 'identifier': numeric comparisons (=, !=, <, <=, >, >=) with distinct value picker, no text filters
export type ColumnType = 'text' | 'quantitative' | 'identifier';

// Cell alignment options. If not specified, alignment is inferred from the
// cell value type (numbers right-aligned, text left-aligned).
export type CellAlignment = 'left' | 'right' | 'center';

/**
 * A leaf column definition with rendering configuration.
 * This replaces the old ColumnDefinition type.
 */
export interface ColumnDef {
  // Human readable title to display instead of the column name.
  readonly title?: m.Children;

  // Plain string title for exports. Used when title is m.Children.
  // Falls back to column path if not provided.
  readonly titleString?: string;

  // Control which types of filters are available for this column.
  // - 'numeric': Shows comparison filters (=, !=, <, <=, >, >=) and null filters
  // - 'string': Shows text filters (contains, glob) and equals/null filters
  readonly columnType?: ColumnType;

  // Custom renderer for this column's cells
  readonly cellRenderer?: CellRenderer;

  // Optional value formatter for this column, used when exporting data.
  readonly cellFormatter?: CellFormatter;

  // Additional fields this column depends on for rendering.
  // These fields will be included in queries and made available in the row
  // parameter passed to cellRenderer, even if they're not visible columns.
  // Use this for lineage tracking or other metadata fields.
  // Example: id column depending on __groupid and __partition for clickable links
  readonly dependsOn?: readonly string[];
}

/**
 * A reference to another named schema in the registry.
 * Used for nested relationships like parent -> slice or thread -> process.
 */
export interface SchemaRef {
  // Name of the schema in the registry to reference
  readonly ref: string;

  // Override the title for this reference (e.g., "Parent Slice" instead of "Slice")
  readonly title?: string;

  // Override filter type for all columns accessed through this reference
  readonly filterType?: 'quantitative' | 'text';
}

/**
 * A parameterized column where the key is determined at runtime.
 * Used for things like args.foo, args.bar where keys are data-dependent.
 */
export interface ParameterizedColumnDef {
  readonly parameterized: true;

  // Title can be a static string or a function that takes the key
  readonly title?: m.Children | ((key: string) => m.Children);

  // Plain string title for exports. Falls back to column path if not provided.
  readonly titleString?: string;

  readonly filterType?: 'quantitative' | 'text';
  readonly cellRenderer?: CellRenderer;
  readonly cellFormatter?: CellFormatter;
  readonly distinctValues?: boolean;

  // Additional fields this column depends on for rendering.
  readonly dependsOn?: readonly string[];
}

/**
 * Type guard to check if a schema entry is a leaf ColumnDef.
 */
export function isColumnDef(
  entry: ColumnDef | SchemaRef | ParameterizedColumnDef,
): entry is ColumnDef {
  return !('ref' in entry) && !('parameterized' in entry);
}

/**
 * Type guard to check if a schema entry is a SchemaRef.
 */
export function isSchemaRef(
  entry: ColumnDef | SchemaRef | ParameterizedColumnDef,
): entry is SchemaRef {
  return 'ref' in entry;
}

/**
 * Type guard to check if a schema entry is a ParameterizedColumnDef.
 */
export function isParameterizedColumnDef(
  entry: ColumnDef | SchemaRef | ParameterizedColumnDef,
): entry is ParameterizedColumnDef {
  return 'parameterized' in entry;
}

/**
 * Gets the default visible columns for a schema.
 * Returns all leaf columns (ColumnDef) at the root level.
 * Does not include schema references or parameterized columns by default.
 *
 * @param registry The schema registry
 * @param rootSchema The root schema name
 * @returns Array of column names that are leaf columns
 */
export function getDefaultVisibleColumns(
  registry: SchemaRegistry,
  rootSchema: string,
): string[] {
  const schema = maybeUndefined(registry[rootSchema]);
  if (!schema) return [];

  const columns: string[] = [];
  for (const [columnName, entry] of Object.entries(schema)) {
    if (isColumnDef(entry)) {
      columns.push(columnName);
    }
  }
  return columns;
}

/**
 * Complete information about a resolved column, including all properties
 * needed for rendering. This consolidates multiple schema lookups into one.
 */
export interface ColumnInfo {
  // The resolved column definition
  readonly def: ColumnDef | ParameterizedColumnDef;

  // For parameterized columns, the key that was extracted from the path
  readonly paramKey?: string;

  // Title parts for building the column header (e.g., ["Parent", "Name"])
  readonly titleParts: m.Children[];

  // Convenience properties extracted from def:
  readonly columnType?: ColumnType;
  readonly cellRenderer?: CellRenderer;
  readonly cellFormatter?: CellFormatter;
  readonly dependsOn?: readonly string[];
}

/**
 * Resolves a column path and returns all information needed for rendering.
 * This consolidates multiple schema lookups into a single traversal.
 *
 * @param registry The schema registry
 * @param rootSchema The root schema name
 * @param path The column path (e.g., "parent.parent.name")
 * @returns Complete column info, or undefined if path is invalid
 */
export function getColumnInfo(
  registry: SchemaRegistry,
  rootSchema: string,
  path: string,
): ColumnInfo | undefined {
  const parts = path.split('.');
  const initialSchema = maybeUndefined(registry[rootSchema]);

  if (!initialSchema) {
    return undefined;
  }

  const titleParts: m.Children[] = [];
  let currentSchema = initialSchema;

  for (let i = 0; i < parts.length; i++) {
    const part = parts[i];
    const entry = maybeUndefined(currentSchema[part]);

    if (!entry) {
      return undefined;
    }

    if (isSchemaRef(entry)) {
      // Add title for this ref
      titleParts.push(entry.title ?? part);
      // Follow the reference
      const referencedSchema = maybeUndefined(registry[entry.ref]);
      if (!referencedSchema) {
        return undefined;
      }
      currentSchema = referencedSchema;
    } else if (isParameterizedColumnDef(entry)) {
      // For parameterized columns, remaining parts are the key
      const remainingParts = parts.slice(i + 1);
      const paramKey =
        remainingParts.length > 0 ? remainingParts.join('.') : undefined;

      // Build title for parameterized column as "Name[key]" format
      if (typeof entry.title === 'function' && paramKey) {
        titleParts.push(entry.title(paramKey));
      } else {
        const baseName = typeof entry.title === 'string' ? entry.title : part;
        if (paramKey) {
          // Format as "Name[key]" - single element with index-style notation
          titleParts.push(
            m('span', [baseName, m('span.pf-param-key', `[${paramKey}]`)]),
          );
        } else {
          titleParts.push(baseName);
        }
      }

      return {
        def: entry,
        paramKey,
        titleParts,
        columnType: entry.filterType,
        cellRenderer: entry.cellRenderer,
        cellFormatter: entry.cellFormatter,
        dependsOn: entry.dependsOn,
      };
    } else if (isColumnDef(entry)) {
      // Leaf column - should be the last part of the path
      if (i === parts.length - 1) {
        titleParts.push(entry.title ?? part);
        return {
          def: entry,
          titleParts,
          columnType: entry.columnType,
          cellRenderer: entry.cellRenderer,
          cellFormatter: entry.cellFormatter,
          dependsOn: entry.dependsOn,
        };
      }
      // Trying to navigate deeper into a leaf column - invalid
      return undefined;
    }
  }

  // We ended on a schema ref without resolving to a leaf
  return undefined;
}
