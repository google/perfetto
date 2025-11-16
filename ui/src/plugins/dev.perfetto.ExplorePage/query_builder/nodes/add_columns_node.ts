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

import {
  QueryNode,
  QueryNodeState,
  nextNodeId,
  NodeType,
  ModificationNode,
} from '../../query_node';
import {ColumnInfo, columnInfoFromName} from '../column_info';
import protos from '../../../../protos';
import m from 'mithril';
import {Card, CardStack} from '../../../../widgets/card';
import {MultiselectInput} from '../../../../widgets/multiselect_input';
import {Select} from '../../../../widgets/select';
import {Button} from '../../../../widgets/button';
import {TabStrip, TabOption} from '../../../../widgets/tabs';
import {TextInput} from '../../../../widgets/text_input';
import {
  StructuredQueryBuilder,
  ColumnSpec,
  JoinCondition,
} from '../structured_query_builder';
import {setValidationError} from '../node_issues';

export type AddColumnsMode = 'guided' | 'free';

export interface AddColumnsNodeState extends QueryNodeState {
  prevNode: QueryNode;
  selectedColumns?: string[];
  leftColumn?: string;
  rightColumn?: string;
  mode?: AddColumnsMode; // 'guided' or 'free' mode
  // Note: sqlTable is no longer used - we get columns from the connected node

  // Note: onAddAndConnectTable callback is now provided through
  // QueryNodeState.actions.onAddAndConnectTable

  // Pre-selected columns for each suggested table (before connecting)
  suggestionSelections?: Map<string, string[]>;

  // Track which suggestions are expanded to show column selection
  expandedSuggestions?: Set<string>;

  // Map from column name to its alias (for renaming added columns)
  columnAliases?: Map<string, string>;

  // Track if connection was made through guided suggestion
  isGuidedConnection?: boolean;
}

export class AddColumnsNode implements ModificationNode {
  readonly nodeId: string;
  readonly type = NodeType.kAddColumns;
  readonly prevNode: QueryNode;
  inputNodes?: (QueryNode | undefined)[];
  nextNodes: QueryNode[];
  readonly state: AddColumnsNodeState;

  constructor(state: AddColumnsNodeState) {
    this.nodeId = nextNodeId();
    this.state = state;
    this.prevNode = state.prevNode;
    this.inputNodes = [];
    this.nextNodes = [];
    this.state.selectedColumns = this.state.selectedColumns ?? [];
    this.state.leftColumn = this.state.leftColumn ?? 'id';
    this.state.rightColumn = this.state.rightColumn ?? 'id';
    this.state.autoExecute = this.state.autoExecute ?? false;
    this.state.mode = this.state.mode ?? 'guided';
    this.state.suggestionSelections =
      this.state.suggestionSelections ?? new Map();
    this.state.expandedSuggestions =
      this.state.expandedSuggestions ?? new Set();
    this.state.columnAliases = this.state.columnAliases ?? new Map();
  }

  // Called when a node is connected/disconnected to inputNodes
  onPrevNodesUpdated(): void {
    // Reset column selection when the right node changes
    this.state.selectedColumns = [];

    // When a node is connected, always switch to Guided mode
    if (this.rightNode) {
      this.state.mode = 'guided';
    }

    // If node is disconnected, reset the guided connection flag
    if (!this.rightNode) {
      this.state.isGuidedConnection = false;
    }

    this.state.onchange?.();
  }

  get sourceCols(): ColumnInfo[] {
    return this.prevNode?.finalCols ?? [];
  }

  // Get the node connected to the left-side input port (for adding columns from)
  get rightNode(): QueryNode | undefined {
    return this.inputNodes?.[0];
  }

  get rightCols(): ColumnInfo[] {
    return this.rightNode?.finalCols ?? [];
  }

  get finalCols(): ColumnInfo[] {
    if (this.rightNode) {
      // In free mode, add ALL columns from the connected node
      if (this.state.mode === 'free') {
        return [...this.sourceCols, ...this.rightCols];
      }
      // In guided mode, add only selected columns (with aliases if provided)
      const newCols =
        this.state.selectedColumns?.map((c) => {
          const alias = this.state.columnAliases?.get(c);
          // If an alias is provided, use it as the column name
          return columnInfoFromName(alias ?? c);
        }) ?? [];
      return [...this.sourceCols, ...newCols];
    }
    return this.sourceCols;
  }

