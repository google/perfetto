// Copyright (C) 2020 The Android Open Source Project
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
import {errResult, Result, okResult} from '../../base/result';
import {Engine} from '../../trace_processor/engine';
import {STR} from '../../trace_processor/query_result';
import {Select} from '../../widgets/select';
import {Spinner} from '../../widgets/spinner';
import {assertExists, assertUnreachable} from '../../base/assert';
import {Trace} from '../../public/trace';
import {SegmentedButtons} from '../../widgets/segmented_buttons';
import {Editor} from '../../widgets/editor';
import {Button, ButtonVariant} from '../../widgets/button';
import {Intent} from '../../widgets/common';
import {CodeSnippet} from '../../widgets/code_snippet';
import {Callout} from '../../widgets/callout';
import {TextInput} from '../../widgets/text_input';
import {Tabs} from '../../widgets/tabs';
import {DataGrid} from '../../components/widgets/datagrid/datagrid';
import {SchemaRegistry} from '../../components/widgets/datagrid/datagrid_schema';
import {Row} from '../../trace_processor/query_result';
import protos from '../../protos';

type Format = 'json' | 'prototext' | 'proto';
const FORMATS: Format[] = ['json', 'prototext', 'proto'];

// Parsed metric bundle for table display
interface MetricBundle {
  metricId: string;
  schema: SchemaRegistry;
  rows: Row[];
}

// Result type that includes both text and parsed table data
interface MetricV2Result {
  text: string;
  bundles: MetricBundle[];
}

type V2Mode = 'metric-spec' | 'full-trace-summary';

const METRIC_SPEC_EXAMPLE = `id: "memory_per_process"
dimensions: "process_name"
value: "avg_rss_and_swap"
query: {
  table: {
    table_name: "memory_rss_and_swap_per_process"
    module_name: "linux.memory.process"
  }
  group_by: {
    column_names: "process_name"
    aggregates: {
      column_name: "rss_and_swap"
      op: DURATION_WEIGHTED_MEAN
      result_column_name: "avg_rss_and_swap"
    }
  }
}`;

const FULL_TRACE_SUMMARY_EXAMPLE = `# Memory per process using stdlib table
metric_spec: {
  id: "memory_per_process"
  dimensions: "process_name"
  value: "avg_rss"
  unit: BYTES
  query: {
    table: {
      table_name: "memory_rss_and_swap_per_process"
      column_names: "process_name"
      column_names: "rss_and_swap"
    }
    referenced_modules: "linux.memory.process"
    group_by: {
      column_names: "process_name"
      aggregates: {
        column_name: "rss_and_swap"
        op: DURATION_WEIGHTED_MEAN
        result_column_name: "avg_rss"
      }
    }
    limit: 5
  }
}

# Slice stats for Choreographer slices only
metric_spec: {
  id: "choreographer_stats"
  dimensions: "slice_name"
  value: "total_dur"
  unit: TIME_NANOS
  query: {
    simple_slices: {
      slice_name_glob: "Choreographer*"
    }
    group_by: {
      column_names: "slice_name"
      aggregates: {
        column_name: "dur"
        op: SUM
        result_column_name: "total_dur"
      }
    }
  }
}

# Template for system_server slices
metric_template_spec: {
  id_prefix: "system_server"
  dimensions: "slice_name"
  value_columns: "total_dur"
  value_columns: "slice_count"
  query: {
    simple_slices: {
      process_name_glob: "system_server"
    }
    group_by: {
      column_names: "slice_name"
      aggregates: {
        column_name: "dur"
        op: SUM
        result_column_name: "total_dur"
      }
      aggregates: {
        column_name: "id"
        op: COUNT
        result_column_name: "slice_count"
      }
    }
    limit: 10
  }
}`;

function getExampleForMode(mode: V2Mode): string {
  switch (mode) {
    case 'metric-spec':
      return METRIC_SPEC_EXAMPLE;
    case 'full-trace-summary':
      return FULL_TRACE_SUMMARY_EXAMPLE;
    default:
      assertUnreachable(mode);
  }
}

function getDescriptionForMode(mode: V2Mode): string {
  switch (mode) {
    case 'metric-spec':
      return 'Provide metric v2 spec in prototext format';
    case 'full-trace-summary':
      return 'Provide complete trace summary spec (can include multiple metric_spec, query, and metric_template_spec)';
    default:
      assertUnreachable(mode);
  }
}

async function getMetrics(engine: Engine): Promise<string[]> {
  const metrics: string[] = [];
  const metricsResult = await engine.query('select name from trace_metrics');
  for (const it = metricsResult.iter({name: STR}); it.valid(); it.next()) {
    metrics.push(it.name);
  }
  return metrics;
}

