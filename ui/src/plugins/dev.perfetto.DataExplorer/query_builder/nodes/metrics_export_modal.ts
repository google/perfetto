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
import protos from '../../../../protos';
import {showModal} from '../../../../widgets/modal';
import {CodeSnippet} from '../../../../widgets/code_snippet';
import {traceSummarySpecToText} from '../../../../base/proto_utils_wasm';
import {Spinner} from '../../../../widgets/spinner';
import {DataGrid} from '../../../../components/widgets/datagrid/datagrid';
import {SchemaRegistry} from '../../../../components/widgets/datagrid/datagrid_schema';
import {Row} from '../../../../trace_processor/query_result';
import {Tabs} from '../../../../widgets/tabs';
import {Engine} from '../../../../trace_processor/engine';
import {
  getStructuredQueries,
  buildEmbeddedQueryTree,
} from '../query_builder_utils';
import {ValueColumnConfig} from './metrics_node';
import {QueryNode} from '../../query_node';

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

export interface MetricResult {
  schema: SchemaRegistry;
  rows: Row[];
  metricId: string;
}

/**
 * Parse a specific value column's data from a TraceSummary proto bundle.
 * The bundle contains one row per dimension combination, and each row has
 * one value per value column (in the same order as valueColumnSpecs).
 */
export function parseMetricBundleForValue(
  data: Uint8Array,
  metricIdPrefix: string,
  dimensionNames: string[],
  valueColumnNames: string[],
  valueIndex: number,
): MetricResult | undefined {
  const summary = protos.TraceSummary.decode(data);
  const bundle = summary.metricBundles[0];
  if (bundle === undefined) return undefined;

  const valueColumn = valueColumnNames[valueIndex];
  if (valueColumn === undefined) return undefined;
  const metricId = `${metricIdPrefix}_${valueColumn}`;

  // Build schema columns: dimensions (text) + this value (quantitative).
  const schemaColumns: Record<
    string,
    {title: string; columnType: 'text' | 'quantitative'}
  > = {};
  for (const dim of dimensionNames) {
    schemaColumns[dim] = {title: dim, columnType: 'text'};
  }
  schemaColumns[valueColumn] = {
    title: valueColumn,
    columnType: 'quantitative',
  };

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
    // Extract the value at the specific index for this value column.
    const val = row.values?.[valueIndex];
    rowData[valueColumn] = val ? getMetricValue(val) : null;
    rows.push(rowData);
  }

  return {schema, rows, metricId};
}

type ResultState =
  | {kind: 'loading'}
  | {kind: 'error'; message: string}
  | {kind: 'data'; result: MetricResult};

export interface ShowExportModalArgs {
  templateSpec: protos.TraceMetricV2TemplateSpec;
  primaryInput: QueryNode | undefined;
  metricIdPrefix: string;
  dimensions: string[];
  valueColumns: ValueColumnConfig[];
  engine: Engine | undefined;
}

export async function showMetricsExportModal(
  args: ShowExportModalArgs,
): Promise<void> {
  const {templateSpec, primaryInput, metricIdPrefix, dimensions, valueColumns} =
    args;

  // Build a self-contained query tree for the template.
  if (primaryInput !== undefined) {
    const allQueries = getStructuredQueries(primaryInput);
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

  // Per-value-column result state, updated asynchronously.
  const resultStates = new Map<string, ResultState>();
  for (const vc of valueColumns) {
    resultStates.set(vc.column, {kind: 'loading'});
  }
  // `activeTab` is a plain `let` captured by the `content` closure passed to
  // showModal. This is intentional: Mithril re-calls `content()` on every
  // redraw, so reassigning `activeTab` here (not a stale closure) correctly
  // reflects the new active tab in the next render cycle.
  let activeTab = valueColumns[0]?.column;

  const valueColumnNames = valueColumns.map((vc) => vc.column);

  const renderResultForValue = (columnName: string): m.Children => {
    const state = resultStates.get(columnName);
    if (state === undefined) return null;
    switch (state.kind) {
      case 'loading':
        return m(Spinner);
      case 'error':
        return m('span.pf-metrics-error', state.message);
      case 'data':
        return m(DataGrid, {
          data: state.result.rows,
          schema: state.result.schema,
          rootSchema: state.result.metricId,
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
          valueColumns.length === 1
            ? [
                m('.pf-metrics-result-header', valueColumns[0].column),
                m(
                  '.pf-metrics-result-content',
                  renderResultForValue(valueColumns[0].column),
                ),
              ]
            : m(Tabs, {
                activeTabKey: activeTab,
                onTabChange: (key: string) => {
                  activeTab = key;
                },
                tabs: valueColumns.map((vc) => ({
                  key: vc.column,
                  title: vc.column,
                  content: m(
                    '.pf-metrics-result-content',
                    renderResultForValue(vc.column),
                  ),
                })),
              }),
        ),
      ),
    buttons: [{text: 'Close'}],
  });

  const engine = args.engine;
  if (engine === undefined) {
    for (const vc of valueColumns) {
      resultStates.set(vc.column, {
        kind: 'error',
        message: 'No trace engine available. Please load a trace first.',
      });
    }
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
  // Note: Promise.race does not cancel the losing promise. If summarizeTrace
  // completes after the timeout fires, its result is silently ignored because
  // resultStates is already in the error state and the modal may be closed.
  Promise.race([
    engine.summarizeTrace([summarySpec], undefined, undefined, 'proto'),
    timeout,
  ]).then(
    (result) => {
      if (result.error) {
        for (const vc of valueColumns) {
          resultStates.set(vc.column, {kind: 'error', message: result.error});
        }
      } else if (result.protoSummary) {
        for (let i = 0; i < valueColumns.length; i++) {
          const parsed = parseMetricBundleForValue(
            result.protoSummary,
            metricIdPrefix,
            dimensions,
            valueColumnNames,
            i,
          );
          if (parsed !== undefined && parsed.rows.length > 0) {
            resultStates.set(valueColumns[i].column, {
              kind: 'data',
              result: parsed,
            });
          } else {
            resultStates.set(valueColumns[i].column, {
              kind: 'error',
              message: 'No metric data found',
            });
          }
        }
      } else {
        for (const vc of valueColumns) {
          resultStates.set(vc.column, {
            kind: 'error',
            message: 'No results returned',
          });
        }
      }
      m.redraw();
    },
    (err) => {
      for (const vc of valueColumns) {
        resultStates.set(vc.column, {kind: 'error', message: String(err)});
      }
      m.redraw();
    },
  );
}
