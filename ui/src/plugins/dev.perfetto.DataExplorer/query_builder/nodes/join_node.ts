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
  nextNodeId,
  NodeType,
  SecondaryInputSpec,
  NodeContext,
} from '../../query_node';
import {getSecondaryInput} from '../graph_utils';
import protos from '../../../../protos';
import {ColumnInfo, legacyDeserializeType} from '../column_info';
import {Callout} from '../../../../widgets/callout';
import {NodeIssues} from '../node_issues';
import {Switch} from '../../../../widgets/switch';
import {
  StructuredQueryBuilder,
  JoinCondition,
} from '../structured_query_builder';
import {loadNodeDoc} from '../node_doc_loader';
import {NodeModifyAttrs, NodeDetailsAttrs} from '../../node_types';
import {NodeTitle} from '../node_styling_widgets';
import {JoinConditionSelector, JoinConditionDisplay} from '../join_widgets';
import {ResizableSqlEditor} from '../widgets';

// Serializable node configuration.
export interface JoinNodeAttrs {
  leftQueryAlias: string;
  rightQueryAlias: string;
  conditionType: 'equality' | 'freeform';
  joinType: 'INNER' | 'LEFT';
  leftColumn: string;
  rightColumn: string;
  sqlExpression: string;
  // Column selections from left and right sources with checked/alias state
  leftColumns?: ColumnInfo[];
  rightColumns?: ColumnInfo[];
}

export class JoinNode implements QueryNode {
  readonly nodeId: string;
  readonly type = NodeType.kJoin;
  secondaryInputs: SecondaryInputSpec;
  nextNodes: QueryNode[];
  readonly attrs: JoinNodeAttrs;
  readonly context: NodeContext;

  get leftNode(): QueryNode | undefined {
    return getSecondaryInput(this, 0);
  }

  get rightNode(): QueryNode | undefined {
    return getSecondaryInput(this, 1);
  }

  get finalCols(): ColumnInfo[] {
    const result: ColumnInfo[] = [];
    for (const col of this.attrs.leftColumns ?? []) {
      if (col.checked) result.push(col);
    }
    for (const col of this.attrs.rightColumns ?? []) {
      if (col.checked) result.push(col);
    }
    return result;
  }

  constructor(attrs: JoinNodeAttrs, context: NodeContext) {
    this.nodeId = nextNodeId();
    this.attrs = {
      ...attrs,
      leftQueryAlias: attrs.leftQueryAlias ?? 'left',
      rightQueryAlias: attrs.rightQueryAlias ?? 'right',
      conditionType: attrs.conditionType ?? 'equality',
      joinType: attrs.joinType ?? 'INNER',
      leftColumn: attrs.leftColumn ?? '',
      rightColumn: attrs.rightColumn ?? '',
      sqlExpression: attrs.sqlExpression ?? '',
      leftColumns: attrs.leftColumns ?? [],
      rightColumns: attrs.rightColumns ?? [],
    };
    this.context = {
      ...context,
      autoExecute: context.autoExecute ?? false,
    };
    this.secondaryInputs = {
      connections: new Map(),
      min: 2,
      max: 2,
      portNames: (portIndex: number) =>
        portIndex === 0
          ? this.attrs.leftQueryAlias
          : this.attrs.rightQueryAlias,
    };
    this.nextNodes = [];

    // Initialize column arrays from connected nodes if empty
    // BUT: Only do this if we don't already have deserialized column data
    // If leftColumns/rightColumns exist, they came from deserialization and
    // we should preserve them. updateColumnArrays() will be called later
    // when connections are restored via onPrevNodesUpdated()
    if (
      (!this.attrs.leftColumns || this.attrs.leftColumns.length === 0) &&
      (!this.attrs.rightColumns || this.attrs.rightColumns.length === 0)
    ) {
      this.updateColumnArrays();
    }
  }

  onPrevNodesUpdated() {
    // Update column arrays when input nodes change
    this.updateColumnArrays();
  }

  // Update column arrays when nodes change or on initialization
  private updateColumnArrays() {
    const buildDescriptors = (
      sourceCols: ColumnInfo[],
      existing: ColumnInfo[],
    ): ColumnInfo[] => {
      const isFirstInit = existing.length === 0;
      return sourceCols.map((col) => {
        const oldCol = existing.find((c) => c.name === col.name);
        return {
          name: col.name,
          type: col.type,
          description: col.description,
          checked: isFirstInit ? false : oldCol?.checked ?? false,
          alias: oldCol?.alias,
          typeUserModified: oldCol?.typeUserModified,
        };
      });
    };

    if (this.leftNode) {
      this.attrs.leftColumns = buildDescriptors(
        this.leftNode.finalCols,
        this.attrs.leftColumns ?? [],
      );
    } else {
      this.attrs.leftColumns = [];
    }

    if (this.rightNode) {
      this.attrs.rightColumns = buildDescriptors(
        this.rightNode.finalCols,
        this.attrs.rightColumns ?? [],
      );
    } else {
      this.attrs.rightColumns = [];
    }
  }

