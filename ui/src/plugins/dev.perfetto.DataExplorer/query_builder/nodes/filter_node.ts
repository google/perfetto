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
import {ColumnInfo} from '../column_info';
import protos from '../../../../protos';
import {
  UIFilter,
  createExperimentalFiltersProto,
  formatFilterDetails,
  isFilterDefinitionValid,
  ALL_FILTER_OPS,
  isValueRequired,
  parseFilterValue,
} from '../operations/filter';
import {StructuredQueryBuilder} from '../structured_query_builder';
import {NodeIssues} from '../node_issues';
import {showModal} from '../../../../widgets/modal';
import {Editor} from '../../../../widgets/editor';
import {ListItem, OutlinedField, InlineEditList} from '../widgets';
import {EmptyState} from '../../../../widgets/empty_state';
import {NodeModifyAttrs, NodeDetailsAttrs} from '../../node_types';
import {Button, ButtonVariant} from '../../../../widgets/button';
import {NodeDetailsMessage} from '../node_styling_widgets';
import {Icons} from '../../../../base/semantic_icons';
import {loadNodeDoc} from '../node_doc_loader';
import {SqlValue} from '../../../../trace_processor/query_result';

// Maximum length for truncated SQL display
const SQL_TRUNCATE_LENGTH = 50;

// Serializable node configuration.
export interface FilterNodeAttrs {
  filterMode?: 'structured' | 'freeform';
  sqlExpression?: string;
  filters?: Partial<UIFilter>[];
  filterOperator?: 'AND' | 'OR';
}

export class FilterNode implements QueryNode {
  readonly nodeId: string;
  readonly type = NodeType.kFilter;
  primaryInput?: QueryNode;
  secondaryInputs?: undefined; // FilterNode doesn't support secondary inputs
  nextNodes: QueryNode[];
  readonly attrs: FilterNodeAttrs;
  readonly context: NodeContext;

  constructor(attrs: FilterNodeAttrs, context: NodeContext) {
    this.nodeId = nextNodeId();
    // Migrate filter values from strings back to numbers when appropriate.
    // This is needed because JSON serialization converts BigInt to string,
    // and we want numeric values to be stored as numbers for proper filtering.
    const filters = attrs.filters?.map((f): Partial<UIFilter> => {
      if ('value' in f && typeof f.value === 'string') {
        if (!Array.isArray(f.value)) {
          const parsed = parseFilterValue(f.value);
          if (parsed !== undefined && parsed !== f.value) {
            return {...f, value: parsed} as Partial<UIFilter>;
          }
        }
      }
      return f;
    });
    this.attrs = {
      ...attrs,
      filters: filters ?? [],
    };
    this.context = context;
    this.nextNodes = [];
  }

  get sourceCols(): ColumnInfo[] {
    return this.primaryInput?.finalCols ?? [];
  }

  get finalCols(): ColumnInfo[] {
    return this.sourceCols;
  }

  /**
   * Check if a filter is valid for this node.
   * A filter is valid if:
   * 1. Its definition is structurally valid (has column, op, value etc)
   * 2. The column it references actually exists in sourceCols
   */
  private isFilterValid(filter: Partial<UIFilter>): filter is UIFilter {
    // First check if the filter structure is valid
    if (!isFilterDefinitionValid(filter)) {
      return false;
    }

    // Then check if the column exists in sourceCols
    const columnExists = this.sourceCols.some(
      (col) => col.name === filter.column,
    );
    return columnExists;
  }

  getTitle(): string {
    return 'Filter';
  }

  private setValidationError(message: string): void {
    if (!this.context.issues) {
      this.context.issues = new NodeIssues();
    }
    this.context.issues.queryError = new Error(message);
  }

