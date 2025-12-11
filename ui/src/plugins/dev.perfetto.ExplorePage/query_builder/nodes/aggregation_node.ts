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
import protos from '../../../../protos';
import {
  ColumnInfo,
  columnInfoFromName,
  columnInfoFromSqlColumn,
  newColumnInfoList,
} from '../column_info';
import {
  PerfettoSqlTypes,
  PerfettoSqlType,
} from '../../../../trace_processor/perfetto_sql_type';
import {NodeIssues} from '../node_issues';
import {
  StructuredQueryBuilder,
  AggregationSpec,
} from '../structured_query_builder';
import {isColumnValidForAggregation} from '../utils';
import {
  LabeledControl,
  InlineEditList,
  OutlinedField,
  OutlinedMultiSelect,
  MultiSelectOption,
  MultiSelectDiff,
} from '../widgets';
import {NodeModifyAttrs, NodeDetailsAttrs} from '../node_explorer_types';
import {loadNodeDoc} from '../node_doc_loader';
import {ColumnName, NodeDetailsSpacer} from '../node_styling_widgets';

export interface AggregationSerializedState {
  groupByColumns: {name: string; checked: boolean}[];
  aggregations: {
    column?: ColumnInfo;
    aggregationOp?: string;
    newColumnName?: string;
    percentile?: number;
    isValid?: boolean;
  }[];
  comment?: string;
}

export interface AggregationNodeState extends QueryNodeState {
  groupByColumns: ColumnInfo[];
  aggregations: Aggregation[];
}

export interface Aggregation {
  column?: ColumnInfo;
  aggregationOp?: string;
  newColumnName?: string;
  percentile?: number;
  isValid?: boolean;
}

export class AggregationNode implements QueryNode {
  readonly nodeId: string;
  readonly type = NodeType.kAggregation;
  primaryInput?: QueryNode;
  nextNodes: QueryNode[];
  readonly state: AggregationNodeState;

  get finalCols(): ColumnInfo[] {
    // When there's no primaryInput, aggregation doesn't make sense
    // Return empty array to indicate no output columns
    if (this.primaryInput === undefined) {
      return [];
    }
    const selected = this.state.groupByColumns.filter((c) => c.checked);
    // IMPORTANT: Only include VALID aggregations in output
    // This prevents incomplete/invalid aggregations from propagating downstream
    for (const agg of this.state.aggregations) {
      if (!validateAggregation(agg)) {
        continue; // Skip invalid aggregations
      }
      const resultType = getAggregationResultType(agg);
      const resultName = agg.newColumnName ?? placeholderNewColumnName(agg);
      selected.push(
        columnInfoFromSqlColumn({
          name: resultName,
          type: resultType,
        }),
      );
    }
    return newColumnInfoList(selected, true);
  }

  constructor(state: AggregationNodeState) {
    this.nodeId = nextNodeId();
    this.state = {
      ...state,
      groupByColumns: state.groupByColumns ?? [],
      aggregations: state.aggregations ?? [],
    };
    this.nextNodes = [];
  }

  onPrevNodesUpdated() {
    this.updateGroupByColumns();
  }

  updateGroupByColumns() {
    if (this.primaryInput === undefined) {
      return;
    }
    const newGroupByColumns = newColumnInfoList(
      this.primaryInput.finalCols ?? [],
      false,
    );
    for (const oldCol of this.state.groupByColumns) {
      if (oldCol.checked) {
        const newCol = newGroupByColumns.find((c) => c.name === oldCol.name);
        if (newCol) {
          newCol.checked = true;
        } else {
          const missingCol = columnInfoFromName(oldCol.name);
          missingCol.checked = true;
          newGroupByColumns.push(missingCol);
        }
      }
    }
    this.state.groupByColumns = newGroupByColumns;
  }