async function getMetricV1(
  engine: Engine,
  metric: string,
  format: Format,
): Promise<string> {
  const result = await engine.computeMetric([metric], format);
  if (result instanceof Uint8Array) {
    return `Uint8Array<len=${result.length}>`;
  } else {
    return result;
  }
}

// Helper to extract dimension value as string
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

// Helper to extract value as number or null
function getMetricValue(
  val: protos.TraceMetricV2Bundle.Row.IValue,
): number | null {
  if (val.doubleValue !== undefined && val.doubleValue !== null) {
    return val.doubleValue;
  }
  return null;
}

// Extract dimension names from spec (handles both dimensions and dimensionsSpecs)
function getDimensionNames(
  spec: protos.ITraceMetricV2Spec | null | undefined,
): string[] {
  if (!spec) return [];

  // First check dimensionsSpecs (detailed specs with name field)
  if (spec.dimensionsSpecs && spec.dimensionsSpecs.length > 0) {
    return spec.dimensionsSpecs
      .map((ds) => ds.name)
      .filter((name): name is string => name !== null && name !== undefined);
  }

  // Fall back to simple dimensions array
  return spec.dimensions ?? [];
}

// Parse TraceSummary proto into MetricBundle array
function parseTraceSummary(data: Uint8Array): MetricBundle[] {
  const summary = protos.TraceSummary.decode(data);
  const bundles: MetricBundle[] = [];

  for (const bundle of summary.metricBundles) {
    // Get all specs to find dimension names and value names
    const specs = bundle.specs ?? [];
    const firstSpec = specs[0];
    const metricId = firstSpec?.id ?? bundle.bundleId ?? 'unknown';
    const dimensionNames = getDimensionNames(firstSpec);

    // Get value names from all specs (templates can have multiple value columns)
    const valueNames = specs
      .map((s) => s.value)
      .filter((v): v is string => v !== null && v !== undefined);

    // If no value names found, use default
    if (valueNames.length === 0) {
      valueNames.push('value');
    }

    // Build schema for this metric
    const schemaColumns: Record<
      string,
      {title: string; columnType: 'text' | 'quantitative'}
    > = {};
    for (const dimName of dimensionNames) {
      schemaColumns[dimName] = {title: dimName, columnType: 'text'};
    }
    for (const valueName of valueNames) {
      schemaColumns[valueName] = {title: valueName, columnType: 'quantitative'};
    }

    const schema: SchemaRegistry = {
      [metricId]: schemaColumns,
    };

    // Convert rows to DataGrid format
    const rows: Row[] = [];
    for (const row of bundle.row ?? []) {
      const rowData: Row = {};

      // Add dimensions
      for (let i = 0; i < dimensionNames.length; i++) {
        const dimName = dimensionNames[i];
        const dimValue = row.dimension?.[i];
        rowData[dimName] = dimValue ? getDimensionValue(dimValue) : null;
      }

      // Add all values (templates can have multiple value columns)
      for (let i = 0; i < valueNames.length; i++) {
        const valueName = valueNames[i];
        const val = row.values?.[i];
        rowData[valueName] = val ? getMetricValue(val) : null;
      }

      rows.push(rowData);
    }

    bundles.push({metricId, schema, rows});
  }

  return bundles;
}

async function getMetricV2(
  engine: Engine,
  mode: V2Mode,
  input: string,
  metricIds?: string[],
): Promise<MetricV2Result> {
  let summarySpec: string;

  switch (mode) {
    case 'metric-spec':
      // Wrap input with metric_spec
      summarySpec = `metric_spec: {${input}}`;
      break;
    case 'full-trace-summary':
      // Pass through complete TraceSummarySpec as-is
      summarySpec = input;
      break;
    default:
      assertUnreachable(mode);
  }

  // If metricIds provided and non-empty, use them; otherwise run all
  const idsToRun = metricIds && metricIds.length > 0 ? metricIds : undefined;

  // Request proto format to get binary data we can parse
  const result = await engine.summarizeTrace(
    [summarySpec],
    idsToRun,
    undefined,
    'proto',
  );
  if (result.error) {
    throw new Error(result.error);
  }
  if (!result.protoSummary) {
    throw new Error('No proto summary returned');
  }

  // Parse the proto data
  const bundles = parseTraceSummary(result.protoSummary);

  // Generate text representation (JSON format)
  const summary = protos.TraceSummary.decode(result.protoSummary);
  const text = JSON.stringify(protos.TraceSummary.toObject(summary), null, 2);

  return {text, bundles};
}

class MetricsV1Controller {
  private readonly engine: Engine;
  private _metrics: string[];
  private _selected?: string;
  private _result: Result<string> | 'pending';
  private _format: Format;
  private _json: unknown;

  constructor(trace: Trace) {
    this.engine = trace.engine.getProxy('MetricsPage');
    this._metrics = [];
    this._result = okResult('');
    this._json = {};
    this._format = 'json';
    getMetrics(this.engine).then((metrics) => {
      this._metrics = metrics;
    });
  }

