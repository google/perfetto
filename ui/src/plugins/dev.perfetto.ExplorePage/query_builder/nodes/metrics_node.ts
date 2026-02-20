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
import {
  QueryNode,
  QueryNodeState,
  nextNodeId,
  NodeType,
} from '../../query_node';
import protos from '../../../../protos';
import {ColumnInfo, newColumnInfoList} from '../column_info';
import {NodeIssues} from '../node_issues';
import {OutlinedField} from '../widgets';
import {NodeModifyAttrs, NodeDetailsAttrs} from '../node_explorer_types';
import {Button, ButtonVariant} from '../../../../widgets/button';
import {loadNodeDoc} from '../node_doc_loader';
import {
  ColumnName,
  NodeDetailsMessage,
  NodeDetailsSpacer,
  NodeTitle,
} from '../node_styling_widgets';
import {isNumericType} from '../utils';
import {classNames} from '../../../../base/classnames';
import {Icons} from '../../../../base/semantic_icons';
import {Icon} from '../../../../widgets/icon';
import {
  getDimensionUniquenessOptions,
  getMetricUnitOptions,
  getPolarityOptions,
} from './metrics_enum_utils';
import {showMetricsExportModal} from './metrics_export_modal';

export interface ValueColumnConfig {
  column: string;
  unit: string;
  customUnit?: string;
  polarity: string;
  expanded?: boolean;
}

export interface MetricsSerializedState {
  metricIdPrefix: string;
  valueColumns?: ValueColumnConfig[];
  dimensionUniqueness: string;
}

export interface MetricsNodeState extends QueryNodeState {
  metricIdPrefix: string;
  valueColumns: ValueColumnConfig[];
  dimensionUniqueness: string;
  availableColumns: ColumnInfo[];
}

// Drag data transferred between the two column lists.
interface DragPayload {
  source: 'dimensions' | 'values';
  columnName: string;
}

const DRAG_MIME = 'application/x-metrics-column';

export class MetricsNode implements QueryNode {
  readonly nodeId: string;
  readonly type = NodeType.kMetrics;
  primaryInput?: QueryNode;
  nextNodes: QueryNode[];
  readonly state: MetricsNodeState;

  // Transient drag-and-drop state (not serialized).
  private dragOverTarget?: 'dimensions' | 'values';
  private dragPayload?: DragPayload;

  get finalCols(): ColumnInfo[] {
    return this.primaryInput?.finalCols ?? [];
  }

  constructor(state: MetricsNodeState) {
    this.nodeId = nextNodeId();
    this.state = {
      ...state,
      metricIdPrefix: state.metricIdPrefix ?? '',
      valueColumns: state.valueColumns ?? [],
      dimensionUniqueness: state.dimensionUniqueness ?? 'NOT_UNIQUE',
      availableColumns: state.availableColumns ?? [],
    };
    this.nextNodes = [];
  }

  getDimensions(): string[] {
    const valueNames = new Set(this.state.valueColumns.map((vc) => vc.column));
    return this.state.availableColumns
      .filter((c) => !valueNames.has(c.name))
      .map((c) => c.name);
  }

  onPrevNodesUpdated() {
    this.updateAvailableColumns();
  }

  updateAvailableColumns() {
    if (this.primaryInput === undefined) {
      return;
    }
    this.state.availableColumns = newColumnInfoList(
      this.primaryInput.finalCols ?? [],
      false,
    );

    // Remove value columns whose column no longer exists or is no longer numeric.
    this.state.valueColumns = this.state.valueColumns.filter((vc) => {
      const col = this.state.availableColumns.find((c) => c.name === vc.column);
      return col !== undefined && isNumericType(col.type);
    });
  }