  validate(): boolean {
    // Clear any previous errors at the start of validation
    if (this.state.issues) {
      this.state.issues.clear();
    }

    if (this.primaryInput === undefined) {
      this.setValidationError('No input node connected');
      return false;
    }
    if (!this.primaryInput.validate()) {
      this.setValidationError('Previous node is invalid');
      return false;
    }
    const sourceColNames = new Set(
      (this.primaryInput.finalCols ?? []).map((c) => c.name),
    );
    const missingCols: string[] = [];
    for (const col of this.state.groupByColumns) {
      if (col.checked && !sourceColNames.has(col.name)) {
        missingCols.push(col.name);
      }
    }

    if (missingCols.length > 0) {
      this.setValidationError(
        `Group by columns ['${missingCols.join(', ')}'] not found in input`,
      );
      return false;
    }

    // Must have at least one of: group by columns OR aggregation functions
    const hasGroupBy = this.state.groupByColumns.find((c) => c.checked);

    // Validate aggregations first
    for (const agg of this.state.aggregations) {
      agg.isValid = validateAggregation(agg);
    }
    const hasAggregations =
      this.state.aggregations.filter((a) => a.isValid).length > 0;

    if (!hasGroupBy && !hasAggregations) {
      this.setValidationError(
        'Aggregation node requires at least one group by column or aggregation function',
      );
      return false;
    }

    // Check for duplicate column names
    const selectedGroupBy = this.state.groupByColumns.filter((c) => c.checked);
    const columnNames = new Set<string>();

    // Add group-by column names
    for (const col of selectedGroupBy) {
      columnNames.add(col.name);
    }

    // Check aggregation result column names for duplicates
    for (const agg of this.state.aggregations) {
      if (!agg.isValid) continue;
      const resultName = agg.newColumnName ?? placeholderNewColumnName(agg);
      if (columnNames.has(resultName)) {
        this.setValidationError(
          `Duplicate column name "${resultName}" - aggregation result conflicts with GROUP BY column or another aggregation`,
        );
        return false;
      }
      columnNames.add(resultName);
    }

    return true;
  }

  private setValidationError(message: string): void {
    if (!this.state.issues) {
      this.state.issues = new NodeIssues();
    }
    this.state.issues.queryError = new Error(message);
  }

  getTitle(): string {
    return 'Aggregation';
  }

  nodeDetails(): NodeDetailsAttrs {
    const selectedGroupBy = this.state.groupByColumns.filter((c) => c.checked);

    const details: m.Child[] = [];

    // Display group by columns
    if (selectedGroupBy.length > 0) {
      details.push(
        m(
          'div',
          'Group by: ',
          selectedGroupBy.map((c, index) => [
            ColumnName(c.name),
            index < selectedGroupBy.length - 1 ? ', ' : '',
          ]),
        ),
      );
    } else {
      details.push(m('div', 'Group by: None'));
    }

    const validAggregations = this.state.aggregations.filter(
      (agg) => agg.isValid,
    );

    // Add spacing before aggregations if there are any
    if (validAggregations.length > 0) {
      details.push(NodeDetailsSpacer());
    }

    // Show each aggregation on its own line with styled column names
    validAggregations.forEach((agg) => {
      const resultName = agg.newColumnName ?? placeholderNewColumnName(agg);

      if (isCountAll(agg)) {
        details.push(m('div', 'COUNT(*) AS ', ColumnName(resultName)));
      } else if (
        agg.aggregationOp === 'COUNT_DISTINCT' &&
        agg.column !== undefined
      ) {
        details.push(
          m(
            'div',
            'COUNT(DISTINCT ',
            ColumnName(agg.column.name),
            ') AS ',
            ColumnName(resultName),
          ),
        );
      } else if (
        agg.aggregationOp === 'PERCENTILE' &&
        agg.percentile !== undefined
      ) {
        details.push(
          m(
            'div',
            'PERCENTILE(',
            ColumnName(agg.column?.name ?? ''),
            `, ${agg.percentile}) AS `,
            ColumnName(resultName),
          ),
        );
      } else if (agg.column !== undefined) {
        details.push(
          m(
            'div',
            `${agg.aggregationOp}(`,
            ColumnName(agg.column.name),
            ') AS ',
            ColumnName(resultName),
          ),
        );
      }
    });

    return {
      content: m('.pf-aggregation-node-details', details),
    };
  }