  get metrics(): string[] {
    return this._metrics;
  }

  set selected(metric: string | undefined) {
    if (this._selected === metric) {
      return;
    }
    this._selected = metric;
    this.update();
  }

  get selected(): string | undefined {
    return this._selected;
  }

  set format(format: Format) {
    if (this._format === format) {
      return;
    }
    this._format = format;
    this.update();
  }

  get format(): Format {
    return this._format;
  }

  get result(): Result<string> | 'pending' {
    return this._result;
  }

  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  get resultAsJson(): any {
    return this._json;
  }

  private update() {
    const selected = this._selected;
    const format = this._format;
    if (selected === undefined) {
      this._result = okResult('');
      this._json = {};
    } else {
      this._result = 'pending';
      this._json = {};
      getMetricV1(this.engine, selected, format)
        .then((result) => {
          if (this._selected === selected && this._format === format) {
            this._result = okResult(result);
            if (format === 'json') {
              this._json = JSON.parse(result);
            }
          }
        })
        .catch((e) => {
          if (this._selected === selected && this._format === format) {
            this._result = errResult(e);
            this._json = {};
          }
        });
    }
  }
}

function renderResult(
  result: Result<string> | 'pending' | undefined,
  format: Format,
) {
  if (result === undefined) {
    return null;
  }

  if (result === 'pending') {
    return m(Spinner);
  }

  if (!result.ok) {
    return m(
      Callout,
      {icon: 'error', intent: Intent.Danger},
      `${result.error}`,
    );
  }

  return m(CodeSnippet, {language: format, text: result.value});
}

function renderV2Result(
  result: Result<MetricV2Result> | 'pending' | undefined,
  viewMode: 'table' | 'json',
  onViewModeChange: (mode: 'table' | 'json') => void,
) {
  if (result === undefined) {
    return null;
  }

  if (result === 'pending') {
    return m(Spinner);
  }

  if (!result.ok) {
    return m(
      Callout,
      {icon: 'error', intent: Intent.Danger},
      `${result.error}`,
    );
  }

  const {text, bundles} = result.value;

  return m(
    '.pf-metricsv2-result',
    m(
      '.pf-metricsv2-result__header',
      m(SegmentedButtons, {
        options: [{label: 'Table'}, {label: 'JSON'}],
        selectedOption: viewMode === 'table' ? 0 : 1,
        onOptionSelected: (num) => {
          onViewModeChange(num === 0 ? 'table' : 'json');
        },
      }),
    ),
    viewMode === 'json'
      ? m(CodeSnippet, {language: 'json', text})
      : m(Tabs, {
          className: 'pf-metricsv2-result__tabs',
          tabs: bundles.map((bundle) => ({
            key: bundle.metricId,
            title: bundle.metricId,
            content: m(
              '.pf-metricsv2-result__bundle',
              m(DataGrid, {
                data: bundle.rows,
                schema: bundle.schema,
                rootSchema: bundle.metricId,
              }),
            ),
          })),
        }),
  );
}

interface MetricV1FetcherAttrs {
  controller: MetricsV1Controller;
}

class MetricV1Fetcher implements m.ClassComponent<MetricV1FetcherAttrs> {
  view({attrs}: m.CVnode<MetricV1FetcherAttrs>) {
    const {controller} = attrs;
    return m(
      '.pf-metrics-page-picker',
      m(
        Select,
        {
          value: controller.selected,
          oninput: (e: Event) => {
            if (!e.target) return;
            controller.selected = (e.target as HTMLSelectElement).value;
          },
        },
        controller.metrics.map((metric) =>
          m(
            'option',
            {
              value: metric,
              key: metric,
            },
            metric,
          ),
        ),
      ),
      m(
        Select,
        {
          oninput: (e: Event) => {
            if (!e.target) return;
            controller.format = (e.target as HTMLSelectElement).value as Format;
          },
        },
        FORMATS.map((f) => {
          return m('option', {
            selected: controller.format === f,
            key: f,
            value: f,
            label: f,
          });
        }),
      ),
    );
  }
}

interface MetricV2FetcherAttrs {
  readonly engine: Engine;
  readonly mode: V2Mode;
  readonly showExample: boolean;
  readonly onExecuteRunMetric: (
    result: Result<MetricV2Result> | 'pending',
  ) => void;
  readonly onUpdateText: () => void;
  readonly editorGeneration: number;
}

class MetricV2Fetcher implements m.ClassComponent<MetricV2FetcherAttrs> {
  private text: string = '';
  private metricIdsInput: string = '';
  // Store current attrs so callbacks can access latest values
  // (Editor caches callbacks in oncreate and doesn't update them)
  private currentAttrs?: MetricV2FetcherAttrs;