  nodeDetails(): NodeDetailsAttrs {
    this.validate();

    const mode = this.attrs.filterMode ?? 'structured';

    // Freeform SQL mode
    if (mode === 'freeform') {
      const sql = this.attrs.sqlExpression?.trim();
      if (!sql) {
        return {
          content: NodeDetailsMessage('No filter clause'),
        };
      }

      if (sql.length < 200) {
        return {
          content: m('code', sql),
        };
      } else {
        return {
          content: NodeDetailsMessage('Filter clause applied'),
        };
      }
    }

    // Structured mode - only show valid filters in nodeDetails
    const validFilters =
      this.attrs.filters?.filter((f) => this.isFilterValid(f)) ?? [];

    if (validFilters.length === 0) {
      return {
        content: NodeDetailsMessage('No filters applied'),
      };
    }

    return {
      content: formatFilterDetails(
        validFilters,
        this.attrs.filterOperator,
        {filters: this.attrs.filters, onchange: this.context.onchange},
        undefined, // onRemove - handled internally by formatFilterDetails
        true, // compact mode for smaller font
        undefined, // No edit callback - editing happens in nodeSpecificModify
      ),
    };
  }

  nodeSpecificModify(): NodeModifyAttrs {
    this.validate();

    const mode = this.attrs.filterMode ?? 'structured';
    const filters = this.attrs.filters ?? [];
    const operator = this.attrs.filterOperator ?? 'AND';

    // Set autoExecute based on mode
    this.context.autoExecute = mode === 'structured';

    // Build bottom buttons
    const bottomLeftButtons: NodeModifyAttrs['bottomLeftButtons'] = [];
    const bottomRightButtons: NodeModifyAttrs['bottomRightButtons'] = [];

    // Show AND/OR switch only when there are 2+ filters in structured mode
    if (mode === 'structured' && filters.length >= 2) {
      bottomLeftButtons.push({
        label: operator === 'OR' ? 'OR' : 'AND',
        icon: operator === 'OR' ? 'alt_route' : 'join',
        onclick: () => {
          this.attrs.filterOperator = operator === 'OR' ? 'AND' : 'OR';
          this.context.onchange?.();
        },
      });
    }

    // Mode switch button
    bottomRightButtons.push({
      label:
        mode === 'structured' ? 'Switch to WHERE clause' : 'Switch to filters',
      icon: mode === 'structured' ? 'code' : Icons.Filter,
      onclick: () => this.handleModeSwitch(mode),
      compact: true,
    });

    // Build sections
    const sections: NodeModifyAttrs['sections'] = [];

    // Input section with buttons/inputs - only for freeform mode
    if (mode === 'freeform') {
      sections.push({
        content: m(Button, {
          label: 'Edit WHERE clause',
          icon: 'edit',
          variant: ButtonVariant.Outlined,
          onclick: () => this.showSqlExpressionModal(),
        }),
      });
    }

    // Filters list section
    sections.push({
      content: this.renderFiltersList(),
    });

    // Info text explaining nested filters (only shown in structured mode)
    const info =
      mode === 'structured'
        ? 'To combine AND and OR logic (nested filters), use multiple filter nodes. Each filter node can use either AND or OR to combine its conditions.'
        : 'Use a custom WHERE clause to filter rows. This mode allows for complex SQL expressions that structured filters cannot express.';

    return {
      info,
      bottomLeftButtons,
      bottomRightButtons,
      sections,
    };
  }