  nodeSpecificModify(): NodeModifyAttrs {
    const sections: NodeModifyAttrs['sections'] = [];

    // Group by section
    sections.push({
      content: this.renderGroupBySection(),
    });

    // Aggregations list section with inline editing
    sections.push({
      content: this.renderAggregationsList(),
    });

    return {
      info: 'Groups rows by selected columns and computes aggregations (SUM, COUNT, AVG, etc.). Select columns to group by, then add aggregations to compute summary statistics.',
      sections,
    };
  }

  private renderGroupBySection(): m.Child {
    const groupByOptions: MultiSelectOption[] = this.state.groupByColumns.map(
      (col) => ({
        id: col.name,
        name: col.name,
        checked: col.checked,
      }),
    );

    const selectedGroupBy = this.state.groupByColumns.filter((c) => c.checked);
    const label =
      selectedGroupBy.length > 0
        ? selectedGroupBy.map((c) => c.name).join(', ')
        : 'None';

    return m(
      LabeledControl,
      {label: 'Grouping columns:'},
      m(OutlinedMultiSelect, {
        label,
        options: groupByOptions,
        showNumSelected: false,
        onChange: (diffs: MultiSelectDiff[]) => {
          for (const diff of diffs) {
            const column = this.state.groupByColumns.find(
              (c) => c.name === diff.id,
            );
            if (column) {
              column.checked = diff.checked;
            }
          }
          this.state.onchange?.();
        },
      }),
    );
  }

  private renderAggregationsList(): m.Child {
    return m(InlineEditList<Aggregation>, {
      items: this.state.aggregations,
      validate: validateAggregation,
      renderControls: (agg, _index, onUpdate) =>
        this.renderAggregationFormControls(agg, onUpdate),
      onUpdate: (aggregations) => {
        this.state.aggregations = aggregations;
        this.state.onchange?.();
      },
      onValidChange: () => {
        // Also trigger when validation state changes (invalid -> valid)
        this.state.onchange?.();
      },
      addButtonLabel: 'Add aggregation',
      addButtonIcon: 'add',
      emptyItem: () => ({
        aggregationOp: 'COUNT(*)',
        // Don't pre-fill newColumnName - let the placeholder show
        newColumnName: undefined,
      }),
    });
  }

