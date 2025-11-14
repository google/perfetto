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
import {
  QueryNode,
  QueryNodeState,
  nextNodeId,
  NodeType,
  ModificationNode,
} from '../../query_node';
import {ColumnInfo} from '../column_info';
import protos from '../../../../protos';
import {Button} from '../../../../widgets/button';
import {
  PopupMultiSelect,
  MultiSelectOption,
  MultiSelectDiff,
} from '../../../../widgets/multiselect';
import {
  StructuredQueryBuilder,
  SortCriterion as BuilderSortCriterion,
} from '../structured_query_builder';
import {setValidationError} from '../node_issues';

export interface SortCriterion {
  colName: string;
  direction: 'ASC' | 'DESC';
}

export interface SortNodeState extends QueryNodeState {
  prevNode: QueryNode;
  sortColNames?: string[]; // For backwards compatibility
  sortCriteria?: SortCriterion[];
}

export class SortNode implements ModificationNode {
  readonly nodeId: string;
  readonly type = NodeType.kSort;
  readonly prevNode: QueryNode;
  nextNodes: QueryNode[];
  readonly state: SortNodeState;
  sortCols: ColumnInfo[];
  private showEditControls = false;

  constructor(state: SortNodeState) {
    this.nodeId = nextNodeId();
    this.state = state;
    this.prevNode = state.prevNode;
    this.nextNodes = [];

    this.state.sortCriteria = this.state.sortCriteria ?? [];
    this.sortCols = this.resolveSortCols();
  }

  private resolveSortCols(): ColumnInfo[] {
    if (!this.state.sortCriteria) {
      return [];
    }
    const sourceCols = this.sourceCols;
    return this.state.sortCriteria
      .map((criterion) => sourceCols.find((c) => c.name === criterion.colName))
      .filter((c): c is ColumnInfo => c !== undefined);
  }

  get sourceCols(): ColumnInfo[] {
    return this.prevNode?.finalCols ?? [];
  }

  get finalCols(): ColumnInfo[] {
    return this.sourceCols;
  }

  getTitle(): string {
    return 'Sort';
  }

  nodeDetails(): m.Child {
    if (!this.state.sortCriteria) {
      this.state.sortCriteria = [];
    }

    const sortOptions: MultiSelectOption[] = this.sourceCols.map((col) => ({
      id: col.name,
      name: col.name,
      checked:
        this.state.sortCriteria?.some((c) => c.colName === col.name) ?? false,
    }));

    const label =
      this.state.sortCriteria.length > 0
        ? this.state.sortCriteria
            .map((c) =>
              c.direction === 'DESC' ? `${c.colName} ↓` : `${c.colName} ↑`,
            )
            .join(', ')
        : 'None';

    return m('div', [
      m(
        '.pf-sort-selector',
        {
          style: {
            display: 'flex',
            alignItems: 'center',
            gap: '8px',
            marginBottom:
              this.showEditControls && this.state.sortCriteria.length > 0
                ? '8px'
                : '0',
          },
        },
        m('label', 'Sort by:'),
        m(PopupMultiSelect, {
          label,
          options: sortOptions,
          showNumSelected: false,
          compact: true,
          onChange: (diffs: MultiSelectDiff[]) => {
            if (!this.state.sortCriteria) {
              this.state.sortCriteria = [];
            }
            for (const diff of diffs) {
              if (diff.checked) {
                // Add column if not already present
                if (
                  !this.state.sortCriteria.some((c) => c.colName === diff.id)
                ) {
                  this.state.sortCriteria.push({
                    colName: diff.id,
                    direction: 'ASC',
                  });
                }
              } else {
                // Remove column
                this.state.sortCriteria = this.state.sortCriteria.filter(
                  (c) => c.colName !== diff.id,
                );
              }
            }
            this.sortCols = this.resolveSortCols();
            this.state.onchange?.();
          },
        }),
        this.state.sortCriteria.length > 0 &&
          m(Button, {
            icon: 'edit',
            minimal: true,
            onclick: () => {
              this.showEditControls = !this.showEditControls;
              m.redraw();
            },
          }),
      ),
      this.showEditControls &&
        this.state.sortCriteria?.map((criterion, index) =>
          m(
            '.sort-criterion',
            {
              draggable: true,
              ondragstart: (e: DragEvent) => {
                e.dataTransfer!.setData('text/plain', index.toString());
              },
              ondragover: (e: DragEvent) => {
                e.preventDefault();
              },
              ondrop: (e: DragEvent) => {
                e.preventDefault();
                if (!this.state.sortCriteria) return;
                const from = parseInt(
                  e.dataTransfer!.getData('text/plain'),
                  10,
                );
                const to = index;

                const newSortCriteria = [...this.state.sortCriteria];
                const [removed] = newSortCriteria.splice(from, 1);
                newSortCriteria.splice(to, 0, removed);
                this.state.sortCriteria = newSortCriteria;
                this.sortCols = this.resolveSortCols();
                this.state.onchange?.();
                m.redraw();
              },
            },
            [
              m('span.pf-drag-handle', '☰'),
              m('span', criterion.colName),
              m(Button, {
                label: criterion.direction,
                onclick: () => {
                  if (this.state.sortCriteria) {
                    this.state.sortCriteria[index].direction =
                      criterion.direction === 'ASC' ? 'DESC' : 'ASC';
                    this.state.onchange?.();
                    m.redraw();
                  }
                },
              }),
            ],
          ),
        ),
    ]);
  }

