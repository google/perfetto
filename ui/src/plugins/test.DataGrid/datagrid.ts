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

import m from 'mithril';
import {Grid, GridHeaderCell} from '../../widgets/grid';
import {MenuItem} from '../../widgets/menu';
import {type ColumnMetadata} from './sql_generator';

class LazyMenu implements m.ClassComponent<{renderItems: () => m.Children}> {
  view({attrs}: m.CVnode<{renderItems: () => m.Children}>) {
    return attrs.renderItems();
  }
}

export interface DataGridAttrs {
  readonly schema: Record<string, unknown>;
  readonly columns: ReadonlyArray<ColumnMetadata>;
  readonly onColumnsChanged: (columns: ColumnMetadata[]) => void;
}

export function resolveDisplayNameParts(path: string[], schema: Record<string, unknown>): string[] {
  let currentSchema: Record<string, unknown> = schema;
  const resolvedNames: string[] = [];
  
  for (let i = 0; i < path.length; i++) {
    const segment = path[i];
    let displayName = segment;
    
    if (currentSchema && typeof currentSchema === 'object') {
      const entry = currentSchema[segment];
      if (entry && typeof entry === 'object') {
        const entryObj = entry as {name?: unknown; schema?: unknown; parameterized?: unknown};
        
        if (entryObj.parameterized === true && i + 1 < path.length) {
          const niceName = typeof entryObj.name === 'string' ? entryObj.name : segment;
          const paramSegment = path[i + 1];
          displayName = `${niceName}[${paramSegment}]`;
          
          resolvedNames.push(displayName);
          i++;
          currentSchema = {};
          continue;
        }
        
        if (typeof entryObj.name === 'string') {
          displayName = entryObj.name;
        }
        
        const nestedSchema = entryObj.schema;
        if (nestedSchema && typeof nestedSchema === 'object') {
          currentSchema = nestedSchema as Record<string, unknown>;
        } else {
          currentSchema = {};
        }
      } else {
        currentSchema = {};
      }
    } else {
      currentSchema = {};
    }
    
    resolvedNames.push(displayName);
  }
  
  return resolvedNames;
}

export class DataGrid implements m.ClassComponent<DataGridAttrs> {
  private renderSchemaMenuItems(
    attrs: DataGridAttrs,
    pathPrefix: string[],
    schema: Record<string, unknown>,
    depth = 0,
  ): m.Children {
    if (depth > 20) {
      return m(MenuItem, {label: '(max depth reached)', disabled: true});
    }

    return Object.entries(schema).map(([key, val]) => {
      if (!val || typeof val !== 'object') return null;
      const currentPath = [...pathPrefix, key];
      const entryObj = val as {name?: unknown; schema?: unknown; parameterized?: unknown};
      const displayName = typeof entryObj.name === 'string' ? entryObj.name : key;
      
      const isParameterized = entryObj.parameterized === true;
      if (isParameterized) {
        return m(
          MenuItem,
          {
            label: displayName,
            onclick: () => {
              const param = prompt(`Enter parameter for ${displayName}:`);
              if (param && param.trim()) {
                this.addColumn(attrs, [...currentPath, param.trim()]);
              }
            },
          },
        );
      }
      
      const nestedSchema = entryObj.schema;
      if (nestedSchema && typeof nestedSchema === 'object') {
        return m(
          MenuItem,
          {
            label: displayName,
          },
          m(LazyMenu, {
            renderItems: () => this.renderSchemaMenuItems(
              attrs,
              currentPath,
              nestedSchema as Record<string, unknown>,
              depth + 1,
            ),
          }),
        );
      } else {
        return m(
          MenuItem,
          {
            label: displayName,
            onclick: () => {
              this.addColumn(attrs, currentPath);
            },
          },
        );
      }
    });
  }

  private addColumn(attrs: DataGridAttrs, path: string[]) {
    const pathStr = path.join('.');
    
    // Avoid duplicate paths
    if (attrs.columns.some((col) => col.path.join('.') === pathStr)) {
      console.log('Column with this path already exists:', pathStr);
      return;
    }
    
    const displayNameParts = resolveDisplayNameParts(path, attrs.schema);
    const uniqueId = `${pathStr}_${Math.random().toString(36).substring(2, 9)}`;
    const newCol: ColumnMetadata = {
      key: uniqueId,
      colId: pathStr,
      path,
      displayNameParts,
    };
    
    attrs.onColumnsChanged([...attrs.columns, newCol]);
  }

  private renderHeaderMenu(attrs: DataGridAttrs): m.Children {
    return m(
      MenuItem,
      {
        label: 'Add column',
        icon: 'add',
      },
      this.renderSchemaMenuItems(attrs, [], attrs.schema),
    );
  }

  view({attrs}: m.CVnode<DataGridAttrs>) {
    return m(Grid, {
      columns: attrs.columns.map((col) => ({
        key: col.key,
        header: m(
          GridHeaderCell,
          {
            menuItems: this.renderHeaderMenu(attrs),
          },
          col.displayNameParts.map((part, index) => [
            index > 0 && m('span.pf-test-grid-separator', '▸'),
            part,
          ]),
        ),
      })),
      rowData: [],
      fillHeight: true,
      emptyState: m('div', 'No data available'),
    });
  }
}
