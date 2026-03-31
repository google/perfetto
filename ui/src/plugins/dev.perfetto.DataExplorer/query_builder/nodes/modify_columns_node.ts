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
import {QueryNode, nextNodeId, NodeType, NodeContext} from '../../query_node';
import {TextInput} from '../../../../widgets/text_input';
import {ColumnInfo, legacyDeserializeType} from '../column_info';
import {PerfettoSqlType} from '../../../../trace_processor/perfetto_sql_type';
import protos from '../../../../protos';
import {NodeIssues} from '../node_issues';
import {StructuredQueryBuilder, ColumnSpec} from '../structured_query_builder';
import {ColumnSelector} from '../column_selector';
import {NodeModifyAttrs, NodeDetailsAttrs} from '../../node_types';
import {
  NodeDetailsMessage,
  NodeDetailsSpacer,
  ColumnName,
} from '../node_styling_widgets';
import {loadNodeDoc} from '../node_doc_loader';
import {renderTypeSelector} from './modify_columns_utils';

// Serializable node configuration.
export interface ModifyColumnsNodeAttrs {
  selectedColumns: ColumnInfo[];
}

export class ModifyColumnsNode implements QueryNode {
  readonly nodeId: string;
  readonly type = NodeType.kModifyColumns;
  primaryInput?: QueryNode;
  nextNodes: QueryNode[];
  readonly attrs: ModifyColumnsNodeAttrs;
  readonly context: NodeContext;

  constructor(attrs: ModifyColumnsNodeAttrs, context: NodeContext) {
    this.nodeId = nextNodeId();
    this.nextNodes = [];

    this.attrs = {
      ...attrs,
      selectedColumns: attrs.selectedColumns ?? [],
    };
    this.context = context;
  }

  get finalCols(): ColumnInfo[] {
    return this.attrs.selectedColumns
      .filter((col) => col.checked)
      .map((col) => {
        const finalName = col.alias ?? col.name;
        return {
          name: finalName,
          checked: true,
          type: col.type,
          alias: undefined,
          typeUserModified: col.typeUserModified,
        };
      });
  }

  onPrevNodesUpdated() {
    if (this.primaryInput === undefined) {
      return;
    }

    const sourceCols = this.primaryInput.finalCols;

    const newSelectedColumns: ColumnInfo[] = sourceCols.map((col) => {
      const oldCol = this.attrs.selectedColumns.find(
        (c) => c.name === col.name,
      );
      return {
        name: col.name,
        type: oldCol?.typeUserModified ? oldCol.type : col.type,
        checked: oldCol?.checked ?? true,
        alias: oldCol?.alias,
        typeUserModified: oldCol?.typeUserModified,
      };
    });

    this.attrs.selectedColumns = newSelectedColumns;
    this.context.onchange?.();
  }

  static deserializeState(
    state: ModifyColumnsNodeAttrs,
  ): ModifyColumnsNodeAttrs {
    return {
      selectedColumns: state.selectedColumns.map((c) => ({
        ...c,
        // Handle legacy string types (e.g. 'INT' → {kind: 'int'})
        type: legacyDeserializeType(
          c.type as unknown as PerfettoSqlType | string | undefined,
        ),
      })),
    };
  }