  private renderFiltersList(): m.Child {
    const mode = this.attrs.filterMode ?? 'structured';
    const hasSqlExpression =
      this.attrs.sqlExpression !== undefined &&
      this.attrs.sqlExpression.trim() !== '';

    if (mode === 'freeform') {
      if (!hasSqlExpression) {
        return m(EmptyState, {
          title: 'No WHERE clause defined.',
        });
      }
      // Show SQL expression as a single list item
      return m(
        '.pf-filters-list',
        m(ListItem, {
          icon: 'code',
          name: 'WHERE clause',
          description: this.truncateSql(this.attrs.sqlExpression ?? ''),
          actions: [
            {
              label: 'Edit',
              icon: 'edit',
              onclick: () => this.showSqlExpressionModal(),
            },
          ],
          onRemove: () => {
            this.attrs.sqlExpression = '';
            this.attrs.filterMode = 'structured';
            this.context.onchange?.();
          },
        }),
      );
    }

    // Structured mode - use InlineEditList widget
    return m(InlineEditList<Partial<UIFilter>>, {
      items: this.attrs.filters ?? [],
      validate: (filter) => this.isFilterValid(filter),
      renderControls: (filter, _index, onUpdate) =>
        this.renderFilterFormControls(filter, onUpdate),
      onUpdate: (filters) => {
        this.attrs.filters = filters;
      },
      onValidChange: () => {
        this.context.onchange?.();
      },
      addButtonLabel: 'Add filter',
      addButtonIcon: 'add',
      emptyItem: () => ({enabled: true}),
    });
  }

  private renderFilterFormControls(
    filter: Partial<UIFilter>,
    onUpdate: (updated: Partial<UIFilter>) => void,
  ): m.Children {
    const opObject = ALL_FILTER_OPS.find((o) => o.displayName === filter.op);
    const valueRequired = isValueRequired(opObject);

    return [
      // Column selector with outlined style
      m(
        OutlinedField,
        {
          label: 'Column',
          value: filter.column ?? '',
          onchange: (e: Event) => {
            const target = e.target as HTMLSelectElement;
            onUpdate({...filter, column: target.value});
          },
        },
        [
          m('option', {value: '', disabled: true}, 'Select column...'),
          ...this.sourceCols.map((col) =>
            m('option', {value: col.name}, col.name),
          ),
        ],
      ),
      // Operator selector with outlined style
      m(
        OutlinedField,
        {
          label: 'Operator',
          value: opObject?.key ?? '',
          onchange: (e: Event) => {
            const target = e.target as HTMLSelectElement;
            const newOp = ALL_FILTER_OPS.find((op) => op.key === target.value);
            if (newOp) {
              const updated: Partial<UIFilter> = {
                column: filter.column,
                op: newOp.displayName as UIFilter['op'],
                enabled: filter.enabled,
              };
              // Add value if required
              if (isValueRequired(newOp) && 'value' in filter) {
                // Extract single value (handle both single values and arrays)
                const existingValue = filter.value;
                const singleValue = Array.isArray(existingValue)
                  ? existingValue[0]
                  : existingValue;
                if (singleValue !== undefined) {
                  (updated as {value: SqlValue}).value = singleValue;
                }
              }
              onUpdate(updated);
            }
          },
        },
        [
          m('option', {value: '', disabled: true}, 'Select operator...'),
          ...ALL_FILTER_OPS.map((op) =>
            m('option', {value: op.key}, op.displayName),
          ),
        ],
      ),
      // Value input with outlined style (always show, disabled when not required)
      m(OutlinedField, {
        label: 'Value',
        value: 'value' in filter ? String(filter.value) : '',
        disabled: !valueRequired,
        placeholder: 'Enter value...',
        oninput: (e: Event) => {
          const target = e.target as HTMLInputElement;
          const parsed = parseFilterValue(target.value);
          onUpdate({
            ...filter,
            value: parsed ?? target.value,
          } as Partial<UIFilter>);
        },
      }),
    ];
  }

  private handleModeSwitch(currentMode: 'structured' | 'freeform'): void {
    if (currentMode === 'structured') {
      // Switching to freeform: if no SQL expression yet, open modal first
      const hasExpression =
        this.attrs.sqlExpression !== undefined &&
        this.attrs.sqlExpression.trim() !== '';
      if (hasExpression) {
        this.attrs.filterMode = 'freeform';
        this.context.onchange?.();
      } else {
        this.showSqlExpressionModal();
      }
    } else {
      // Switching to structured
      this.attrs.filterMode = 'structured';
      this.context.onchange?.();
    }
  }