  // Suggest joinable tables based on JOINID column types
  getJoinSuggestions(): Array<{
    colName: string;
    suggestedTable: string;
    targetColumn: string;
  }> {
    const suggestions: Array<{
      colName: string;
      suggestedTable: string;
      targetColumn: string;
    }> = [];

    for (const col of this.sourceCols) {
      const colType = col.column.type;

      // Check if this column has a JOINID type with explicit source information
      if (colType && colType.kind === 'joinid') {
        suggestions.push({
          colName: col.column.name,
          suggestedTable: colType.source.table,
          targetColumn: colType.source.column,
        });
      }
    }

    return suggestions;
  }

  // Get available columns for a suggested table
  getTableColumns(tableName: string): string[] {
    if (!this.state.sqlModules) return [];

    const table = this.state.sqlModules
      .listTables()
      .find((t) => t.name === tableName);
    if (!table) return [];

    return table.columns.map((c) => c.name);
  }

  getTitle(): string {
    return 'Add Columns';
  }

  nodeDetails(): m.Child {
    const details: m.Child[] = [];

    if (this.rightNode) {
      if (this.state.mode === 'free') {
        // Free mode: show that all columns are being added
        const numCols = this.rightCols.length;
        const plural = numCols > 1 ? 's' : '';
        details.push(
          m(
            'div',
            `Adding all ${numCols} column${plural} using `,
            m('strong', 'id = id'),
          ),
        );
      } else {
        // Guided mode: show selected columns and join condition
        if (
          this.state.selectedColumns &&
          this.state.selectedColumns.length > 0
        ) {
          const plural = this.state.selectedColumns.length > 1 ? 's' : '';
          const joinCondition =
            this.state.leftColumn && this.state.rightColumn
              ? `${this.state.leftColumn} = ${this.state.rightColumn}`
              : 'no join condition';
          // Show column names with aliases if provided
          const columnDisplay = this.state.selectedColumns
            .map((col) => {
              const alias = this.state.columnAliases?.get(col);
              return alias ? `${col} as ${alias}` : col;
            })
            .join(', ');
          details.push(
            m(
              'div',
              `Add column${plural} `,
              m('strong', columnDisplay),
              ' using ',
              m('strong', joinCondition),
            ),
          );
        } else {
          details.push(m('div', `No columns selected`));
        }
      }
    } else {
      details.push(m('div', 'Connect a node to add columns from'));
    }

    return m('.pf-aggregation-node-details', details);
  }

  nodeSpecificModify(): m.Child {
    // If a node is connected, always show Guided mode (no tabs)
    if (this.rightNode) {
      return m('div', [this.renderGuidedMode()]);
    }

    // If no node is connected, show tabs to choose between Guided and Free
    const currentMode = this.state.mode ?? 'guided';
    const tabs: TabOption[] = [
      {key: 'guided', title: 'Guided'},
      {key: 'free', title: 'Free'},
    ];

    return m('div', [
      m(TabStrip, {
        tabs,
        currentTabKey: currentMode,
        onTabChange: (key: string) => {
          this.state.mode = key === 'guided' || key === 'free' ? key : 'guided';
          this.state.onchange?.();
          m.redraw();
        },
      }),
      currentMode === 'guided'
        ? this.renderGuidedMode()
        : this.renderFreeMode(),
    ]);
  }