  private parseMetricIds(): string[] | undefined {
    const trimmed = this.metricIdsInput.trim();
    if (trimmed === '') return undefined;
    return trimmed
      .split(',')
      .map((id) => id.trim())
      .filter((id) => id !== '');
  }

  private runQuery() {
    const currentAttrs = this.currentAttrs;
    if (!currentAttrs) return;
    const metricIds = this.parseMetricIds();
    getMetricV2(currentAttrs.engine, currentAttrs.mode, this.text, metricIds)
      .then((result) => {
        currentAttrs.onExecuteRunMetric(okResult(result));
      })
      .catch((e) => {
        currentAttrs.onExecuteRunMetric(errResult(e));
      });
  }

  view({attrs}: m.CVnode<MetricV2FetcherAttrs>) {
    this.currentAttrs = attrs;
    if (attrs.showExample) {
      this.text = getExampleForMode(attrs.mode);
    }
    return m(
      '.pf-metricsv2-page',
      m(
        '.pf-metricsv2-page__header',
        m(
          Callout,
          {icon: 'info', intent: Intent.Primary},
          getDescriptionForMode(attrs.mode),
        ),
        m(Button, {
          label: 'Run',
          variant: ButtonVariant.Outlined,
          icon: 'play_arrow',
          onclick: () => this.runQuery(),
        }),
      ),
      attrs.mode === 'full-trace-summary' &&
        m(
          '.pf-metricsv2-page__metric-filter',
          m(TextInput, {
            placeholder: 'Metric IDs to run (comma-separated, empty = all)',
            value: this.metricIdsInput,
            onInput: (value: string) => {
              this.metricIdsInput = value;
            },
          }),
        ),
      m(
        '.pf-metricsv2-page__editor',
        m(Editor, {
          text: this.text,
          onExecute: (text: string) => {
            this.text = text;
            this.runQuery();
          },
          onUpdate: (text: string) => {
            if (text === this.text) {
              return;
            }
            this.text = text;
            this.currentAttrs?.onUpdateText();
          },
        }),
      ),
    );
  }
}

export interface MetricsPageAttrs {
  readonly trace: Trace;
}

export class MetricsPage implements m.ClassComponent<MetricsPageAttrs> {
  private v1Controller?: MetricsV1Controller;
  private v2Result?: Result<MetricV2Result> | 'pending';
  private showV2MetricExample: boolean = false;
  private mode: 'V1' | 'V2' = 'V2';
  private v2Mode: V2Mode = 'full-trace-summary';
  private fetcherGeneration: number = 0;
  private v2ViewMode: 'table' | 'json' = 'table';

  oninit({attrs}: m.Vnode<MetricsPageAttrs>) {
    this.v1Controller = new MetricsV1Controller(attrs.trace);
  }

  view({attrs}: m.Vnode<MetricsPageAttrs>) {
    const v1Controller = assertExists(this.v1Controller);
    return m(
      '.pf-metrics-page',
      m(
        '',
        m(SegmentedButtons, {
          options: [{label: 'Metric v1'}, {label: 'Metric v2'}],
          selectedOption: this.mode === 'V1' ? 0 : 1,
          onOptionSelected: (num) => {
            if (num === 0) {
              this.mode = 'V1';
            } else {
              this.mode = 'V2';
            }
          },
        }),
      ),
      this.mode === 'V2' &&
        m(
          '',
          m(SegmentedButtons, {
            options: [{label: 'Metric Spec'}, {label: 'Full Summary'}],
            selectedOption: this.v2Mode === 'metric-spec' ? 0 : 1,
            onOptionSelected: (num) => {
              this.v2Mode = num === 0 ? 'metric-spec' : 'full-trace-summary';
              this.showV2MetricExample = false;
              this.v2Result = undefined;
            },
          }),
        ),
      this.mode === 'V1' &&
        m(MetricV1Fetcher, {
          controller: v1Controller,
        }),
      this.mode === 'V2' && [
        m(Button, {
          label: 'Load example',
          variant: ButtonVariant.Outlined,
          onclick: () => {
            this.showV2MetricExample = true;
            this.fetcherGeneration++;
          },
        }),
        m(MetricV2Fetcher, {
          engine: attrs.trace.engine,
          mode: this.v2Mode,
          showExample: this.showV2MetricExample,
          editorGeneration: this.fetcherGeneration,
          onExecuteRunMetric: (result: Result<MetricV2Result> | 'pending') => {
            this.v2Result = result;
          },
          onUpdateText: () => {
            this.showV2MetricExample = false;
            this.fetcherGeneration++;
          },
        }),
      ],
      this.mode === 'V1' &&
        renderResult(v1Controller.result, v1Controller.format),
      this.mode === 'V2' &&
        renderV2Result(this.v2Result, this.v2ViewMode, (mode) => {
          this.v2ViewMode = mode;
        }),
    );
  }
}
