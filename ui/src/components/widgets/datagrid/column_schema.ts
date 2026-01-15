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

export type CellRenderer = (value: SqlValue, row: Row) => m.Children;
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

/**
 * Builtins provided to contextMenuRenderer for column header menus.
 */
export interface ContextMenuBuiltins {
  readonly sorting?: m.Children;
  readonly filters?: m.Children;
  readonly fitToContent?: m.Children;
  readonly columnManagement?: m.Children;
}

/**
 * Builtins provided to cellContextMenuRenderer for cell menus.
 */
export interface CellContextMenuBuiltins {
  readonly addFilter?: m.Children;
}

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
  readonly filterType?: 'numeric' | 'string';

  // Custom renderer for this column's cells
  readonly cellRenderer?: CellRenderer;

  // Optional value formatter for this column, used when exporting data.
  readonly cellFormatter?: CellFormatter;

  // Enable distinct values filtering for this column.
  readonly distinctValues?: boolean;

  // Optional function for custom column header context menu.
  readonly contextMenuRenderer?: (builtins: ContextMenuBuiltins) => m.Children;

  // Optional function for custom cell context menu.
  readonly cellContextMenuRenderer?: (
    value: SqlValue,
    row: Row,
    builtins: CellContextMenuBuiltins,
  ) => m.Children;
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
  readonly filterType?: 'numeric' | 'string';
}

/**
 * A parameterized column where the key is determined at runtime.
 * Used for things like args.foo, args.bar where keys are data-dependent.
 */
export interface ParameterizedColumnDef {
  readonly parameterized: true;

  // Title can be a static string or a function that takes the key
  readonly title?: string | ((key: string) => string);

  // Plain string title for exports. Falls back to column path if not provided.
  readonly titleString?: string;

  readonly filterType?: 'numeric' | 'string';
  readonly cellRenderer?: CellRenderer;
  readonly cellFormatter?: CellFormatter;
  readonly distinctValues?: boolean;
  readonly contextMenuRenderer?: (builtins: ContextMenuBuiltins) => m.Children;
  readonly cellContextMenuRenderer?: (
    value: SqlValue,
    row: Row,
    builtins: CellContextMenuBuiltins,
  ) => m.Children;
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
 * Result of resolving a column path against a schema.
 */
export interface ResolvedColumn {
  // The resolved column definition (leaf or parameterized)
  readonly def: ColumnDef | ParameterizedColumnDef;

