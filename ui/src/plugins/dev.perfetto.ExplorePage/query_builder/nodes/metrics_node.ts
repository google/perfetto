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
import {LabeledControl, OutlinedField} from '../widgets';
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
import {showModal} from '../../../../widgets/modal';
import {CodeSnippet} from '../../../../widgets/code_snippet';
import {traceSummarySpecToText} from '../../../../base/proto_utils_wasm';
import {Spinner} from '../../../../widgets/spinner';
import {DataGrid} from '../../../../components/widgets/datagrid/datagrid';
import {SchemaRegistry} from '../../../../components/widgets/datagrid/datagrid_schema';
import {Row} from '../../../../trace_processor/query_result';
import {
  getStructuredQueries,
  buildEmbeddedQueryTree,
} from '../query_builder_utils';

interface EnumOption {
  value: string;
  label: string;
}

/**
 * Converts an UPPER_SNAKE_CASE enum key to a human-readable label.
 * E.g., "TIME_NANOS" -> "Time nanos", "HIGHER_IS_BETTER" -> "Higher is better"
 */
function enumKeyToLabel(key: string): string {
  return key
    .toLowerCase()
    .split('_')
    .map((word, i) =>
      i === 0 ? word.charAt(0).toUpperCase() + word.slice(1) : word,
    )
    .join(' ');
}

/**
 * Extracts enum options from a protobuf enum object.
 * Filters out UNSPECIFIED values and converts keys to human-readable labels.
 */
function getEnumOptions(
  enumObj: Record<string, string | number>,
  excludePatterns: string[] = ['UNSPECIFIED'],
): EnumOption[] {
  const options: EnumOption[] = [];
  for (const key of Object.keys(enumObj)) {
    // Skip numeric reverse mappings and excluded patterns
    if (typeof enumObj[key] !== 'number') continue;
    if (excludePatterns.some((pattern) => key.includes(pattern))) continue;
    // Skip legacy values
    if (key.includes('LEGACY')) continue;

    options.push({
      value: key,
      label: enumKeyToLabel(key),
    });
  }
  return options;
}

/**
 * Returns metric unit options from the proto enum, plus a CUSTOM option.
 */
function getMetricUnitOptions(): EnumOption[] {
  const options = getEnumOptions(protos.TraceMetricV2Spec.MetricUnit);
  // Add custom unit option at the end
  options.push({value: 'CUSTOM', label: 'Custom unit...'});
  return options;
}

/**
 * Returns polarity options from the proto enum.
 */
function getPolarityOptions(): EnumOption[] {
  return getEnumOptions(protos.TraceMetricV2Spec.MetricPolarity);
}

/**
 * Returns dimension uniqueness options from the proto enum.
 */
function getDimensionUniquenessOptions(): EnumOption[] {
  return getEnumOptions(protos.TraceMetricV2Spec.DimensionUniqueness);
}

// Helper to extract dimension value as string.
function getDimensionValue(
  dim: protos.TraceMetricV2Bundle.Row.IDimension,
): string {
  if (dim.stringValue !== undefined && dim.stringValue !== null) {
    return dim.stringValue;
  }
  if (dim.int64Value !== undefined && dim.int64Value !== null) {
    return String(dim.int64Value);
  }
  if (dim.doubleValue !== undefined && dim.doubleValue !== null) {
    return String(dim.doubleValue);
  }
  if (dim.boolValue !== undefined && dim.boolValue !== null) {
    return String(dim.boolValue);
  }
  return 'NULL';
}

// Helper to extract metric value as number or null.
function getMetricValue(
  val: protos.TraceMetricV2Bundle.Row.IValue,
): number | null {
  if (val.doubleValue !== undefined && val.doubleValue !== null) {
    return val.doubleValue;
  }
  return null;
}

interface MetricResult {
  schema: SchemaRegistry;
  rows: Row[];
  metricId: string;
}

