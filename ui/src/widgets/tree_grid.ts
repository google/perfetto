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
import {Grid, GridCell, GridColumn, GridRow} from './grid';

/**
 * A row of data with a hierarchical path key and associated cell data.
 */
export interface TreeGridRow {
  /**
   * Slash-separated path that defines the tree hierarchy.
   * Example: "root/child/grandchild"
   */
  readonly path: string;

  /**
   * Cell data for this row. The first cell will have the tree controls
   * (indent, chevron) added automatically.
   */
  readonly cells: ReadonlyArray<m.Children>;
}

/**
 * Internal tree node structure.
 */
interface TreeNode {
  readonly name: string;
  readonly fullPath: string;
  readonly depth: number;
  readonly children: Map<string, TreeNode>;
  cells?: ReadonlyArray<m.Children>;
  collapsed: boolean;
  parent?: TreeNode;
}

/**
 * TreeGrid component attributes.
 */
export interface TreeGridAttrs {
  /**
   * Column definitions. The first column will automatically include
   * tree controls (indent and chevron).
   */
  readonly columns: ReadonlyArray<GridColumn>;

  /**
   * Row data with hierarchical paths.
   */
  readonly rows: ReadonlyArray<TreeGridRow>;

  /**
   * Optional virtualization configuration.
   */
  readonly virtualization?: {
    readonly rowHeightPx: number;
  };

  /**
   * Whether to fill parent container height.
   */
  readonly fillHeight?: boolean;

  /**
   * Optional CSS class name.
   */
  readonly className?: string;

  /**
   * Path separator character. Default is '/'.
   */
  readonly separator?: string;

  /**
   * Content to display when there are no rows.
   */
  readonly emptyState?: m.Children;
}

/**
 * TreeGrid - A grid widget that automatically organizes rows into a tree
 * structure based on slash-separated path keys.
 *
 * Example usage:
 * ```typescript
 * m(TreeGrid, {
 *   columns: [
 *     {key: 'name', header: m(GridHeaderCell, 'Name')},
 *     {key: 'value', header: m(GridHeaderCell, 'Value')},
 *   ],
 *   rows: [
 *     {
 *       path: 'root/child1/leaf1',
 *       cells: [m(GridCell, 'Leaf 1'), m(GridCell, '100')]
 *     },
 *     {
 *       path: 'root/child1/leaf2',
 *       cells: [m(GridCell, 'Leaf 2'), m(GridCell, '200')]
 *     },
 *     {
 *       path: 'root/child2/leaf1',
 *       cells: [m(GridCell, 'Leaf 1'), m(GridCell, '300')]
 *     },
 *   ],
 * })
 * ```
 *
 * This will create a tree:
 * - root (expandable)
 *   - child1 (expandable)
 *     - Leaf 1 (leaf) - 100
 *     - Leaf 2 (leaf) - 200
 *   - child2 (expandable)
 *     - Leaf 1 (leaf) - 300
 */
export class TreeGrid implements m.ClassComponent<TreeGridAttrs> {
  private root: TreeNode = this.createRootNode();
  private collapsedPaths: Set<string> = new Set();
  private lastRowsJson = '';

  private createRootNode(): TreeNode {
    return {
      name: '',
      fullPath: '',
      depth: -1,
      children: new Map(),
      collapsed: false,
    };
  }

  /**
   * Build the tree structure from flat row data.
   * Preserves collapsed state from previous builds.
   */
  private buildTree(rows: ReadonlyArray<TreeGridRow>, separator: string): void {
    this.root = this.createRootNode();

    for (const row of rows) {
      const parts = row.path.split(separator).filter((p) => p.length > 0);
      let currentNode = this.root;
      let currentPath = '';

      for (let i = 0; i < parts.length; i++) {
        const part = parts[i];
        currentPath = currentPath ? `${currentPath}${separator}${part}` : part;

        if (!currentNode.children.has(part)) {
          const newNode: TreeNode = {
            name: part,
            fullPath: currentPath,
            depth: i,
            children: new Map(),
            // Restore collapsed state if it was previously collapsed
            collapsed: this.collapsedPaths.has(currentPath),
            parent: currentNode,
            // Only set cells on leaf nodes (last part of path)
            cells: i === parts.length - 1 ? row.cells : undefined,
          };
          currentNode.children.set(part, newNode);
        }

        currentNode = currentNode.children.get(part)!;
      }
    }
  }

  /**
   * Flatten the tree into a list of visible rows.
   */
  private flattenTree(node: TreeNode, output: TreeNode[]): void {
    // Don't add the root node itself
    if (node.depth >= 0) {
      output.push(node);
    }

    // If not collapsed, add children
    if (!node.collapsed) {
      // Sort children by name for consistent display
      const sortedChildren = Array.from(node.children.values()).sort((a, b) =>
        a.name.localeCompare(b.name),
      );

      for (const child of sortedChildren) {
        this.flattenTree(child, output);
      }
    }
  }

  view({attrs}: m.Vnode<TreeGridAttrs>) {
    const separator = attrs.separator ?? '/';

    // Only rebuild tree if data has changed
    const rowsJson = JSON.stringify(attrs.rows.map((r) => r.path));
    if (rowsJson !== this.lastRowsJson) {
      this.lastRowsJson = rowsJson;
      this.buildTree(attrs.rows, separator);
    }

    // Flatten tree to visible rows
    const visibleNodes: TreeNode[] = [];
    this.flattenTree(this.root, visibleNodes);

    // Convert tree nodes to grid rows
    const gridRows: GridRow[] = visibleNodes.map((node) => {
      const row: m.Children[] = [];
      const hasChildren = node.children.size > 0;
      const isLeaf = !hasChildren;

      // First cell gets tree controls
      const firstCell = node.cells?.[0] ?? m(GridCell, node.name);

      // Extract the content from the first cell and add tree controls
      const chevron = isLeaf
        ? ('leaf' as const)
        : node.collapsed
          ? ('collapsed' as const)
          : ('expanded' as const);

      row.push(
        m(
          GridCell,
          {
            indent: node.depth,
            chevron,
            onChevronClick: hasChildren
              ? () => {
                  node.collapsed = !node.collapsed;
                  // Update collapsed paths set
                  if (node.collapsed) {
                    this.collapsedPaths.add(node.fullPath);
                  } else {
                    this.collapsedPaths.delete(node.fullPath);
                  }
                  m.redraw();
                }
              : undefined,
            ...((firstCell as m.Vnode).attrs ?? {}),
          },
          (firstCell as m.Vnode).children ?? node.name,
        ),
      );

      // Add remaining cells
      if (node.cells) {
        for (let i = 1; i < node.cells.length; i++) {
          row.push(node.cells[i]);
        }
      } else {
        // For intermediate nodes without data, add empty cells
        for (let i = 1; i < attrs.columns.length; i++) {
          row.push(m(GridCell, ''));
        }
      }

      return row;
    });

    return m(Grid, {
      columns: attrs.columns,
      rowData: gridRows,
      virtualization: attrs.virtualization,
      fillHeight: attrs.fillHeight,
      className: attrs.className,
      emptyState: attrs.emptyState,
    });
  }
}