  // For parameterized columns, the key that was extracted from the path
  readonly paramKey?: string;
}

/**
 * Resolves a dot-separated column path to its definition in the schema.
 *
 * Examples:
 * - "id" -> resolves to slice.id
 * - "parent.name" -> follows ref to slice, then resolves name
 * - "parent.parent.dur" -> follows ref twice, then resolves dur
 * - "thread.process.pid" -> follows thread ref, then process ref, then pid
 * - "args.foo" -> resolves to parameterized column with key "foo"
 *
 * @param registry The schema registry containing all named schemas
 * @param rootSchema The name of the root schema to start from
 * @param path The dot-separated path to resolve (e.g., "parent.parent.name")
 * @returns The resolved column definition, or undefined if path is invalid
 */
export function resolveColumnPath(
  registry: SchemaRegistry,
  rootSchema: string,
  path: string,
): ResolvedColumn | undefined {
  const parts = path.split('.');
  const initialSchema = maybeUndefined(registry[rootSchema]);

  if (!initialSchema) {
    return undefined;
  }

  let currentSchema = initialSchema;

  for (let i = 0; i < parts.length; i++) {
    const part = parts[i];
    const entry = maybeUndefined(currentSchema[part]);

    if (!entry) {
      // Check if we're at the last level and the current schema has a
      // parameterized entry that could match any key
      // This handles cases like "args.foo" where "args" is parameterized
      // and "foo" is the parameter key
      // Actually, we need to check if the previous entry was parameterized
      return undefined;
    }

    if (isSchemaRef(entry)) {
      // Follow the reference to another schema
      const referencedSchema = maybeUndefined(registry[entry.ref]);
      if (!referencedSchema) {
        return undefined;
      }
      currentSchema = referencedSchema;
      // Continue to next part of path
    } else if (isParameterizedColumnDef(entry)) {
      // For parameterized columns, the remaining path parts are the parameter key
      const remainingParts = parts.slice(i + 1);
      if (remainingParts.length === 0) {
        // Just "args" with no key - return the parameterized def without a key
        return {def: entry};
      }
      // The remaining parts form the parameter key (e.g., "foo" or "foo.bar")
      return {def: entry, paramKey: remainingParts.join('.')};
    } else if (isColumnDef(entry)) {
      // Leaf column - should be the last part of the path
      if (i === parts.length - 1) {
        return {def: entry};
      }
      // Trying to navigate deeper into a leaf column - invalid
      return undefined;
    }
  }

  // We ended on a schema ref without resolving to a leaf
  // This means the path ended at a nested object, not a column
  return undefined;
}

/**
 * Gets the display title parts for a column path.
 *
 * Builds up an array of title parts by traversing the path.
 * For example, "manager.manager.name" -> ["Manager", "Manager", "Name"]
 * Uses schema-defined titles where available.
 *
 * @param registry The schema registry
 * @param rootSchema The root schema name
 * @param path The column path
 * @returns Array of title parts for the column
 */
export function getColumnTitleParts(
  registry: SchemaRegistry,
  rootSchema: string,
  path: string,
): m.Children[] {
  const parts = path.split('.');
  const titleParts: m.Children[] = [];
  const initialSchema = maybeUndefined(registry[rootSchema]);

  if (!initialSchema) {
    // Fallback: return parts as-is
    return parts;
  }

  let currentSchema = initialSchema;

  for (let i = 0; i < parts.length; i++) {
    const part = parts[i];
    const entry = maybeUndefined(currentSchema[part]);

    if (!entry) {
      // Unknown entry - use the part name as-is
      titleParts.push(part);
      break;
    }

    if (isSchemaRef(entry)) {
      // Use ref's title or the part name as-is
      titleParts.push(entry.title ?? part);
      // Follow the reference
      const referencedSchema = maybeUndefined(registry[entry.ref]);
      if (!referencedSchema) {
        break;
      }
      currentSchema = referencedSchema;
    } else if (isParameterizedColumnDef(entry)) {
      // For parameterized columns, remaining parts are the key
      const remainingParts = parts.slice(i + 1);
      const paramKey = remainingParts.join('.');

      if (typeof entry.title === 'function' && paramKey) {
        titleParts.push(entry.title(paramKey));
      } else if (typeof entry.title === 'string') {
        titleParts.push(entry.title);
        if (paramKey) {
          titleParts.push(paramKey);
        }
      } else {
        titleParts.push(part);
        if (paramKey) {
          titleParts.push(paramKey);
        }
      }
      break;
    } else if (isColumnDef(entry)) {
      // Leaf column - use its title or the part name as-is
      titleParts.push(entry.title ?? part);
      break;
    }
  }

  return titleParts;
}

/**
 * Gets the display title for a column path as renderable content.
 * Parts are joined with a separator element.
 *
 * @param registry The schema registry
 * @param rootSchema The root schema name
 * @param path The column path
 * @returns The display title as m.Children
 */
export function getColumnTitle(
  registry: SchemaRegistry,
  rootSchema: string,
  path: string,
): m.Children {
  const parts = getColumnTitleParts(registry, rootSchema, path);
  if (parts.length === 0) return path;
  if (parts.length === 1) return parts[0];

  // Join with separator
  const result: m.Children[] = [];
  for (let i = 0; i < parts.length; i++) {
    if (i > 0) {
      result.push(m('span.pf-data-grid__title-separator', ' > '));
    }
    result.push(parts[i]);
  }
  return result;
}

/**
 * Gets the string title for a column path, suitable for exports.
 * Returns titleString if available, otherwise falls back to the column path.
 */
export function getColumnTitleString(
  registry: SchemaRegistry,
  rootSchema: string,
  path: string,
): string {
  const resolved = resolveColumnPath(registry, rootSchema, path);
  return resolved?.def.titleString ?? path;
}

/**
 * Gets the display title for a column path as a plain string.
 * This is like getColumnTitle but returns a string instead of m.Children.
 * Parts are joined with " > " separator.
 *
 * For example: "manager.manager.name" -> "Manager > Manager > Name"
 */
export function getColumnDisplayTitleString(
  registry: SchemaRegistry,
  rootSchema: string,
  path: string,
): string {
  const parts = getColumnTitleParts(registry, rootSchema, path);
  if (parts.length === 0) return path;

  // Convert m.Children parts to strings
  const stringParts = parts.map((part) => {
    if (typeof part === 'string') return part;
    if (typeof part === 'number') return String(part);
    // For complex m.Children, fall back to the path segment
    return path.split('.')[parts.indexOf(part)] ?? String(part);
  });

  return stringParts.join(' > ');
}

/**
 * Gets the filter type for a column path.
 */
export function getColumnFilterType(
  registry: SchemaRegistry,
  rootSchema: string,
  path: string,
): 'numeric' | 'string' | undefined {
  const resolved = resolveColumnPath(registry, rootSchema, path);
  return resolved?.def.filterType;
}

/**
 * Gets the cell renderer for a column path.
 */
export function getColumnCellRenderer(
  registry: SchemaRegistry,
  rootSchema: string,
  path: string,
): CellRenderer | undefined {
  const resolved = resolveColumnPath(registry, rootSchema, path);
  return resolved?.def.cellRenderer;
}

/**
 * Gets the cell formatter for a column path.
 */
export function getColumnCellFormatter(
  registry: SchemaRegistry,
  rootSchema: string,
  path: string,
): CellFormatter | undefined {
  const resolved = resolveColumnPath(registry, rootSchema, path);
  return resolved?.def.cellFormatter;
}

/**
 * Gets whether distinct values filtering is enabled for a column path.
 */
export function getColumnDistinctValues(
  registry: SchemaRegistry,
  rootSchema: string,
  path: string,
): boolean | undefined {
  const resolved = resolveColumnPath(registry, rootSchema, path);
  return resolved?.def.distinctValues;
}

/**
 * Gets the context menu renderer for a column path.
 */
export function getColumnContextMenuRenderer(
  registry: SchemaRegistry,
  rootSchema: string,
  path: string,
): ((builtins: ContextMenuBuiltins) => m.Children) | undefined {
  const resolved = resolveColumnPath(registry, rootSchema, path);
  return resolved?.def.contextMenuRenderer;
}

/**
 * Gets the cell context menu renderer for a column path.
 */
export function getColumnCellContextMenuRenderer(
  registry: SchemaRegistry,
  rootSchema: string,
  path: string,
):
  | ((
      value: SqlValue,
      row: Row,
      builtins: CellContextMenuBuiltins,
    ) => m.Children)
  | undefined {
  const resolved = resolveColumnPath(registry, rootSchema, path);
  return resolved?.def.cellContextMenuRenderer;
}
