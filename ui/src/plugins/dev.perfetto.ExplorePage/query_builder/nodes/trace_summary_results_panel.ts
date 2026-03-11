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
import {TraceSummaryNode} from './trace_summary_node';
import {Trace} from '../../../../public/trace';
import {DetailsShell} from '../../../../widgets/details_shell';
import {Button, ButtonVariant} from '../../../../widgets/button';
import {Spinner} from '../../../../widgets/spinner';
import {Switch} from '../../../../widgets/switch';
import {Intent} from '../../../../widgets/common';
import {DataGrid} from '../../../../components/widgets/datagrid/datagrid';
import {SchemaRegistry} from '../../../../components/widgets/datagrid/datagrid_schema';
import {Row} from '../../../../trace_processor/query_result';
import {Tabs} from '../../../../widgets/tabs';
import {
  getStructuredQueries,
  buildEmbeddedQueryTree,
} from '../query_builder_utils';
import {ResultsPanelEmptyState} from '../widgets';

// ============================================================================
// Proto parsing helpers
// ============================================================================

function parseDimensionValue(
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

function parseValueNumber(
  val: protos.TraceMetricV2Bundle.Row.IValue,
): number | null {
  if (val.doubleValue !== undefined && val.doubleValue !== null) {
    return val.doubleValue;
  }
  return null;
}

function extractDimensionNames(
  spec: protos.ITraceMetricV2Spec | null | undefined,
): string[] {
  if (!spec) return [];
  if (spec.dimensionsSpecs && spec.dimensionsSpecs.length > 0) {
    return spec.dimensionsSpecs
      .map((ds) => ds.name)
      .filter((name): name is string => name !== null && name !== undefined);
  }
  return spec.dimensions ?? [];
}

interface MetricBundleResult {
  metricId: string;
  schema: SchemaRegistry;
  rows: Row[];
}

function parseTraceSummaryProto(data: Uint8Array): MetricBundleResult[] {
  const summary = protos.TraceSummary.decode(data);
  const results: MetricBundleResult[] = [];

  for (const bundle of summary.metricBundles) {
    const specs = bundle.specs ?? [];
    const firstSpec = specs[0];
    const dimensionNames = extractDimensionNames(firstSpec);

    // Collect (metricId, valueName, valueIndex) tuples — one per metric tab.
    const metrics: {metricId: string; valueName: string; valueIndex: number}[] =
      [];
    if (specs.length === 0) {
      metrics.push({
        metricId: bundle.bundleId ?? 'unknown',
        valueName: 'value',
        valueIndex: 0,
      });
    } else {
      for (let i = 0; i < specs.length; i++) {
        const spec = specs[i];
        metrics.push({
          metricId: spec.id ?? `${bundle.bundleId ?? 'unknown'}_${i}`,
          valueName: spec.value ?? 'value',
          valueIndex: i,
        });
      }
    }

    // Build one MetricBundleResult per metric (each gets its own tab).
    for (const {metricId, valueName, valueIndex} of metrics) {
      const schemaColumns: Record<
        string,
        {title: string; columnType: 'text' | 'quantitative'}
      > = {};
      for (const dimName of dimensionNames) {
        schemaColumns[dimName] = {title: dimName, columnType: 'text'};
      }
      schemaColumns[valueName] = {
        title: valueName,
        columnType: 'quantitative',
      };

      const schema: SchemaRegistry = {[metricId]: schemaColumns};

      const rows: Row[] = [];
      for (const row of bundle.row ?? []) {
        const rowData: Row = {};
        for (let i = 0; i < dimensionNames.length; i++) {
          const dimValue = row.dimension?.[i];
          rowData[dimensionNames[i]] =
            dimValue !== undefined && dimValue !== null
              ? parseDimensionValue(dimValue)
              : null;
        }
        const val = row.values?.[valueIndex];
        rowData[valueName] =
          val !== undefined && val !== null ? parseValueNumber(val) : null;
        rows.push(rowData);
      }

      results.push({metricId, schema, rows});
    }
  }

  return results;
}

// ============================================================================
// Execution state
// ============================================================================

type ExecutionState =
  | {kind: 'idle'}
  | {kind: 'loading'}
  | {kind: 'error'; message: string}
  | {kind: 'done'; bundles: MetricBundleResult[]; durationMs: number};

// ============================================================================
// Component
// ============================================================================

export interface TraceSummaryResultsPanelAttrs {
  readonly node: TraceSummaryNode;
  readonly trace: Trace;
  readonly onchange?: () => void;
}

export class TraceSummaryResultsPanel
  implements m.ClassComponent<TraceSummaryResultsPanelAttrs>
{
  private executionState: ExecutionState = {kind: 'idle'};
  private activeTab?: string;
  private prevSpecHash?: string;

  view({attrs}: m.CVnode<TraceSummaryResultsPanelAttrs>) {
    const {node} = attrs;
    const autoExecute = node.state.autoExecute ?? true;

    // Auto-execute when spec changes.
    if (autoExecute && node.validate()) {
      const specHash = this.computeSpecHash(node);
      if (specHash !== this.prevSpecHash) {
        this.prevSpecHash = specHash;
        this.execute(attrs);
      }
    }

    return m(
      DetailsShell,
      {
        title: 'Query data',
        buttons: this.renderMenu(attrs),
        fillHeight: true,
      },
      this.renderContent(attrs),
    );
  }

  private computeSpecHash(node: TraceSummaryNode): string {
    const metricsNodes = node.getAllMetricsNodes();
    const parts = metricsNodes.map((mn) => {
      const values = mn.state.valueColumns
        .map((v) => `${v.column}/${v.unit}/${v.customUnit ?? ''}/${v.polarity}`)
        .join(',');
      const dims = mn.getDimensions().join(',');
      return `${mn.nodeId}:${mn.state.metricIdPrefix}:${mn.state.dimensionUniqueness}:${dims}:${values}`;
    });
    return parts.join('|');
  }

  private renderMenu(attrs: TraceSummaryResultsPanelAttrs): m.Children {
    const autoExecute = attrs.node.state.autoExecute ?? true;
    const isLoading = this.executionState.kind === 'loading';
    const isStale = this.prevSpecHash !== this.computeSpecHash(attrs.node);

    const runButton =
      !autoExecute &&
      (isStale || this.executionState.kind === 'idle') &&
      m(Button, {
        label: 'Run Query',
        icon: 'play_arrow',
        intent: Intent.Primary,
        variant: ButtonVariant.Filled,
        disabled: !attrs.node.validate() || isLoading,
        onclick: () => {
          this.prevSpecHash = this.computeSpecHash(attrs.node);
          this.execute(attrs);
        },
      });

    const statusIndicator = isLoading ? m(Spinner) : null;

    const autoExecuteSwitch = m(Switch, {
      label: 'Auto Execute',
      checked: autoExecute,
      onchange: (e: Event) => {
        const target = e.target as HTMLInputElement;
        attrs.node.state.autoExecute = target.checked;
        attrs.onchange?.();
        if (target.checked && attrs.node.validate()) {
          this.prevSpecHash = this.computeSpecHash(attrs.node);
          this.execute(attrs);
        }
      },
    });

    const separator = () =>
      m('span.pf-query-stats-separator', {'aria-hidden': 'true'}, '\u2022');

    const queryStats =
      this.executionState.kind === 'done'
        ? m('.pf-query-stats', [
            m(
              'span',
              `${this.executionState.bundles.reduce((n, b) => n + b.rows.length, 0).toLocaleString()} rows`,
            ),
            separator(),
            m('span', `${this.executionState.durationMs.toFixed(1)}ms`),
          ])
        : null;

    const items = [
      runButton,
      statusIndicator,
      queryStats,
      autoExecuteSwitch,
    ].filter((item) => item !== null && item !== false);

    const menuItems: m.Children = [];
    for (let i = 0; i < items.length; i++) {
      menuItems.push(items[i]);
      if (i < items.length - 1) {
        menuItems.push(separator());
      }
    }

    return menuItems;
  }

  private renderContent(attrs: TraceSummaryResultsPanelAttrs): m.Children {
    if (!attrs.node.validate()) {
      return m(ResultsPanelEmptyState, {
        icon: 'warning',
        title:
          attrs.node.state.issues?.queryError?.message ??
          'Node validation failed',
      });
    }

    switch (this.executionState.kind) {
      case 'idle':
        return m(ResultsPanelEmptyState, {
          icon: 'play_arrow',
          title: 'Click Run or enable Auto Execute',
        });
      case 'loading':
        return m('.pf-trace-summary-loading', m(Spinner));
      case 'error':
        return m(ResultsPanelEmptyState, {
          icon: 'error',
          title: this.executionState.message,
        });
      case 'done': {
        const {bundles} = this.executionState;
        if (bundles.length === 0) {
          return m(ResultsPanelEmptyState, {
            icon: 'info',
            title: 'No metric data returned',
          });
        }
        if (bundles.length === 1) {
          const bundle = bundles[0];
          return m(DataGrid, {
            data: bundle.rows,
            schema: bundle.schema,
            rootSchema: bundle.metricId,
            fillHeight: true,
          });
        }
        return m(Tabs, {
          activeTabKey: this.activeTab,
          onTabChange: (key: string) => {
            this.activeTab = key;
          },
          tabs: bundles.map((bundle) => ({
            key: bundle.metricId,
            title: bundle.metricId,
            content: m(DataGrid, {
              data: bundle.rows,
              schema: bundle.schema,
              rootSchema: bundle.metricId,
              fillHeight: true,
            }),
          })),
        });
      }
    }
  }

  private async execute(attrs: TraceSummaryResultsPanelAttrs): Promise<void> {
    const {node} = attrs;
    if (!node.validate()) return;

    // Guard against concurrent executions (e.g. re-renders during loading).
    if (this.executionState.kind === 'loading') return;

    const engine = node.state.trace?.engine;
    if (engine === undefined) {
      this.executionState = {
        kind: 'error',
        message: 'No trace loaded. Please load a trace first.',
      };
      m.redraw();
      return;
    }

    this.executionState = {kind: 'loading'};
    m.redraw();

    const startTime = performance.now();

    try {
      const metricsNodes = node.getAllMetricsNodes();
      const templateSpecs: protos.TraceMetricV2TemplateSpec[] = [];

      for (const metricsNode of metricsNodes) {
        const templateSpec = metricsNode.getMetricTemplateSpec();
        if (templateSpec === undefined) continue;

        // Build self-contained embedded query trees for summarizeTrace.
        if (metricsNode.primaryInput !== undefined) {
          const allQueries = getStructuredQueries(metricsNode.primaryInput);
          if (!(allQueries instanceof Error)) {
            const embedded = buildEmbeddedQueryTree(allQueries);
            if (embedded !== undefined) {
              templateSpec.query = embedded;
            }
          }
        }
        templateSpecs.push(templateSpec);
      }

      if (templateSpecs.length === 0) {
        this.executionState = {
          kind: 'error',
          message: 'No valid metrics to execute',
        };
        m.redraw();
        return;
      }

      const summarySpec = new protos.TraceSummarySpec();
      summarySpec.metricTemplateSpec = templateSpecs;

      const result = await engine.summarizeTrace(
        [summarySpec],
        undefined,
        undefined,
        'proto',
      );

      const durationMs = performance.now() - startTime;

      if (result.error) {
        this.executionState = {kind: 'error', message: result.error};
      } else if (result.protoSummary) {
        const bundles = parseTraceSummaryProto(result.protoSummary);
        this.executionState = {kind: 'done', bundles, durationMs};
        if (bundles.length > 0) {
          this.activeTab ??= bundles[0].metricId;
        }
      } else {
        this.executionState = {kind: 'error', message: 'No results returned'};
      }
    } catch (e) {
      this.executionState = {kind: 'error', message: String(e)};
    }

    m.redraw();
  }
}