  private renderAggregationFormControls(
    agg: Aggregation,
    onUpdate: (updated: Aggregation) => void,
  ): m.Children {
    const columnOptions = this.state.groupByColumns.map((col) => {
      const isValid = isColumnValidForAggregation(col, agg.aggregationOp);
      return m(
        'option',
        {
          value: col.name,
          selected: agg.column?.name === col.name,
          disabled: !isValid,
        },
        col.name,
      );
    });

    const needsColumn =
      agg.aggregationOp !== undefined && agg.aggregationOp !== 'COUNT(*)';
    const needsPercentile = agg.aggregationOp === 'PERCENTILE';

    // Generate smart placeholder for column name
    let columnNamePlaceholder = 'result_column';
    if (agg.aggregationOp) {
      const opLower = agg.aggregationOp.toLowerCase();
      if (agg.column?.name) {
        columnNamePlaceholder = `${opLower}_${agg.column.name}`;
      } else if (agg.aggregationOp === 'COUNT(*)') {
        columnNamePlaceholder = 'count';
      } else {
        columnNamePlaceholder = opLower;
      }
    }

    return [
      // Operation selector
      m(
        OutlinedField,
        {
          label: 'Operation',
          value: agg.aggregationOp ?? '',
          onchange: (e: Event) => {
            const value = (e.target as HTMLSelectElement).value;
            const updated: Aggregation = {
              ...agg,
              aggregationOp: value,
            };
            // Clear column if switching to COUNT(*)
            if (value === 'COUNT(*)') {
              updated.column = undefined;
            }
            // Clear percentile if not PERCENTILE
            if (value !== 'PERCENTILE') {
              updated.percentile = undefined;
            }
            onUpdate(updated);
          },
        },
        [
          m('option', {value: '', disabled: true}, 'Select operation...'),
          ...AGGREGATION_OPS.map((op) => m('option', {value: op}, op)),
        ],
      ),
      // Column selector (conditionally shown)
      needsColumn === true
        ? m(
            OutlinedField,
            {
              label: 'Column',
              value: agg.column?.name ?? '',
              onchange: (e: Event) => {
                const value = (e.target as HTMLSelectElement).value;
                const column = this.state.groupByColumns.find(
                  (c) => c.name === value,
                );
                onUpdate({
                  ...agg,
                  column,
                });
              },
            },
            [
              m('option', {value: '', disabled: true}, 'Select column...'),
              ...columnOptions,
            ],
          )
        : undefined,
      // Percentile input (conditionally shown)
      needsPercentile === true
        ? m(OutlinedField, {
            label: 'Percentile',
            value: agg.percentile?.toString() ?? '',
            placeholder: 'e.g. 50, 95, 99',
            oninput: (e: Event) => {
              const value = parseFloat((e.target as HTMLInputElement).value);
              onUpdate({
                ...agg,
                percentile: isNaN(value) ? undefined : value,
              });
            },
          })
        : undefined,
      // New column name input (always shown)
      m(OutlinedField, {
        label: 'New column name',
        value: agg.newColumnName ?? '',
        placeholder: columnNamePlaceholder,
        oninput: (e: Event) => {
          const value = (e.target as HTMLInputElement).value.trim();
          onUpdate({
            ...agg,
            newColumnName: value || undefined,
          });
        },
      }),
    ];
  }

  nodeInfo(): m.Children {
    return loadNodeDoc('aggregation');
  }

  clone(): QueryNode {
    const stateCopy: AggregationNodeState = {
      groupByColumns: newColumnInfoList(this.state.groupByColumns),
      aggregations: this.state.aggregations.map((a) => ({...a})),
      onchange: this.state.onchange,
      issues: this.state.issues,
    };
    return new AggregationNode(stateCopy);
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (!this.validate()) return;

    // Defensive check: primaryInput must exist for aggregation to work
    if (this.primaryInput === undefined) return undefined;

    // Prepare groupByColumns
    const groupByColumns = this.state.groupByColumns
      .filter((c) => c.checked)
      .map((c) => c.column.name);

    // Prepare aggregations
    const aggregations: AggregationSpec[] = [];
    for (const agg of this.state.aggregations) {
      agg.isValid = validateAggregation(agg);
      if (agg.isValid && agg.aggregationOp) {
        // Map COUNT(*) to COUNT for the proto (COUNT(*) is UI-only)
        const protoOp = isCountAll(agg) ? 'COUNT' : agg.aggregationOp;

        const aggSpec: AggregationSpec = {
          columnName: agg.column?.column.name, // Optional for COUNT(*)
          op: protoOp,
          resultColumnName: agg.newColumnName ?? placeholderNewColumnName(agg),
        };

        // Add percentile if specified
        if (agg.percentile !== undefined) {
          aggSpec.percentile = agg.percentile;
        }

        aggregations.push(aggSpec);
      }
    }

    // Only use GROUP BY if we have group by columns
    // Otherwise, aggregations apply to the entire result set
    const sq =
      groupByColumns.length > 0
        ? StructuredQueryBuilder.withGroupBy(
            this.primaryInput,
            groupByColumns,
            aggregations,
            this.nodeId,
          )
        : StructuredQueryBuilder.withGroupBy(
            this.primaryInput,
            [], // Empty group by columns means aggregate entire result set
            aggregations,
            this.nodeId,
          );
    if (sq === undefined) return undefined;

    // For aggregation, we must always set select_columns to match GROUP BY + aggregates
    // Clear any previous select_columns and set to the correct aggregation output
    sq.selectColumns = [];

    // Add GROUP BY columns (if any)
    for (const colName of groupByColumns) {
      const selectCol = new protos.PerfettoSqlStructuredQuery.SelectColumn();
      selectCol.columnName = colName;
      sq.selectColumns.push(selectCol);
    }

    // Add aggregate result columns
    for (const agg of aggregations) {
      const selectCol = new protos.PerfettoSqlStructuredQuery.SelectColumn();
      selectCol.columnName = agg.resultColumnName!;
      sq.selectColumns.push(selectCol);
    }

    return sq;
  }