// Parse a single metric bundle from a TraceSummary proto.
// Column names must be provided because the response bundles don't
// include specs when using template specs.
function parseFirstMetricBundle(
  data: Uint8Array,
  metricId: string,
  dimensionNames: string[],
  valueColumn: string,
): MetricResult | undefined {
  const summary = protos.TraceSummary.decode(data);
  const bundle = summary.metricBundles[0];
  if (bundle === undefined) return undefined;

  // Build schema columns: dimensions (text) + value (quantitative).
  const schemaColumns: Record<
    string,
    {title: string; columnType: 'text' | 'quantitative'}
  > = {};
  for (const dim of dimensionNames) {
    schemaColumns[dim] = {title: dim, columnType: 'text'};
  }
  schemaColumns[valueColumn] = {title: valueColumn, columnType: 'quantitative'};

  const schema: SchemaRegistry = {[metricId]: schemaColumns};

  // Convert rows to DataGrid format.
  const rows: Row[] = [];
  for (const row of bundle.row ?? []) {
    const rowData: Row = {};
    for (let i = 0; i < dimensionNames.length; i++) {
      const dimValue = row.dimension?.[i];
      rowData[dimensionNames[i]] = dimValue
        ? getDimensionValue(dimValue)
        : null;
    }
    // Template spec produces one value per row.
    const val = row.values?.[0];
    rowData[valueColumn] = val ? getMetricValue(val) : null;
    rows.push(rowData);
  }

  return {schema, rows, metricId};
}

export interface MetricsSerializedState {
  metricIdPrefix: string;
  valueColumn?: string;
  unit: string;
  customUnit?: string;
  polarity: string;
  dimensionUniqueness: string;
}

export interface MetricsNodeState extends QueryNodeState {
  metricIdPrefix: string;
  valueColumn?: string;
  unit: string;
  customUnit?: string;
  polarity: string;
  dimensionUniqueness: string;
  // Available columns from the input (for UI selection)
  availableColumns: ColumnInfo[];
}

export class MetricsNode implements QueryNode {
  readonly nodeId: string;
  readonly type = NodeType.kMetrics;
  primaryInput?: QueryNode;
  nextNodes: QueryNode[];
  readonly state: MetricsNodeState;

  get finalCols(): ColumnInfo[] {
    // Metrics node outputs a TraceMetricV2TemplateSpec, not SQL columns
    // Pass through the input columns since the node doesn't modify them
    // The actual output is the metric template spec proto
    return this.primaryInput?.finalCols ?? [];
  }

  constructor(state: MetricsNodeState) {
    this.nodeId = nextNodeId();
    this.state = {
      ...state,
      metricIdPrefix: state.metricIdPrefix ?? '',
      valueColumn: state.valueColumn,
      unit: state.unit ?? 'COUNT',
      customUnit: state.customUnit,
      polarity: state.polarity ?? 'NOT_APPLICABLE',
      dimensionUniqueness: state.dimensionUniqueness ?? 'NOT_UNIQUE',
      availableColumns: state.availableColumns ?? [],
    };
    this.nextNodes = [];
  }