  private renderGuidedMode(): m.Child {
    if (!this.rightNode) {
      const suggestions = this.getJoinSuggestions();

      return m(
        'div',
        m(
          Card,
          m('h3', 'Join Suggestions'),
          suggestions.length > 0
            ? m(
                'div',
                {style: {display: 'flex', flexDirection: 'column', gap: '8px'}},
                [
                  m(
                    'p',
                    {style: {marginBottom: '8px', color: '#888'}},
                    'Based on your JOINID columns, you could join with:',
                  ),
                  suggestions.map((s) => {
                    const availableColumns = this.getTableColumns(
                      s.suggestedTable,
                    );
                    const selectedColumns =
                      this.state.suggestionSelections?.get(s.suggestedTable) ??
                      [];
                    const isExpanded =
                      this.state.expandedSuggestions?.has(s.suggestedTable) ??
                      false;

                    return m(
                      'div',
                      {
                        style: {
                          padding: '8px',
                          backgroundColor: 'rgba(255, 255, 255, 0.05)',
                          borderRadius: '4px',
                          display: 'flex',
                          flexDirection: 'column',
                          gap: '8px',
                        },
                      },
                      [
                        // Header row with table name and expand/collapse
                        m(
                          'div',
                          {
                            style: {
                              display: 'flex',
                              justifyContent: 'space-between',
                              alignItems: 'center',
                              gap: '8px',
                              cursor: 'pointer',
                              userSelect: 'none',
                            },
                            onclick: (e: MouseEvent) => {
                              // Don't toggle if clicking on the button
                              if (
                                (e.target as HTMLElement).closest('button') ||
                                (e.target as HTMLElement).tagName === 'BUTTON'
                              ) {
                                return;
                              }

                              if (!this.state.expandedSuggestions) {
                                this.state.expandedSuggestions = new Set();
                              }
                              if (isExpanded) {
                                this.state.expandedSuggestions.delete(
                                  s.suggestedTable,
                                );
                              } else {
                                this.state.expandedSuggestions.add(
                                  s.suggestedTable,
                                );
                              }
                              m.redraw();
                            },
                          },
                          [
                            m(
                              'span',
                              {
                                style: {
                                  fontFamily: 'monospace',
                                  fontSize: '12px',
                                  display: 'flex',
                                  alignItems: 'center',
                                  gap: '4px',
                                },
                              },
                              [
                                m(
                                  'span',
                                  {
                                    style: {
                                      fontSize: '16px',
                                      lineHeight: '1',
                                    },
                                  },
                                  isExpanded ? '▼' : '▶',
                                ),
                                m('strong', s.suggestedTable),
                                ' table (using ',
                                m('code', s.colName),
                                ' = ',
                                m('code', s.targetColumn),
                                ')',
                                selectedColumns.length > 0 &&
                                  m(
                                    'span',
                                    {
                                      style: {
                                        marginLeft: '8px',
                                        color: '#888',
                                        fontSize: '11px',
                                      },
                                    },
                                    `${selectedColumns.length} selected`,
                                  ),
                              ],
                            ),
                            isExpanded &&
                              selectedColumns.length > 0 &&
                              m(Button, {
                                label: 'Add & Connect',
                                icon: 'add_link',
                                minimal: true,
                                compact: true,
                                onclick: (e: MouseEvent) => {
                                  e.stopPropagation();
                                  if (
                                    this.state.actions?.onAddAndConnectTable
                                  ) {
                                    // Mark this as a guided connection
                                    this.state.isGuidedConnection = true;
                                    // Port index 0 = first left-side input port
                                    this.state.actions.onAddAndConnectTable(
                                      s.suggestedTable,
                                      0,
                                    );
                                    // Pre-set the join columns based on the suggestion
                                    this.state.leftColumn = s.colName;
                                    this.state.rightColumn = s.targetColumn;
                                    // Pre-set the selected columns
                                    this.state.selectedColumns = [
                                      ...selectedColumns,
                                    ];
                                    this.state.onchange?.();
                                  }
                                },
                              }),
                          ],
                        ),
                        // Column selection (only when expanded)
                        isExpanded &&
                          m(
                            'div',
                            {
                              style: {
                                marginTop: '4px',
                                paddingTop: '8px',
                                borderTop: '1px solid rgba(255, 255, 255, 0.1)',
                              },
                            },
                            [
                              m(
                                'div',
                                {
                                  style: {
                                    marginBottom: '8px',
                                    fontSize: '11px',
                                    color: '#888',
                                  },
                                },
                                `Select columns from ${s.suggestedTable} (${availableColumns.length} available):`,
                              ),
                              m(MultiselectInput, {
                                options: availableColumns.map((col) => ({
                                  key: col,
                                  label: col,
                                })),
                                selectedOptions: selectedColumns,
                                onOptionAdd: (key: string) => {
                                  if (!this.state.suggestionSelections) {
                                    this.state.suggestionSelections = new Map();
                                  }
                                  const current =
                                    this.state.suggestionSelections.get(
                                      s.suggestedTable,
                                    ) ?? [];
                                  this.state.suggestionSelections.set(
                                    s.suggestedTable,
                                    [...current, key],
                                  );
                                  m.redraw();
                                },
                                onOptionRemove: (key: string) => {
                                  if (this.state.suggestionSelections) {
                                    const current =
                                      this.state.suggestionSelections.get(
                                        s.suggestedTable,
                                      ) ?? [];
                                    this.state.suggestionSelections.set(
                                      s.suggestedTable,
                                      current.filter((c) => c !== key),
                                    );
                                    m.redraw();
                                  }
                                },
                              }),
                            ],
                          ),
                      ],
                    );
                  }),
                  m(
                    'p',
                    {
                      style: {
                        marginTop: '8px',
                        color: '#888',
                        fontSize: '12px',
                      },
                    },
                    'Connect a table node to the left port to add columns.',
                  ),
                ],
              )
            : m(
                'p',
                {style: {color: '#888'}},
                'No JOINID columns found in your data. You can still connect any node to the left port, or switch to Free mode.',
              ),
        ),
      );
    }

    const leftCols = this.sourceCols;
    const rightCols = this.rightCols;

    return m('div', [
      m(
        CardStack,
        m(
          Card,
          m('h3', 'Select Columns to Add'),
          m(MultiselectInput, {
            options: rightCols.map((c) => ({
              key: c.column.name,
              label: c.column.name,
            })),
            selectedOptions: this.state.selectedColumns ?? [],
            onOptionAdd: (key: string) => {
              if (!this.state.selectedColumns) {
                this.state.selectedColumns = [];
              }
              this.state.selectedColumns.push(key);
              this.state.onchange?.();
              m.redraw();
            },
            onOptionRemove: (key: string) => {
              if (this.state.selectedColumns) {
                this.state.selectedColumns = this.state.selectedColumns.filter(
                  (c) => c !== key,
                );
                // Also remove the alias for this column
                this.state.columnAliases?.delete(key);
                this.state.onchange?.();
                m.redraw();
              }
            },
          }),
          // Show alias inputs for selected columns
          this.state.selectedColumns && this.state.selectedColumns.length > 0
            ? m(
                'div',
                {
                  style: {
                    paddingTop: '5px',
                    borderTop: '1px solid rgba(255, 255, 255, 0.1)',
                  },
                },
                [
                  m(
                    'h4',
                    {style: {marginBottom: '8px'}},
                    'Column Aliases (optional)',
                  ),
                  m(
                    'div',
                    {
                      style: {
                        fontSize: '11px',
                        color: '#888',
                        marginBottom: '8px',
                      },
                    },
                    'Rename columns by providing an alias:',
                  ),
                  this.state.selectedColumns.map((colName) =>
                    m(
                      '.pf-form-row',
                      {
                        style: {
                          display: 'flex',
                          alignItems: 'center',
                          gap: '8px',
                          marginBottom: '8px',
                        },
                      },
                      [
                        m(
                          'code',
                          {style: {minWidth: '120px', fontSize: '12px'}},
                          colName,
                        ),
                        m('span', '→'),
                        m(TextInput, {
                          placeholder: 'alias (optional)',
                          value: this.state.columnAliases?.get(colName) ?? '',
                          oninput: (e: InputEvent) => {
                            const target = e.target as HTMLInputElement;
                            const alias = target.value.trim();
                            if (!this.state.columnAliases) {
                              this.state.columnAliases = new Map();
                            }
                            if (alias) {
                              this.state.columnAliases.set(colName, alias);
                            } else {
                              this.state.columnAliases.delete(colName);
                            }
                            this.state.onchange?.();
                          },
                        }),
                      ],
                    ),
                  ),
                ],
              )
            : null,
        ),
        m(
          Card,
          m('h3', 'Join Condition'),
          m(
            '.pf-form-row',
            m('label', 'Base Column:'),
            m(
              Select,
              {
                onchange: (e: Event) => {
                  const target = e.target as HTMLSelectElement;
                  this.state.leftColumn = target.value;
                  this.state.onchange?.();
                },
              },
              m(
                'option',
                {disabled: true, selected: !this.state.leftColumn},
                'Select column',
              ),
              leftCols.map((col) =>
                m(
                  'option',
                  {
                    value: col.column.name,
                    selected: col.column.name === this.state.leftColumn,
                  },
                  col.column.name,
                ),
              ),
            ),
          ),
          m(
            '.pf-form-row',
            m('label', 'Connected Node Column:'),
            m(
              Select,
              {
                onchange: (e: Event) => {
                  const target = e.target as HTMLSelectElement;
                  this.state.rightColumn = target.value;
                  this.state.onchange?.();
                },
              },
              m(
                'option',
                {disabled: true, selected: !this.state.rightColumn},
                'Select column',
              ),
              rightCols.map((col) =>
                m(
                  'option',
                  {
                    value: col.column.name,
                    selected: col.column.name === this.state.rightColumn,
                  },
                  col.column.name,
                ),
              ),
            ),
          ),
        ),
      ),
    ]);
  }