  validate(): boolean {
    // Clear any previous errors at the start of validation
    if (this.context.issues) {
      this.context.issues.clear();
    }

    if (
      this.secondaryInputs.connections.size !== 2 ||
      !this.leftNode ||
      !this.rightNode
    ) {
      this.setValidationError(
        'Join node requires exactly two sources (left and right).',
      );
      return false;
    }

    if (!this.attrs.leftQueryAlias || !this.attrs.rightQueryAlias) {
      this.setValidationError(
        'Both left and right query aliases are required.',
      );
      return false;
    }

    if (this.attrs.conditionType === 'equality') {
      if (!this.attrs.leftColumn || !this.attrs.rightColumn) {
        this.setValidationError(
          'Both left and right columns are required for equality join.',
        );
        return false;
      }
    } else {
      if (!this.attrs.sqlExpression) {
        this.setValidationError(
          'SQL expression for join condition is required.',
        );
        return false;
      }
    }

    if (!this.leftNode.validate()) {
      this.setValidationError(
        this.leftNode.context.issues?.queryError?.message ??
          `Left node '${this.leftNode.getTitle()}' is invalid`,
      );
      return false;
    }

    if (!this.rightNode.validate()) {
      this.setValidationError(
        this.rightNode.context.issues?.queryError?.message ??
          `Right node '${this.rightNode.getTitle()}' is invalid`,
      );
      return false;
    }

    // Check if there are any columns selected
    const leftColumns = this.attrs.leftColumns ?? [];
    const rightColumns = this.attrs.rightColumns ?? [];
    const hasCheckedColumns =
      leftColumns.some((c) => c.checked) || rightColumns.some((c) => c.checked);

    if (!hasCheckedColumns) {
      this.setValidationError(
        'No columns selected. Select at least one column from either source.',
      );
      return false;
    }

    return true;
  }

  private setValidationError(message: string): void {
    if (!this.context.issues) {
      this.context.issues = new NodeIssues();
    }
    this.context.issues.queryError = new Error(message);
  }

  getTitle(): string {
    return 'Join';
  }

  nodeInfo(): m.Children {
    return loadNodeDoc('join');
  }

  nodeDetails(): NodeDetailsAttrs {
    let content: m.Children;

    if (this.attrs.conditionType === 'equality') {
      if (this.attrs.leftColumn && this.attrs.rightColumn) {
        content = m(JoinConditionDisplay, {
          leftAlias: this.attrs.leftQueryAlias,
          rightAlias: this.attrs.rightQueryAlias,
          leftColumn: this.attrs.leftColumn,
          rightColumn: this.attrs.rightColumn,
        });
      } else {
        content = m('.pf-exp-node-details-message', 'No condition set');
      }
    } else {
      if (this.attrs.sqlExpression) {
        content = m('code.pf-exp-sql-expression', this.attrs.sqlExpression);
      } else {
        content = m('.pf-exp-node-details-message', 'No condition set');
      }
    }

    return {
      content: [NodeTitle(this.getTitle()), content],
    };
  }