  validate(): boolean {
    if (this.context.issues) {
      this.context.issues.clear();
    }

    if (this.primaryInput === undefined) {
      this.setValidationError('No input node connected');
      return false;
    }

    if (!this.primaryInput.validate()) {
      return false;
    }

    const colNames = new Set<string>();
    for (const col of this.attrs.selectedColumns) {
      if (!col.checked) continue;
      const name = col.alias ? col.alias.trim() : col.name;
      if (colNames.has(name)) {
        this.setValidationError('Duplicate column names');
        return false;
      }
      colNames.add(name);
    }

    if (colNames.size === 0) {
      this.setValidationError(
        'No columns selected. Select at least one column.',
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
    return 'Modify Columns';
  }

  nodeDetails(): NodeDetailsAttrs {
    const selectedCols = this.attrs.selectedColumns.filter((c) => c.checked);
    const totalCols = this.attrs.selectedColumns.length;

    if (selectedCols.length === 0) {
      return {
        content: NodeDetailsMessage('All columns deselected'),
      };
    }

    const hasUnselected = this.attrs.selectedColumns.some((c) => !c.checked);
    const hasAlias = this.attrs.selectedColumns.some((c) => c.alias);
    if (!hasUnselected && !hasAlias) {
      return {
        content: NodeDetailsMessage('Select all columns'),
      };
    }

    const maxColumnsToShow = 5;
    if (selectedCols.length > maxColumnsToShow) {
      const renamedCols = selectedCols.filter((c) => c.alias);
      const allSelected = selectedCols.length === totalCols;

      if (renamedCols.length > 0 && renamedCols.length <= 3) {
        const renamedItems = renamedCols.map((c) =>
          m('div', ColumnName(c.name), ' AS ', ColumnName(c.alias!)),
        );
        if (allSelected) {
          return {
            content: m('div', ...renamedItems),
          };
        }
        const summaryText = `${selectedCols.length} of ${totalCols} columns selected`;
        return {
          content: m(
            'div',
            m('div', summaryText),
            NodeDetailsSpacer(),
            ...renamedItems,
          ),
        };
      } else {
        if (allSelected) {
          return {
            content: NodeDetailsMessage('Select all'),
          };
        }
        const summaryText = `${selectedCols.length} of ${totalCols} columns selected`;
        return {
          content: m('div', summaryText),
        };
      }
    }

    const selectedItems = selectedCols.map((c) => {
      if (c.alias) {
        return m('div', ColumnName(c.name), ' AS ', ColumnName(c.alias));
      } else {
        return m('div', ColumnName(c.name));
      }
    });
    return {
      content: m('div', ...selectedItems),
    };
  }

  nodeSpecificModify(): NodeModifyAttrs {
    const selectedCount = this.attrs.selectedColumns.filter(
      (col) => col.checked,
    ).length;
    const totalCount = this.attrs.selectedColumns.length;

    // Convert descriptors to ColumnInfo for the ColumnSelector widget.
    const columnsForWidget: ColumnInfo[] = this.attrs.selectedColumns.map(
      (d) => ({
        name: d.name,
        checked: d.checked,
        type: d.type,
        alias: d.alias,
        typeUserModified: d.typeUserModified,
      }),
    );

    const sections: NodeModifyAttrs['sections'] = [
      {
        title: `Select and Rename Columns (${selectedCount} / ${totalCount} selected)`,
        content: m(ColumnSelector, {
          columns: columnsForWidget,
          onColumnsChange: (columns) => {
            this.attrs.selectedColumns = columns.map((c) => ({
              name: c.name,
              type: c.type,
              checked: c.checked,
              alias: c.alias,
              typeUserModified: c.typeUserModified,
            }));
            this.context.onchange?.();
          },
          helpText:
            'Check columns to include, add aliases to rename, and drag to reorder',
          draggable: true,
          renderExtra: (col, index) => {
            const handleTypeChange = (
              idx: number,
              newType: PerfettoSqlType,
            ) => {
              const newSelectedColumns = [...this.attrs.selectedColumns];
              newSelectedColumns[idx] = {
                ...newSelectedColumns[idx],
                type: newType,
                typeUserModified: true,
              };
              this.attrs.selectedColumns = newSelectedColumns;
              this.context.onchange?.();
            };

            return [
              m(TextInput, {
                oninput: (e: Event) => {
                  const newSelectedColumns = [...this.attrs.selectedColumns];
                  const inputValue = (e.target as HTMLInputElement).value;
                  newSelectedColumns[index] = {
                    ...newSelectedColumns[index],
                    alias: inputValue.trim() === '' ? undefined : inputValue,
                  };
                  this.attrs.selectedColumns = newSelectedColumns;
                  this.context.onchange?.();
                },
                placeholder: 'alias',
                value: col.alias ? col.alias : '',
              }),
              renderTypeSelector(
                col,
                index,
                this.context.sqlModules,
                handleTypeChange,
              ),
            ];
          },
        }),
      },
    ];

    return {
      info: 'Select which columns to include in the output and optionally rename them using aliases. Check columns to include, add aliases to rename, and drag to reorder.',
      sections,
    };
  }

  nodeInfo(): m.Children {
    return loadNodeDoc('modify_columns');
  }

  clone(): QueryNode {
    return new ModifyColumnsNode(
      {selectedColumns: this.attrs.selectedColumns.map((c) => ({...c}))},
      this.context,
    );
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (this.primaryInput === undefined) return undefined;

    const hasModification = this.attrs.selectedColumns.some(
      (col) => !col.checked || (col.alias && col.alias.trim() !== ''),
    );

    if (!hasModification) {
      return StructuredQueryBuilder.passthrough(this.primaryInput, this.nodeId);
    }

    const columns: ColumnSpec[] = [];

    for (const col of this.attrs.selectedColumns) {
      if (!col.checked) continue;
      columns.push({
        columnNameOrExpression: col.name,
        alias: col.alias,
      });
    }

    return StructuredQueryBuilder.withSelectColumns(
      this.primaryInput,
      columns,
      undefined,
      this.nodeId,
    );
  }
}