  validate(): boolean {
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

    if (!this.state.metricIdPrefix || this.state.metricIdPrefix.trim() === '') {
      this.setValidationError('Metric ID prefix is required');
      return false;
    }

    if (this.state.valueColumns.length === 0) {
      this.setValidationError('At least one value column is required');
      return false;
    }

    const inputCols = this.primaryInput.finalCols ?? [];
    for (const vc of this.state.valueColumns) {
      if (
        vc.unit === 'CUSTOM' &&
        (!vc.customUnit || vc.customUnit.trim() === '')
      ) {
        this.setValidationError(
          `Custom unit is required for value column '${vc.column}'`,
        );
        return false;
      }

      const col = inputCols.find((c) => c.name === vc.column);
      if (col === undefined) {
        this.setValidationError(
          `Value column '${vc.column}' not found in input`,
        );
        return false;
      }
      if (!isNumericType(col.type)) {
        this.setValidationError(
          `Value column '${vc.column}' must be numeric (got ${col.type})`,
        );
        return false;
      }
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
    return 'Metrics';
  }

  nodeDetails(): NodeDetailsAttrs {
    const details: m.Child[] = [NodeTitle(this.getTitle())];

    if (!this.state.metricIdPrefix || this.state.metricIdPrefix.trim() === '') {
      details.push(NodeDetailsMessage('Metric ID prefix required'));
      return {
        content: m('.pf-metrics-v2-node-details', details),
      };
    }

    details.push(
      m('div', 'ID prefix: ', ColumnName(this.state.metricIdPrefix)),
    );

    if (this.state.valueColumns.length > 0) {
      details.push(NodeDetailsSpacer());
      const label =
        this.state.valueColumns.length === 1 ? 'Value: ' : 'Values: ';
      details.push(
        m(
          'div',
          label,
          this.state.valueColumns.map((vc, i) => [
            ColumnName(vc.column),
            i < this.state.valueColumns.length - 1 ? ', ' : '',
          ]),
        ),
      );
    }

    const dimensions = this.getDimensions();
    if (dimensions.length > 0) {
      details.push(
        m(
          'div',
          'Dimensions: ',
          dimensions.map((d, i) => [
            ColumnName(d),
            i < dimensions.length - 1 ? ', ' : '',
          ]),
        ),
      );
    }

    return {
      content: m('.pf-metrics-v2-node-details', details),
    };
  }

  nodeSpecificModify(): NodeModifyAttrs {
    const sections: NodeModifyAttrs['sections'] = [];

    // Metric ID prefix and Export button row.
    const canExport =
      this.primaryInput !== undefined &&
      !!this.state.metricIdPrefix?.trim() &&
      this.state.valueColumns.length > 0;
    sections.push({
      content: m(
        '.pf-metrics-header-row',
        m(OutlinedField, {
          label: 'Metric ID prefix',
          value: this.state.metricIdPrefix,
          placeholder: 'e.g., memory_per_process',
          oninput: (e: Event) => {
            this.state.metricIdPrefix = (e.target as HTMLInputElement).value;
            this.state.onchange?.();
          },
        }),
        m(Button, {
          label: 'Export',
          icon: 'download',
          onclick: () => this.showExportModal(),
          disabled: !canExport,
          variant: ButtonVariant.Outlined,
          className: 'pf-metrics-v2-export-button',
        }),
      ),
    });

    // Two-column drag-and-drop layout: Dimensions | Values
    sections.push({
      content: this.renderColumnsLayout(),
    });

    // Dimension uniqueness section
    sections.push({
      content: m(
        OutlinedField,
        {
          label: 'Dimension uniqueness',
          value: this.state.dimensionUniqueness,
          onchange: (e: Event) => {
            this.state.dimensionUniqueness = (
              e.target as HTMLSelectElement
            ).value;
            this.state.onchange?.();
          },
        },
        getDimensionUniquenessOptions().map((d) =>
          m(
            'option',
            {
              value: d.value,
              selected: this.state.dimensionUniqueness === d.value,
            },
            d.label,
          ),
        ),
      ),
    });

    return {
      info: 'Configure trace-based metrics. Drag numeric columns from Dimensions to Values to create metrics. Each value column becomes a separate metric with its own unit and polarity.',
      sections,
    };
  }

  private renderColumnsLayout(): m.Children {
    const dimensionCols = this.getDimensionColumns();

    return m(
      '.pf-metrics-v2-columns-layout',
      // Dimensions panel
      this.renderDimensionsPanel(dimensionCols),
      // Values panel
      this.renderValuesPanel(dimensionCols),
    );
  }

  private getDimensionColumns(): ColumnInfo[] {
    const valueNames = new Set(this.state.valueColumns.map((vc) => vc.column));
    return this.state.availableColumns.filter((c) => !valueNames.has(c.name));
  }

  private renderDimensionsPanel(dimensionCols: ColumnInfo[]): m.Children {
    const isDropTarget = this.dragOverTarget === 'dimensions';
    return m(
      '.pf-metrics-v2-column-panel',
      {
        className: classNames(isDropTarget && 'pf-drop-active'),
        ondragover: (e: DragEvent) => {
          if (!e.dataTransfer?.types.includes(DRAG_MIME)) return;
          e.preventDefault();
          this.dragOverTarget = 'dimensions';
        },
        ondragleave: (e: DragEvent) => {
          const target = e.currentTarget as HTMLElement;
          const related = e.relatedTarget as HTMLElement | null;
          if (!related || !target.contains(related)) {
            if (this.dragOverTarget === 'dimensions') {
              this.dragOverTarget = undefined;
            }
          }
        },
        ondrop: (e: DragEvent) => {
          e.preventDefault();
          this.dragOverTarget = undefined;
          const payload = this.readDragPayload(e);
          if (payload === undefined || payload.source !== 'values') return;
          this.moveToValues(payload.columnName, false);
        },
      },
      m(
        '.pf-metrics-v2-column-panel__header',
        `Dimensions (${dimensionCols.length})`,
      ),
      m(
        '.pf-metrics-v2-column-panel__list',
        dimensionCols.length === 0
          ? m('.pf-metrics-v2-empty-hint', 'No dimensions')
          : dimensionCols.map((col) => {
              const numeric = isNumericType(col.type);
              return m(
                '.pf-metrics-v2-column-item',
                {
                  // Only numeric columns can be dragged to Values.
                  draggable: numeric,
                  ondragstart: numeric
                    ? (e: DragEvent) => {
                        const payload: DragPayload = {
                          source: 'dimensions',
                          columnName: col.name,
                        };
                        this.dragPayload = payload;
                        e.dataTransfer?.setData(
                          DRAG_MIME,
                          JSON.stringify(payload),
                        );
                        e.dataTransfer?.setData('text/plain', col.name);
                      }
                    : undefined,
                  ondragend: numeric
                    ? () => {
                        this.dragPayload = undefined;
                        this.dragOverTarget = undefined;
                      }
                    : undefined,
                },
                numeric
                  ? m(Icon, {
                      icon: Icons.DragHandle,
                      className: 'pf-metrics-v2-drag-handle',
                    })
                  : m('span.pf-metrics-v2-drag-handle'),
                m('span.pf-metrics-v2-col-name', col.name),
                m('span.pf-metrics-v2-col-type', col.type),
                numeric
                  ? m(Button, {
                      icon: 'arrow_forward',
                      compact: true,
                      title: 'Move to values',
                      onclick: () => this.moveToValues(col.name, true),
                    })
                  : undefined,
              );
            }),
      ),
    );
  }

  private renderValuesPanel(dimensionCols: ColumnInfo[]): m.Children {
    const isDropTarget = this.dragOverTarget === 'values';
    // Check if the currently dragged column is non-numeric (reject).
    const isRejected =
      isDropTarget &&
      this.dragPayload?.source === 'dimensions' &&
      (() => {
        const col = this.state.availableColumns.find(
          (c) => c.name === this.dragPayload?.columnName,
        );
        return col !== undefined && !isNumericType(col.type);
      })();

    // Numeric columns available to add (not already in values).
    const numericDimensions = dimensionCols.filter((c) =>
      isNumericType(c.type),
    );

    return m(
      '.pf-metrics-v2-column-panel.pf-metrics-v2-values-panel',
      {
        className: classNames(
          isDropTarget && !isRejected && 'pf-drop-active',
          isRejected && 'pf-drop-rejected',
        ),
        ondragover: (e: DragEvent) => {
          if (!e.dataTransfer?.types.includes(DRAG_MIME)) return;
          this.dragOverTarget = 'values';
          // Only allow drop of numeric columns from dimensions.
          if (this.dragPayload?.source === 'dimensions') {
            const col = this.state.availableColumns.find(
              (c) => c.name === this.dragPayload?.columnName,
            );
            if (col !== undefined && isNumericType(col.type)) {
              e.preventDefault();
            }
            // Non-numeric: don't preventDefault → drop not allowed.
          } else if (this.dragPayload?.source === 'values') {
            // Reordering within values - allow.
            e.preventDefault();
          }
        },
        ondragleave: (e: DragEvent) => {
          const target = e.currentTarget as HTMLElement;
          const related = e.relatedTarget as HTMLElement | null;
          if (!related || !target.contains(related)) {
            if (this.dragOverTarget === 'values') {
              this.dragOverTarget = undefined;
            }
          }
        },
        ondrop: (e: DragEvent) => {
          e.preventDefault();
          this.dragOverTarget = undefined;
          const payload = this.readDragPayload(e);
          if (payload === undefined) return;
          if (payload.source === 'dimensions') {
            this.moveToValues(payload.columnName, true);
          }
          // Reordering within values could be added later.
        },
      },
      m(
        '.pf-metrics-v2-column-panel__header',
        `Values (${this.state.valueColumns.length})`,
      ),
      m(
        '.pf-metrics-v2-column-panel__list',
        this.state.valueColumns.length === 0
          ? m('.pf-metrics-v2-empty-hint', 'Drag numeric columns here')
          : this.state.valueColumns.map((vc, index) =>
              this.renderValueItem(vc, index),
            ),
        // "Add value column" button if there are numeric dimensions available.
        numericDimensions.length > 0
          ? m(
              '.pf-metrics-v2-add-value',
              m(
                OutlinedField,
                {
                  label: 'Add value column',
                  value: '',
                  onchange: (e: Event) => {
                    const name = (e.target as HTMLSelectElement).value;
                    if (name) {
                      this.moveToValues(name, true);
                      // Reset select.
                      (e.target as HTMLSelectElement).value = '';
                    }
                  },
                },
                [
                  m('option', {value: '', disabled: true}, 'Select column...'),
                  ...numericDimensions.map((col) =>
                    m('option', {value: col.name}, `${col.name} (${col.type})`),
                  ),
                ],
              ),
            )
          : undefined,
      ),
    );
  }

  private renderValueItem(vc: ValueColumnConfig, index: number): m.Children {
    const col = this.state.availableColumns.find((c) => c.name === vc.column);
    const colType = col?.type ?? '?';
    const needsCustomUnit = vc.unit === 'CUSTOM';
    const isExpanded = vc.expanded ?? false;

    return m(
      '.pf-metrics-v2-column-item.pf-metrics-v2-value-item',
      {
        draggable: true,
        ondragstart: (e: DragEvent) => {
          const payload: DragPayload = {
            source: 'values',
            columnName: vc.column,
          };
          this.dragPayload = payload;
          e.dataTransfer?.setData(DRAG_MIME, JSON.stringify(payload));
          e.dataTransfer?.setData('text/plain', vc.column);
        },
        ondragend: () => {
          this.dragPayload = undefined;
          this.dragOverTarget = undefined;
        },
      },
      // Header row with name, type, expand toggle, and remove button
      m(
        '.pf-metrics-v2-value-header',
        m(Icon, {
          icon: Icons.DragHandle,
          className: 'pf-metrics-v2-drag-handle',
        }),
        m('span.pf-metrics-v2-col-name', vc.column),
        m('span.pf-metrics-v2-col-type', colType),
        m(Button, {
          icon: isExpanded ? 'expand_more' : 'chevron_right',
          compact: true,
          title: isExpanded ? 'Collapse config' : 'Expand config',
          onclick: () => {
            this.state.valueColumns[index] = {...vc, expanded: !isExpanded};
          },
        }),
        m(Button, {
          icon: 'close',
          compact: true,
          title: 'Move back to dimensions',
          onclick: () => this.moveToValues(vc.column, false),
        }),
      ),
      // Config controls: unit + polarity (only shown when expanded)
      isExpanded &&
        m(
          '.pf-metrics-v2-value-config',
          m(
            OutlinedField,
            {
              label: 'Unit',
              value: vc.unit,
              onchange: (e: Event) => {
                const newUnit = (e.target as HTMLSelectElement).value;
                this.state.valueColumns[index] = {
                  ...vc,
                  unit: newUnit,
                  customUnit: newUnit !== 'CUSTOM' ? undefined : vc.customUnit,
                };
                this.state.onchange?.();
              },
            },
            getMetricUnitOptions().map((u) =>
              m(
                'option',
                {value: u.value, selected: vc.unit === u.value},
                u.label,
              ),
            ),
          ),
          needsCustomUnit
            ? m(OutlinedField, {
                label: 'Custom unit',
                value: vc.customUnit ?? '',
                placeholder: 'Enter custom unit...',
                oninput: (e: Event) => {
                  this.state.valueColumns[index] = {
                    ...vc,
                    customUnit: (e.target as HTMLInputElement).value,
                  };
                  this.state.onchange?.();
                },
              })
            : undefined,
          m(
            OutlinedField,
            {
              label: 'Polarity',
              value: vc.polarity,
              onchange: (e: Event) => {
                this.state.valueColumns[index] = {
                  ...vc,
                  polarity: (e.target as HTMLSelectElement).value,
                };
                this.state.onchange?.();
              },
            },
            getPolarityOptions().map((p) =>
              m(
                'option',
                {value: p.value, selected: vc.polarity === p.value},
                p.label,
              ),
            ),
          ),
        ),
    );
  }

  /**
   * Moves a column into or out of the values list.
   * @param columnName The column to move.
   * @param toValues If true, move from dimensions to values. If false, move from values to dimensions.
   */
  private moveToValues(columnName: string, toValues: boolean): void {
    if (toValues) {
      // Guard: only numeric columns can be value columns.
      const col = this.state.availableColumns.find(
        (c) => c.name === columnName,
      );
      if (col === undefined || !isNumericType(col.type)) return;

      // Add to values with default config.
      const alreadyExists = this.state.valueColumns.some(
        (vc) => vc.column === columnName,
      );
      if (alreadyExists) return;
      this.state.valueColumns = [
        ...this.state.valueColumns,
        {
          column: columnName,
          unit: 'COUNT',
          polarity: 'NOT_APPLICABLE',
        },
      ];
    } else {
      // Remove from values (moves back to dimensions implicitly).
      this.state.valueColumns = this.state.valueColumns.filter(
        (vc) => vc.column !== columnName,
      );
    }
    this.state.onchange?.();
  }

  private readDragPayload(e: DragEvent): DragPayload | undefined {
    const raw = e.dataTransfer?.getData(DRAG_MIME);
    if (raw === undefined || raw === '') return undefined;
    try {
      return JSON.parse(raw) as DragPayload;
    } catch {
      return undefined;
    }
  }

  private async showExportModal(): Promise<void> {
    const templateSpec = this.getMetricTemplateSpec();
    if (templateSpec === undefined) return;

    await showMetricsExportModal({
      templateSpec,
      primaryInput: this.primaryInput,
      metricIdPrefix: this.state.metricIdPrefix,
      dimensions: this.getDimensions(),
      valueColumns: [...this.state.valueColumns],
      engine: this.state.trace?.engine,
    });
  }

  nodeInfo(): m.Children {
    return loadNodeDoc('metrics');
  }

  clone(): QueryNode {
    const stateCopy: MetricsNodeState = {
      metricIdPrefix: this.state.metricIdPrefix,
      valueColumns: this.state.valueColumns.map((vc) => ({...vc})),
      dimensionUniqueness: this.state.dimensionUniqueness,
      availableColumns: newColumnInfoList(this.state.availableColumns),
      onchange: this.state.onchange,
    };
    return new MetricsNode(stateCopy);
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (!this.validate()) return undefined;
    if (this.primaryInput === undefined) return undefined;

    const inputQuery = this.primaryInput.getStructuredQuery();
    if (inputQuery === undefined) return undefined;

    const sq = new protos.PerfettoSqlStructuredQuery();
    sq.id = this.nodeId;
    sq.innerQueryId = this.primaryInput.nodeId;
    return sq;
  }

  getMetricTemplateSpec(): protos.TraceMetricV2TemplateSpec | undefined {
    if (!this.validate()) return undefined;
    if (this.primaryInput === undefined) return undefined;

    const inputQuery = this.primaryInput.getStructuredQuery();
    if (inputQuery === undefined) return undefined;

    const templateSpec = new protos.TraceMetricV2TemplateSpec();
    templateSpec.idPrefix = this.state.metricIdPrefix;
    templateSpec.dimensions = this.getDimensions();
    templateSpec.query = inputQuery;

    if (this.state.dimensionUniqueness === 'UNIQUE') {
      templateSpec.dimensionUniqueness =
        protos.TraceMetricV2Spec.DimensionUniqueness.UNIQUE;
    } else {
      templateSpec.dimensionUniqueness =
        protos.TraceMetricV2Spec.DimensionUniqueness.NOT_UNIQUE;
    }

    templateSpec.valueColumnSpecs = this.state.valueColumns.map((vc) => {
      const valueSpec = new protos.TraceMetricV2TemplateSpec.ValueColumnSpec();
      valueSpec.name = vc.column;

      if (vc.unit === 'CUSTOM' && vc.customUnit) {
        valueSpec.customUnit = vc.customUnit;
      } else if (vc.unit !== 'CUSTOM') {
        const unitEnum =
          protos.TraceMetricV2Spec.MetricUnit[
            vc.unit as keyof typeof protos.TraceMetricV2Spec.MetricUnit
          ];
        if (unitEnum !== undefined) {
          valueSpec.unit = unitEnum;
        }
      }

      const polarityEnum =
        protos.TraceMetricV2Spec.MetricPolarity[
          vc.polarity as keyof typeof protos.TraceMetricV2Spec.MetricPolarity
        ];
      if (polarityEnum !== undefined) {
        valueSpec.polarity = polarityEnum;
      }

      return valueSpec;
    });

    return templateSpec;
  }

  serializeState(): MetricsSerializedState & {primaryInputId?: string} {
    return {
      primaryInputId: this.primaryInput?.nodeId,
      metricIdPrefix: this.state.metricIdPrefix,
      valueColumns: this.state.valueColumns.map(({expanded: _, ...vc}) => vc),
      dimensionUniqueness: this.state.dimensionUniqueness,
    };
  }

  static deserializeState(state: MetricsSerializedState): MetricsNodeState {
    // Handle migration from old metricId to metricIdPrefix.
    const metricIdPrefix =
      state.metricIdPrefix ??
      (state as unknown as {metricId?: string}).metricId ??
      '';

    let valueColumns: ValueColumnConfig[] = [];

    if (state.valueColumns !== undefined && Array.isArray(state.valueColumns)) {
      // New format: valueColumns array.
      valueColumns = state.valueColumns;
    } else {
      // Migration from old single-value format.
      const old = state as unknown as {
        valueColumn?: string;
        unit?: string;
        customUnit?: string;
        polarity?: string;
        values?: Array<{
          column?: string;
          unit: string;
          customUnit?: string;
          polarity: string;
        }>;
      };

      if (old.valueColumn !== undefined) {
        valueColumns = [
          {
            column: old.valueColumn,
            unit: old.unit ?? 'COUNT',
            customUnit: old.customUnit,
            polarity: old.polarity ?? 'NOT_APPLICABLE',
          },
        ];
      } else if (old.values !== undefined && old.values.length > 0) {
        // Migration from old multi-value format.
        valueColumns = old.values
          .filter((v) => v.column !== undefined)
          .map((v) => ({
            column: v.column ?? '',
            unit: v.unit ?? 'COUNT',
            customUnit: v.customUnit,
            polarity: v.polarity ?? 'NOT_APPLICABLE',
          }));
      }
    }

    return {
      metricIdPrefix,
      valueColumns,
      dimensionUniqueness: state.dimensionUniqueness ?? 'NOT_UNIQUE',
      availableColumns: [],
    };
  }
}