  /**
   * Returns the dimensions for this metric.
   * Dimensions are all columns except the value column.
   */
  getDimensions(): string[] {
    if (this.state.valueColumn === undefined) {
      return this.state.availableColumns.map((c) => c.name);
    }
    return this.state.availableColumns
      .filter((c) => c.name !== this.state.valueColumn)
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

    // Validate that selected value column still exists and is numeric
    if (this.state.valueColumn !== undefined) {
      const valueCol = this.state.availableColumns.find(
        (c) => c.name === this.state.valueColumn,
      );
      if (valueCol === undefined || !isNumericType(valueCol.type)) {
        this.state.valueColumn = undefined;
      }
    }
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

    // Validate metric ID prefix
    if (!this.state.metricIdPrefix || this.state.metricIdPrefix.trim() === '') {
      this.setValidationError('Metric ID prefix is required');
      return false;
    }

    // Check for custom unit issue
    if (
      this.state.valueColumn !== undefined &&
      this.state.unit === 'CUSTOM' &&
      (!this.state.customUnit || this.state.customUnit.trim() === '')
    ) {
      this.setValidationError(
        `Custom unit is required for value column '${this.state.valueColumn}'`,
      );
      return false;
    }

    // Must have a value column
    if (
      this.state.valueColumn === undefined ||
      this.state.valueColumn.trim() === ''
    ) {
      this.setValidationError('A value column is required');
      return false;
    }

    // Check that the value column exists and is numeric
    const inputCols = this.primaryInput.finalCols ?? [];
    const valueCol = inputCols.find((c) => c.name === this.state.valueColumn);
    if (valueCol === undefined) {
      this.setValidationError(
        `Value column '${this.state.valueColumn}' not found in input`,
      );
      return false;
    }
    if (!isNumericType(valueCol.type)) {
      this.setValidationError(
        `Value column '${this.state.valueColumn}' must be numeric (got ${valueCol.type})`,
      );
      return false;
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

    // Show invalid state when metric ID prefix is empty
    if (!this.state.metricIdPrefix || this.state.metricIdPrefix.trim() === '') {
      details.push(NodeDetailsMessage('Metric ID prefix required'));
      return {
        content: m('.pf-metrics-v2-node-details', details),
      };
    }

    details.push(
      m('div', 'ID prefix: ', ColumnName(this.state.metricIdPrefix)),
    );

    if (this.state.valueColumn !== undefined) {
      details.push(NodeDetailsSpacer());
      details.push(m('div', 'Value: ', ColumnName(this.state.valueColumn)));
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
    // Use a lightweight check for the button's disabled state rather than
    // calling getMetricTemplateSpec() which triggers full upstream validation.
    const canExport =
      this.primaryInput !== undefined &&
      !!this.state.metricIdPrefix?.trim() &&
      this.state.valueColumn !== undefined;
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

    // Value column controls
    sections.push({
      content: this.renderValueControls(),
    });

    // Show computed dimensions (read-only info)
    const dimensions = this.getDimensions();
    if (dimensions.length > 0) {
      sections.push({
        content: m(
          LabeledControl,
          {label: 'Dimensions:'},
          m(
            '.pf-metrics-v2-dimensions-info',
            dimensions.map((d, i) => [
              ColumnName(d),
              i < dimensions.length - 1 ? ', ' : '',
            ]),
          ),
        ),
      });
    }

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
      info: 'Configure a trace-based metric. Select a numeric value column - all other columns become dimensions automatically. Use a Modify Columns node before this to control which columns are included.',
      sections,
    };
  }

  private renderValueControls(): m.Children {
    // Only show numeric columns
    const numericColumns = this.state.availableColumns.filter((c) =>
      isNumericType(c.type),
    );

    const columnOptions = numericColumns.map((col) =>
      m(
        'option',
        {
          value: col.name,
          selected: this.state.valueColumn === col.name,
        },
        `${col.name} (${col.type})`,
      ),
    );

    const needsCustomUnit = this.state.unit === 'CUSTOM';

    return [
      // Column selector
      m(
        OutlinedField,
        {
          label: 'Value column',
          value: this.state.valueColumn ?? '',
          onchange: (e: Event) => {
            const selectedValue = (e.target as HTMLSelectElement).value;
            this.state.valueColumn = selectedValue || undefined;
            this.state.onchange?.();
          },
        },
        [
          m('option', {value: '', disabled: true}, 'Select numeric column...'),
          ...columnOptions,
        ],
      ),
      // Unit selector
      m(
        OutlinedField,
        {
          label: 'Unit',
          value: this.state.unit,
          onchange: (e: Event) => {
            const newUnit = (e.target as HTMLSelectElement).value;
            this.state.unit = newUnit;
            // Clear custom unit if not using custom
            if (newUnit !== 'CUSTOM') {
              this.state.customUnit = undefined;
            }
            this.state.onchange?.();
          },
        },
        getMetricUnitOptions().map((u) =>
          m(
            'option',
            {value: u.value, selected: this.state.unit === u.value},
            u.label,
          ),
        ),
      ),
      // Custom unit input (conditionally shown)
      needsCustomUnit
        ? m(OutlinedField, {
            label: 'Custom unit',
            value: this.state.customUnit ?? '',
            placeholder: 'Enter custom unit...',
            oninput: (e: Event) => {
              this.state.customUnit = (e.target as HTMLInputElement).value;
              this.state.onchange?.();
            },
          })
        : undefined,
      // Polarity selector
      m(
        OutlinedField,
        {
          label: 'Polarity',
          value: this.state.polarity,
          onchange: (e: Event) => {
            this.state.polarity = (e.target as HTMLSelectElement).value;
            this.state.onchange?.();
          },
        },
        getPolarityOptions().map((p) =>
          m(
            'option',
            {value: p.value, selected: this.state.polarity === p.value},
            p.label,
          ),
        ),
      ),
    ];
  }

  private async showExportModal(): Promise<void> {
    const templateSpec = this.getMetricTemplateSpec();
    if (templateSpec === undefined) {
      return;
    }

    // Build a self-contained query tree for the template. The
    // summarizeTrace API materializes shared queries as standalone tables,
    // which breaks when those queries contain nested innerQueryId
    // references. Embedding resolves this by inlining everything.
    if (this.primaryInput !== undefined) {
      const allQueries = getStructuredQueries(this.primaryInput);
      if (!(allQueries instanceof Error)) {
        const embedded = buildEmbeddedQueryTree(allQueries);
        if (embedded !== undefined) {
          templateSpec.query = embedded;
        }
      }
    }

    const summarySpec = new protos.TraceSummarySpec();
    summarySpec.metricTemplateSpec = [templateSpec];

    const textproto = await traceSummarySpecToText(summarySpec);

    // Result state, updated asynchronously. Store data, not vnodes,
    // so that fresh vnodes are created on each redraw (required by Mithril
    // for stateful components like DataGrid).
    let resultState:
      | {kind: 'loading'}
      | {kind: 'error'; message: string}
      | {kind: 'data'; result: MetricResult} = {kind: 'loading'};
    const metricIdPrefix = this.state.metricIdPrefix;
    const dimensions = this.getDimensions();
    const valueCol = this.state.valueColumn ?? 'value';

    const renderResult = (): m.Children => {
      switch (resultState.kind) {
        case 'loading':
          return m(Spinner);
        case 'error':
          return m('span.pf-metrics-error', resultState.message);
        case 'data':
          return m(DataGrid, {
            data: resultState.result.rows,
            schema: resultState.result.schema,
            rootSchema: resultState.result.metricId,
          });
      }
    };

    showModal({
      title: 'Export Metric',
      className: 'pf-metrics-export-modal',
      content: () =>
        m(
          'div',
          m(CodeSnippet, {
            text: textproto,
            language: 'textproto',
            downloadFileName: `${metricIdPrefix || 'metric'}_spec.pbtxt`,
          }),
          m(
            '.pf-metrics-result-box',
            m('.pf-metrics-result-header', 'Result'),
            m('.pf-metrics-result-content', renderResult()),
          ),
        ),
      buttons: [{text: 'Close'}],
    });

    const engine = this.state.trace?.engine;
    if (engine === undefined) {
      resultState = {
        kind: 'error',
        message: 'No trace engine available. Please load a trace first.',
      };
      m.redraw();
      return;
    }

    const TIMEOUT_MS = 30_000;
    const timeout = new Promise<never>((_, reject) =>
      setTimeout(
        () => reject(new Error('Timed out waiting for trace processor')),
        TIMEOUT_MS,
      ),
    );
    Promise.race([
      engine.summarizeTrace([summarySpec], undefined, undefined, 'proto'),
      timeout,
    ]).then(
      (result) => {
        if (result.error) {
          resultState = {kind: 'error', message: result.error};
        } else if (result.protoSummary) {
          const parsed = parseFirstMetricBundle(
            result.protoSummary,
            metricIdPrefix,
            dimensions,
            valueCol,
          );
          if (parsed !== undefined && parsed.rows.length > 0) {
            resultState = {kind: 'data', result: parsed};
          } else {
            resultState = {kind: 'error', message: 'No metric data found'};
          }
        } else {
          resultState = {kind: 'error', message: 'No results returned'};
        }
        m.redraw();
      },
      (err) => {
        resultState = {kind: 'error', message: String(err)};
        m.redraw();
      },
    );
  }

  nodeInfo(): m.Children {
    return loadNodeDoc('metrics');
  }

  clone(): QueryNode {
    const stateCopy: MetricsNodeState = {
      metricIdPrefix: this.state.metricIdPrefix,
      valueColumn: this.state.valueColumn,
      unit: this.state.unit,
      customUnit: this.state.customUnit,
      polarity: this.state.polarity,
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

    // Wrap the input query to give this node its own ID in the query tree.
    // The explore page uses node IDs to map query results back to nodes,
    // so each node needs a unique ID even if it doesn't transform the SQL.
    const sq = new protos.PerfettoSqlStructuredQuery();
    sq.id = this.nodeId;
    sq.innerQueryId = this.primaryInput.nodeId;
    return sq;
  }

  /**
   * Returns the TraceMetricV2TemplateSpec proto for this metric configuration.
   */
  getMetricTemplateSpec(): protos.TraceMetricV2TemplateSpec | undefined {
    if (!this.validate()) return undefined;
    if (this.primaryInput === undefined) return undefined;

    const valueColumn = this.state.valueColumn;
    if (valueColumn === undefined) return undefined;

    const inputQuery = this.primaryInput.getStructuredQuery();
    if (inputQuery === undefined) return undefined;

    const templateSpec = new protos.TraceMetricV2TemplateSpec();
    templateSpec.idPrefix = this.state.metricIdPrefix;
    templateSpec.dimensions = this.getDimensions();
    templateSpec.query = inputQuery;

    // Set dimension uniqueness
    if (this.state.dimensionUniqueness === 'UNIQUE') {
      templateSpec.dimensionUniqueness =
        protos.TraceMetricV2Spec.DimensionUniqueness.UNIQUE;
    } else {
      templateSpec.dimensionUniqueness =
        protos.TraceMetricV2Spec.DimensionUniqueness.NOT_UNIQUE;
    }

    // Build value column spec
    const valueSpec = new protos.TraceMetricV2TemplateSpec.ValueColumnSpec();
    valueSpec.name = valueColumn;

    // Set unit
    if (this.state.unit === 'CUSTOM' && this.state.customUnit) {
      valueSpec.customUnit = this.state.customUnit;
    } else if (this.state.unit !== 'CUSTOM') {
      const unitEnum =
        protos.TraceMetricV2Spec.MetricUnit[
          this.state.unit as keyof typeof protos.TraceMetricV2Spec.MetricUnit
        ];
      if (unitEnum !== undefined) {
        valueSpec.unit = unitEnum;
      }
    }

    // Set polarity
    const polarityEnum =
      protos.TraceMetricV2Spec.MetricPolarity[
        this.state
          .polarity as keyof typeof protos.TraceMetricV2Spec.MetricPolarity
      ];
    if (polarityEnum !== undefined) {
      valueSpec.polarity = polarityEnum;
    }

    templateSpec.valueColumnSpecs = [valueSpec];

    return templateSpec;
  }

  serializeState(): MetricsSerializedState & {primaryInputId?: string} {
    return {
      primaryInputId: this.primaryInput?.nodeId,
      metricIdPrefix: this.state.metricIdPrefix,
      valueColumn: this.state.valueColumn,
      unit: this.state.unit,
      customUnit: this.state.customUnit,
      polarity: this.state.polarity,
      dimensionUniqueness: this.state.dimensionUniqueness,
    };
  }

  static deserializeState(state: MetricsSerializedState): MetricsNodeState {
    let valueColumn = state.valueColumn;
    let unit = state.unit ?? 'COUNT';
    let customUnit = state.customUnit;
    let polarity = state.polarity ?? 'NOT_APPLICABLE';

    // Handle migration from old multi-value format
    if (valueColumn === undefined && 'values' in state) {
      const values = (
        state as unknown as {
          values?: Array<{
            column?: string;
            unit: string;
            customUnit?: string;
            polarity: string;
          }>;
        }
      ).values;
      if (values !== undefined && values.length > 0) {
        valueColumn = values[0].column;
        unit = values[0].unit;
        customUnit = values[0].customUnit;
        polarity = values[0].polarity;
      }
    }

    // Handle migration from old metricId to metricIdPrefix
    const metricIdPrefix =
      state.metricIdPrefix ??
      (state as unknown as {metricId?: string}).metricId ??
      '';

    return {
      metricIdPrefix,
      valueColumn,
      unit,
      customUnit,
      polarity,
      dimensionUniqueness: state.dimensionUniqueness ?? 'NOT_UNIQUE',
      availableColumns: [],
    };
  }
}