  private renderFreeMode(): m.Child {
    if (!this.rightNode) {
      return m(
        'div',
        m(
          Card,
          m('h3', 'Free Mode'),
          m(
            'p',
            {style: {color: '#888'}},
            'Connect any node to the left port. All columns from the connected node will be added via LEFT JOIN.',
          ),
        ),
      );
    }

    // Show simple UI when a node is connected
    return m(
      'div',
      m(
        Card,
        m('h3', 'Connected Node'),
        m(
          'p',
          {style: {marginBottom: '8px'}},
          `All ${this.rightCols.length} columns from the connected node will be added.`,
        ),
        m(
          'p',
          {style: {color: '#888', fontSize: '12px'}},
          'Switch to Guided mode to select specific columns and configure the join condition.',
        ),
      ),
    );
  }

  nodeInfo(): m.Children {
    return m(
      'div',
      m(
        'p',
        'Enrich your data by adding columns from another table or query. Connect the additional source to the left port.',
      ),
      m(
        'p',
        'Specify which columns to match (join key) and which columns to add. In Guided mode, get suggestions based on JOINID columns.',
      ),
      m(
        'p',
        m('strong', 'Example:'),
        ' Add process details to slices by joining ',
        m('code', 'upid'),
        ' with the process table.',
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

    // Require a node to be connected to add columns from
    if (this.rightNode === undefined) {
      setValidationError(this.state, 'No node connected to add columns from');
      return false;
    }

    // In free mode, we use default join columns, so it's always valid
    if (this.state.mode === 'free') {
      return true;
    }

    // In guided mode, we need valid join columns
    if (!this.state.leftColumn || !this.state.rightColumn) {
      setValidationError(
        this.state,
        'Guided mode requires both left and right join columns to be selected',
      );
      return false;
    }

    return true;
  }

  clone(): QueryNode {
    return new AddColumnsNode(this.state);
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (!this.validate()) return undefined;
    if (!this.rightNode) return this.prevNode.getStructuredQuery();

    // Prepare input columns based on mode
    const inputColumns: ColumnSpec[] =
      this.state.mode === 'free'
        ? this.rightCols.map((col) => ({
            columnNameOrExpression: col.column.name,
          }))
        : (this.state.selectedColumns ?? []).map((colName) => {
            const alias = this.state.columnAliases?.get(colName);
            return {
              columnNameOrExpression: colName,
              alias: alias && alias.trim() !== '' ? alias.trim() : undefined,
            };
          });

    // Prepare join condition
    const condition: JoinCondition = {
      type: 'equality',
      leftColumn: this.state.mode === 'free' ? 'id' : this.state.leftColumn!,
      rightColumn: this.state.mode === 'free' ? 'id' : this.state.rightColumn!,
    };

    return StructuredQueryBuilder.withAddColumns(
      this.prevNode,
      this.rightNode,
      inputColumns,
      condition,
      this.nodeId,
    );
  }

  serializeState(): object {
    return {
      selectedColumns: this.state.selectedColumns,
      leftColumn: this.state.leftColumn,
      rightColumn: this.state.rightColumn,
      mode: this.state.mode,
      suggestionSelections: this.state.suggestionSelections
        ? Object.fromEntries(this.state.suggestionSelections)
        : undefined,
      expandedSuggestions: this.state.expandedSuggestions
        ? Array.from(this.state.expandedSuggestions)
        : undefined,
      columnAliases: this.state.columnAliases
        ? Object.fromEntries(this.state.columnAliases)
        : undefined,
      isGuidedConnection: this.state.isGuidedConnection,
      comment: this.state.comment,
      autoExecute: this.state.autoExecute,
    };
  }

  static deserializeState(
    serializedState: AddColumnsNodeState,
  ): AddColumnsNodeState {
    return {
      ...serializedState,
      prevNode: undefined as unknown as QueryNode,
      suggestionSelections:
        (serializedState.suggestionSelections as unknown as Record<
          string,
          string[]
        >) !== undefined
          ? new Map(
              Object.entries(
                serializedState.suggestionSelections as unknown as Record<
                  string,
                  string[]
                >,
              ),
            )
          : undefined,
      expandedSuggestions:
        (serializedState.expandedSuggestions as unknown as string[]) !==
        undefined
          ? new Set(serializedState.expandedSuggestions as unknown as string[])
          : undefined,
      columnAliases:
        (serializedState.columnAliases as unknown as Record<string, string>) !==
        undefined
          ? new Map(
              Object.entries(
                serializedState.columnAliases as unknown as Record<
                  string,
                  string
                >,
              ),
            )
          : undefined,
    };
  }
}