  resolveColumns() {
    if (this.primaryInput === undefined) {
      return;
    }
    const sourceCols = this.primaryInput.finalCols ?? [];
    this.state.groupByColumns.forEach((c) => {
      const sourceCol = sourceCols.find((s) => s.name === c.name);
      if (sourceCol) {
        c.column = sourceCol.column;
      }
    });
    this.state.aggregations.forEach((a) => {
      if (a.column) {
        const sourceCol = sourceCols.find((s) => s.name === a.column?.name);
        if (sourceCol) {
          a.column = sourceCol;
        }
      }
    });
  }

  serializeState(): AggregationSerializedState & {primaryInputId?: string} {
    return {
      primaryInputId: this.primaryInput?.nodeId,
      groupByColumns: this.state.groupByColumns.map((c) => ({
        name: c.name,
        checked: c.checked,
      })),
      aggregations: this.state.aggregations.map((a) => ({
        column: a.column,
        aggregationOp: a.aggregationOp,
        newColumnName: a.newColumnName,
        percentile: a.percentile,
        isValid: a.isValid,
      })),
    };
  }

  static deserializeState(
    state: AggregationSerializedState,
  ): AggregationNodeState {
    const groupByColumns = state.groupByColumns.map((c) => {
      const col = columnInfoFromName(c.name);
      col.checked = c.checked;
      return col;
    });
    const aggregations = state.aggregations.map((a) => {
      // Migrate old COUNT_ALL to COUNT(*) for backward compatibility
      const aggregationOp =
        a.aggregationOp === 'COUNT_ALL' ? 'COUNT(*)' : a.aggregationOp;
      return {
        column: a.column,
        aggregationOp,
        newColumnName: a.newColumnName,
        percentile: a.percentile,
        isValid: a.isValid,
      };
    });
    return {
      ...state,
      groupByColumns,
      aggregations,
    };
  }
}

export function createGroupByProto(
  groupByColumns: ColumnInfo[],
  aggregations: Aggregation[],
): protos.PerfettoSqlStructuredQuery.GroupBy | undefined {
  // Allow group by with empty column names (aggregates entire result set)
  const groupByProto = new protos.PerfettoSqlStructuredQuery.GroupBy();
  groupByProto.columnNames = groupByColumns
    .filter((c) => c.checked)
    .map((c) => c.column.name);

  for (const agg of aggregations) {
    agg.isValid = validateAggregation(agg);
  }
  groupByProto.aggregates = aggregations
    .filter((agg) => agg.isValid)
    .map(GroupByAggregationAttrsToProto);

  // Only return undefined if we have no aggregates at all
  if (groupByProto.aggregates.length === 0) return undefined;

  return groupByProto;
}

