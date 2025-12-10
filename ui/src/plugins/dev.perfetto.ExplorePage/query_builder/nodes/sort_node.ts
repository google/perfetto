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
} from '../../query_node';
import {ColumnInfo} from '../column_info';
import protos from '../../../../protos';
import {Button} from '../../../../widgets/button';
import {
  StructuredQueryBuilder,
  SortCriterion as BuilderSortCriterion,
} from '../structured_query_builder';
import {setValidationError} from '../node_issues';
import {
  LabeledControl,
  DraggableItem,
  OutlinedMultiSelect,
  MultiSelectOption,
  MultiSelectDiff,
} from '../widgets';
import {NodeDetailsAttrs, NodeModifyAttrs} from '../node_explorer_types';
import {loadNodeDoc} from '../node_doc_loader';
import {createErrorSections} from '../widgets';
import {NodeDetailsMessage} from '../node_styling_widgets';

export interface SortCriterion {
  colName: string;
  direction: 'ASC' | 'DESC';
}

export interface SortNodeState extends QueryNodeState {
  sortColNames?: string[]; // For backwards compatibility
  sortCriteria?: SortCriterion[];
}

export class SortNode implements QueryNode {
  readonly nodeId: string;
  readonly type = NodeType.kSort;
  primaryInput?: QueryNode;
  nextNodes: QueryNode[];
  readonly state: SortNodeState;
  sortCols: ColumnInfo[];

  constructor(state: SortNodeState) {
    this.nodeId = nextNodeId();
    this.state = state;
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
    return this.primaryInput?.finalCols ?? [];
  }

  get finalCols(): ColumnInfo[] {
    return this.sourceCols;
  }

  getTitle(): string {
    return 'Sort';
  }

  nodeDetails(): NodeDetailsAttrs {
    if (!this.state.sortCriteria || this.state.sortCriteria.length === 0) {
      return {
        content: NodeDetailsMessage('No sort columns'),
      };
    }

    const label = this.state.sortCriteria
      .map((c) =>
        c.direction === 'DESC' ? `${c.colName} ↓` : `${c.colName} ↑`,
      )
      .join(', ');

    return {
      content: m('div', label),
    };
  }

  nodeSpecificModify(): NodeModifyAttrs {
    if (!this.state.sortCriteria) {
      this.state.sortCriteria = [];
    }

    const sections: NodeModifyAttrs['sections'] = [
      ...createErrorSections(this),
    ];

    // Column selector section
    sections.push({
      content: this.renderColumnSelector(),
    });

    // Sort criteria list section
    sections.push({
      content: this.renderSortCriteriaList(),
    });

    return {
      info: 'Orders rows by selected columns. Add columns to sort by, then drag to reorder. Click column chips to toggle between ascending (ASC) and descending (DESC) order.',
      sections,
    };
  }

  private renderColumnSelector(): m.Child {
    const sortCriteria = this.state.sortCriteria ?? [];

    const sortOptions: MultiSelectOption[] = this.sourceCols.map((col) => ({
      id: col.name,
      name: col.name,
      checked: sortCriteria.some((c) => c.colName === col.name),
    }));

    const label =
      sortCriteria.length > 0
        ? sortCriteria
            .map((c) =>
              c.direction === 'DESC' ? `${c.colName} ↓` : `${c.colName} ↑`,
            )
            .join(', ')
        : 'None';

    return m(
      LabeledControl,
      {
        label: 'Sort by:',
      },
      m(OutlinedMultiSelect, {
        label,
        options: sortOptions,
        showNumSelected: false,
        onChange: (diffs: MultiSelectDiff[]) => {
          if (!this.state.sortCriteria) {
            this.state.sortCriteria = [];
          }
          for (const diff of diffs) {
            if (diff.checked) {
              // Add column if not already present
              if (!this.state.sortCriteria.some((c) => c.colName === diff.id)) {
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
    );
  }

  private renderSortCriteriaList(): m.Child {
    const sortCriteria = this.state.sortCriteria ?? [];

    if (sortCriteria.length === 0) {
      return null;
    }

    const handleReorder = (from: number, to: number) => {
      if (!this.state.sortCriteria) return;
      const newSortCriteria = [...this.state.sortCriteria];
      const [removed] = newSortCriteria.splice(from, 1);
      newSortCriteria.splice(to, 0, removed);
      this.state.sortCriteria = newSortCriteria;
      this.sortCols = this.resolveSortCols();
      this.state.onchange?.();
      m.redraw();
    };

    return m(
      '.pf-sort-criteria-list',
      sortCriteria.map((criterion, index) =>
        m(
          DraggableItem,
          {
            index,
            onReorder: handleReorder,
          },
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
        ),
      ),
    );
  }

  nodeInfo(): m.Children {
    return loadNodeDoc('sort');
  }

  validate(): boolean {
    // Clear any previous errors at the start of validation
    if (this.state.issues) {
      this.state.issues.clear();
    }

    if (this.primaryInput === undefined) {
      setValidationError(this.state, 'No input node connected');
      return false;
    }

    if (!this.primaryInput.validate()) {
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
    const stateCopy: SortNodeState = {
      sortColNames: this.state.sortColNames
        ? [...this.state.sortColNames]
        : undefined,
      sortCriteria: this.state.sortCriteria?.map((c) => ({...c})),
      filters: this.state.filters?.map((f) => ({...f})),
      filterOperator: this.state.filterOperator,
      onchange: this.state.onchange,
    };
    return new SortNode(stateCopy);
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (this.primaryInput === undefined) return undefined;

    if (this.sortCols.length === 0) {
      return this.primaryInput.getStructuredQuery();
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
      return this.primaryInput.getStructuredQuery();
    }

    return StructuredQueryBuilder.withOrderBy(
      this.primaryInput,
      criteria,
      this.nodeId,
    );
  }

  serializeState(): object {
    // Only return serializable fields, excluding callbacks and objects
    // that might contain circular references
    return {
      primaryInputId: this.primaryInput?.nodeId,
      sortColNames: this.state.sortColNames,
      sortCriteria: this.state.sortCriteria,
    };
  }

  static deserializeState(state: SortNodeState): SortNodeState {
    return {...state};
  }
}