  nodeSpecificModify(): NodeModifyAttrs {
    this.validate();
    const error = this.context.issues?.queryError;

    const sections: NodeModifyAttrs['sections'] = [];
    const bottomRightButtons: NodeModifyAttrs['bottomRightButtons'] = [];

    // Add error if present
    if (error) {
      sections.push({
        content: m(Callout, {icon: 'error'}, error.message),
      });
    }

    // Join type section
    sections.push({
      title: 'Join Type',
      content: m(Switch, {
        checked: this.attrs.joinType === 'LEFT',
        label: 'Left Join',
        onchange: (e: Event) => {
          const target = e.target as HTMLInputElement;
          this.attrs.joinType = target.checked ? 'LEFT' : 'INNER';
          this.context.onchange?.();
        },
      }),
    });

    // Join condition section with integrated column selection
    sections.push({
      content:
        this.attrs.conditionType === 'equality'
          ? m(JoinConditionSelector, {
              leftLabel: 'Left',
              rightLabel: 'Right',
              leftColumns: this.attrs.leftColumns ?? [],
              rightColumns: this.attrs.rightColumns ?? [],
              leftColumn: this.attrs.leftColumn,
              rightColumn: this.attrs.rightColumn,
              onLeftColumnChange: (columnName: string) => {
                this.attrs.leftColumn = columnName;
                this.context.onchange?.();
              },
              onRightColumnChange: (columnName: string) => {
                this.attrs.rightColumn = columnName;
                this.context.onchange?.();
              },
              onLeftColumnToggle: (index: number, checked: boolean) => {
                if (this.attrs.leftColumns) {
                  this.attrs.leftColumns[index].checked = checked;
                  this.context.onchange?.();
                }
              },
              onRightColumnToggle: (index: number, checked: boolean) => {
                if (this.attrs.rightColumns) {
                  this.attrs.rightColumns[index].checked = checked;
                  this.context.onchange?.();
                }
              },
              onLeftColumnAlias: (index: number, alias: string) => {
                if (this.attrs.leftColumns) {
                  this.attrs.leftColumns[index].alias =
                    alias.trim() === '' ? undefined : alias;
                  this.context.onchange?.();
                }
              },
              onRightColumnAlias: (index: number, alias: string) => {
                if (this.attrs.rightColumns) {
                  this.attrs.rightColumns[index].alias =
                    alias.trim() === '' ? undefined : alias;
                  this.context.onchange?.();
                }
              },
            })
          : m(ResizableSqlEditor, {
              sql: this.attrs.sqlExpression,
              onUpdate: (text: string) => {
                this.attrs.sqlExpression = text;
                m.redraw();
              },
              onExecute: (text: string) => {
                this.attrs.sqlExpression = text.trim();
                m.redraw();
              },
            }),
    });

    // Mode switch button
    bottomRightButtons.push({
      label:
        this.attrs.conditionType === 'equality'
          ? 'Switch to freeform SQL'
          : 'Switch to equality',
      icon: this.attrs.conditionType === 'equality' ? 'code' : 'view_column',
      onclick: () => {
        this.attrs.conditionType =
          this.attrs.conditionType === 'equality' ? 'freeform' : 'equality';
        // Disable auto-execute in freeform SQL mode
        this.context.autoExecute = this.attrs.conditionType === 'equality';
        this.context.onchange?.();
      },
      compact: true,
    });

    return {
      info: 'Combines rows from exactly two inputs side-by-side by matching on a join key. Each row from the first input is matched with rows from the second input where the join column values are equal.',
      sections,
      bottomRightButtons,
    };
  }

  clone(): QueryNode {
    return new JoinNode(
      {
        ...this.attrs,
        leftColumns: this.attrs.leftColumns?.map((c) => ({...c})),
        rightColumns: this.attrs.rightColumns?.map((c) => ({...c})),
      },
      this.context,
    );
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (!this.validate() || !this.leftNode || !this.rightNode) return;

    const condition: JoinCondition =
      this.attrs.conditionType === 'equality'
        ? {
            type: 'equality',
            leftColumn: this.attrs.leftColumn,
            rightColumn: this.attrs.rightColumn,
          }
        : {
            type: 'freeform',
            leftQueryAlias: this.attrs.leftQueryAlias,
            rightQueryAlias: this.attrs.rightQueryAlias,
            sqlExpression: this.attrs.sqlExpression,
          };

    const sq = StructuredQueryBuilder.withJoin(
      this.leftNode,
      this.rightNode,
      this.attrs.joinType,
      condition,
      this.nodeId,
    );

    if (sq === undefined) return undefined;

    // Add select_columns to explicitly specify which columns to return
    // Include aliases if specified
    sq.selectColumns = this.finalCols.map((col) => {
      const selectCol = new protos.PerfettoSqlStructuredQuery.SelectColumn();
      selectCol.columnNameOrExpression = col.name;
      if (col.alias) {
        selectCol.alias = col.alias;
      }
      return selectCol;
    });

    return sq;
  }

  static deserializeState(attrs: JoinNodeAttrs): JoinNodeAttrs {
    // Migrate legacy columnName field and string types
    const migrateColumns = (
      cols?: (ColumnInfo & {columnName?: string})[],
    ): ColumnInfo[] =>
      (cols ?? []).map((c) => ({
        name: c.columnName ?? c.name,
        type: legacyDeserializeType(c.type),
        checked: c.checked,
        alias: c.alias,
        typeUserModified: c.typeUserModified,
      }));

    return {
      ...attrs,
      conditionType: attrs.conditionType ?? 'equality',
      joinType: attrs.joinType ?? 'INNER',
      leftColumn: attrs.leftColumn ?? '',
      rightColumn: attrs.rightColumn ?? '',
      sqlExpression: attrs.sqlExpression ?? '',
      leftColumns: migrateColumns(
        attrs.leftColumns as (ColumnInfo & {columnName?: string})[],
      ),
      rightColumns: migrateColumns(
        attrs.rightColumns as (ColumnInfo & {columnName?: string})[],
      ),
    };
  }
}