  private showSqlExpressionModal(): void {
    let tempExpression = this.attrs.sqlExpression ?? '';

    showModal({
      title: 'SQL Filter Expression',
      key: 'sql-expression-modal',
      vAlign: 'TOP',
      content: () =>
        m(
          '.pf-filter-sql-editor',
          m(Editor, {
            text: tempExpression,
            onUpdate: (text: string) => {
              tempExpression = text;
            },
          }),
        ),
      buttons: [
        {
          text: 'Cancel',
          action: () => {},
        },
        {
          text: 'Apply',
          primary: true,
          action: () => {
            this.attrs.sqlExpression = tempExpression;
            if (tempExpression.trim() !== '') {
              this.attrs.filterMode = 'freeform';
            }
            this.context.onchange?.();
          },
        },
      ],
    });
  }

  private truncateSql(sql: string): string {
    const trimmed = sql.trim();
    if (trimmed.length <= SQL_TRUNCATE_LENGTH) {
      return trimmed;
    }
    return trimmed.substring(0, SQL_TRUNCATE_LENGTH - 1) + '\u2026';
  }

  nodeInfo(): m.Children {
    return loadNodeDoc('filter');
  }

  validate(): boolean {
    // Clear any previous errors at the start of validation
    if (this.context.issues) {
      this.context.issues.clear();
    }

    if (this.primaryInput === undefined) {
      this.setValidationError('No input node connected');
      return false;
    }

    if (!this.primaryInput.validate()) {
      this.setValidationError('Previous node is invalid');
      return false;
    }

    // Check if there are columns available from the previous node
    if (this.sourceCols.length === 0) {
      this.setValidationError(
        'No columns available. Please select a table or add columns before filtering.',
      );
      return false;
    }

    return true;
  }

  clone(): QueryNode {
    return new FilterNode(
      {
        filterMode: this.attrs.filterMode,
        sqlExpression: this.attrs.sqlExpression,
        filters: this.attrs.filters?.map((f) => ({...f})),
        filterOperator: this.attrs.filterOperator,
      },
      this.context,
    );
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (this.primaryInput === undefined) return undefined;

    const mode = this.attrs.filterMode ?? 'structured';

    if (mode === 'freeform') {
      // Use SQL expression for freeform filtering
      if (!this.attrs.sqlExpression || this.attrs.sqlExpression.trim() === '') {
        // No filter expression - return passthrough to maintain reference chain
        return StructuredQueryBuilder.passthrough(
          this.primaryInput,
          this.nodeId,
        );
      }

      // Create a filter group with just the SQL expression
      const filterGroup =
        new protos.PerfettoSqlStructuredQuery.ExperimentalFilterGroup({
          op: protos.PerfettoSqlStructuredQuery.ExperimentalFilterGroup.Operator
            .AND,
          sqlExpressions: [this.attrs.sqlExpression],
        });

      return StructuredQueryBuilder.withFilter(
        this.primaryInput,
        filterGroup,
        this.nodeId,
      );
    }

    // Structured mode - only use valid filters for query building
    const validFilters =
      this.attrs.filters?.filter((f) => this.isFilterValid(f)) ?? [];

    if (validFilters.length === 0) {
      // No valid filters - return passthrough to maintain reference chain
      return StructuredQueryBuilder.passthrough(this.primaryInput, this.nodeId);
    }

    const filtersProto = createExperimentalFiltersProto(
      validFilters,
      this.sourceCols,
      this.attrs.filterOperator,
    );

    if (filtersProto === undefined) {
      // No valid filters proto - return passthrough to maintain reference chain
      return StructuredQueryBuilder.passthrough(this.primaryInput, this.nodeId);
    }

    return StructuredQueryBuilder.withFilter(
      this.primaryInput,
      filtersProto,
      this.nodeId,
    );
  }
}