function validateAggregation(aggregation: Aggregation): boolean {
  if (!aggregation.aggregationOp) return false;

  // COUNT(*) doesn't need a column
  if (isCountAll(aggregation)) {
    return true;
  }

  // All other operations require a column
  if (!aggregation.column) return false;

  // Check column type compatibility using utility function
  if (
    !isColumnValidForAggregation(aggregation.column, aggregation.aggregationOp)
  ) {
    return false;
  }

  // PERCENTILE has additional validation requirements
  if (aggregation.aggregationOp === 'PERCENTILE') {
    if (
      aggregation.percentile === undefined ||
      aggregation.percentile < 0 ||
      aggregation.percentile > 100
    ) {
      return false;
    }
  }

  return true;
}

export function GroupByAggregationAttrsToProto(
  agg: Aggregation,
): protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate {
  const newAgg = new protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate();

  // COUNT(*) doesn't have a column; all other operations do
  if (agg.column) {
    newAgg.columnName = agg.column.column.name;
  }

  newAgg.op = stringToAggregateOp(agg.aggregationOp!);
  newAgg.resultColumnName = agg.newColumnName ?? placeholderNewColumnName(agg);

  // PERCENTILE requires percentile value
  if (agg.aggregationOp === 'PERCENTILE' && agg.percentile !== undefined) {
    newAgg.percentile = agg.percentile;
  }

  return newAgg;
}

export function placeholderNewColumnName(agg: Aggregation) {
  // COUNT(*) doesn't have a column
  if (isCountAll(agg)) {
    return 'count';
  }

  if (agg.column && agg.aggregationOp) {
    // Use operation_column format (e.g., "sum_value") to match UI placeholder
    return `${agg.aggregationOp.toLowerCase()}_${agg.column.name}`;
  }

  // Fallback for incomplete aggregations
  return agg.aggregationOp?.toLowerCase() ?? 'result';
}

function stringToAggregateOp(
  s: string,
): protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op {
  // COUNT(*) maps to COUNT in the proto (without a column)
  if (s === 'COUNT(*)') {
    return protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op.COUNT;
  }

  // Only check ops that exist in the proto (exclude COUNT(*))
  const validProtoOps: readonly string[] = AGGREGATION_OPS.filter(
    (op) => op !== 'COUNT(*)',
  );
  if (validProtoOps.includes(s)) {
    return protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op[
      s as keyof typeof protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op
    ];
  }
  throw new Error(`Invalid AggregateOp '${s}'`);
}

// Helper function to determine the result type of an aggregation operation
function getAggregationResultType(agg: Aggregation): PerfettoSqlType {
  if (!agg.aggregationOp) {
    return PerfettoSqlTypes.INT; // Default fallback
  }

  switch (agg.aggregationOp) {
    case 'COUNT':
    case 'COUNT(*)':
    case 'COUNT_DISTINCT':
      return PerfettoSqlTypes.INT;

    case 'SUM':
    case 'MIN':
    case 'MAX':
      // Preserve the input column type for SUM, MIN, MAX
      if (!agg.column?.column.type) {
        console.warn(
          `${agg.aggregationOp} aggregation missing column type information, defaulting to INT`,
        );
      }
      return agg.column?.column.type ?? PerfettoSqlTypes.INT;

    case 'MEAN':
    case 'MEDIAN':
    case 'DURATION_WEIGHTED_MEAN':
    case 'PERCENTILE':
      // These operations always return DOUBLE
      return PerfettoSqlTypes.DOUBLE;

    default:
      return PerfettoSqlTypes.INT; // Default fallback
  }
}

// Helper to check if an aggregation is COUNT(*)
function isCountAll(agg: Aggregation): boolean {
  return agg.aggregationOp === 'COUNT(*)';
}

const AGGREGATION_OPS = [
  'COUNT',
  'COUNT(*)',
  'COUNT_DISTINCT',
  'SUM',
  'MIN',
  'MAX',
  'MEAN',
  'MEDIAN',
  'DURATION_WEIGHTED_MEAN',
  'PERCENTILE',
] as const;