  nodeSpecificModify(): m.Child {
    return null;
  }

  nodeInfo(): m.Children {
    return m(
      'div',
      m(
        'p',
        'Order rows by one or more columns, either ascending or descending. Drag to reorder sort columns.',
      ),
      m(
        'p',
        'When you specify multiple columns, the first is the primary sort, the second is the tiebreaker, and so on.',
      ),
      m(
        'p',
        m('strong', 'Example:'),
        ' Sort by ',
        m('code', 'ts'),
        ' ascending, then by ',
        m('code', 'dur'),
        ' descending to see events in chronological order with longest durations first for each timestamp.',
      ),
    );
  }

  validate(): boolean {
    // Clear any previous errors at the start of validation
    if (this.state.issues) {
      this.state.issues.clear();
    }

    if (this.prevNode === undefined) {
      setValidationError(this.state, 'No input node connected');
      return false;
    }

    if (!this.prevNode.validate()) {
      setValidationError(this.state, 'Previous node is invalid');
      return false;
    }

    if (this.sortCols === undefined || this.sortCols.length === 0) {
      setValidationError(this.state, 'No sort columns selected');
      return false;
    }

    return true;
  }

  clone(): QueryNode {
    return new SortNode(this.state);
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (this.prevNode === undefined) return undefined;

    if (this.sortCols.length === 0) {
      return this.prevNode.getStructuredQuery();
    }

    const criteria: BuilderSortCriterion[] = [];
    for (const criterion of this.state.sortCriteria ?? []) {
      const col = this.sortCols.find(
        (c) => c.column.name === criterion.colName,
      );
      if (!col) continue;

      criteria.push({
        columnName: col.column.name,
        direction: criterion.direction,
      });
    }

    if (criteria.length === 0) {
      return this.prevNode.getStructuredQuery();
    }

    return StructuredQueryBuilder.withOrderBy(
      this.prevNode,
      criteria,
      this.nodeId,
    );
  }

  serializeState(): object {
    // Only return serializable fields, excluding callbacks and objects
    // that might contain circular references
    return {
      sortColNames: this.state.sortColNames,
      sortCriteria: this.state.sortCriteria,
      comment: this.state.comment,
    };
  }

  static deserializeState(state: SortNodeState): SortNodeState {
    return {
      ...state,
      prevNode: undefined as unknown as QueryNode,
    };
  }
}
