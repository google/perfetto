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
import {
  QueryNode,
  QueryNodeState,
  nextNodeId,
  NodeType,
  SecondaryInputSpec,
} from '../../query_node';
import {ColumnInfo} from '../column_info';
import {NodeIssues} from '../node_issues';
import {NodeModifyAttrs, NodeDetailsAttrs} from '../../node_types';
import {loadNodeDoc} from '../node_doc_loader';
import {
  ColumnName,
  NodeDetailsMessage,
  NodeTitle,
} from '../node_styling_widgets';
import {MetricsNode} from './metrics_node';
import {TraceSummaryResultsPanel} from './trace_summary_results_panel';
import {Accordion} from '../../../../widgets/accordion';
import {enumKeyToLabel} from './metrics_enum_utils';
import {showModal} from '../../../../widgets/modal';
import {CodeSnippet} from '../../../../widgets/code_snippet';
import {traceSummarySpecToText} from '../../../../base/proto_utils_wasm';
import {
  getStructuredQueries,
  buildEmbeddedQueryTree,
} from '../query_builder_utils';

export interface TraceSummarySerializedState {}

export interface TraceSummaryNodeState extends QueryNodeState {}

export class TraceSummaryNode implements QueryNode {
  readonly nodeId: string;
  readonly type = NodeType.kTraceSummary;
  nextNodes: QueryNode[];
  readonly state: TraceSummaryNodeState;
  secondaryInputs: SecondaryInputSpec;

  get finalCols(): ColumnInfo[] {
    return [];
  }

  constructor(state: TraceSummaryNodeState) {
    this.nodeId = nextNodeId();
    this.state = {...state};
    this.nextNodes = [];
    this.secondaryInputs = {
      connections: new Map(),
      min: 1,
      max: 'unbounded',
      portNames: (idx) => `Metric node ${idx + 1}`,
    };
  }

  /**
   * Returns all connected Metrics nodes from secondary inputs, sorted by port.
   */
  getAllMetricsNodes(): MetricsNode[] {
    const nodes: MetricsNode[] = [];
    for (const [, node] of [...this.secondaryInputs.connections.entries()].sort(
      ([a], [b]) => a - b,
    )) {
      if (node.type === NodeType.kMetrics) {
        nodes.push(node as MetricsNode);
      }
    }
    return nodes;
  }

  validate(): boolean {
    if (this.state.issues) {
      this.state.issues.clear();
    }

    // Validate all inputs are Metrics nodes.
    for (const [, node] of this.secondaryInputs.connections) {
      if (node.type !== NodeType.kMetrics) {
        this.setValidationError('All inputs must be Metrics nodes');
        return false;
      }
    }

    // Validate at least one Metrics node is connected.
    const metricsNodes = this.getAllMetricsNodes();
    if (metricsNodes.length === 0) {
      this.setValidationError('At least one Metrics node is required');
      return false;
    }

    // Validate metric ID prefixes are unique.
    const seenPrefixes = new Set<string>();
    for (const metricsNode of metricsNodes) {
      const prefix = metricsNode.state.metricIdPrefix;
      if (prefix && seenPrefixes.has(prefix)) {
        this.setValidationError(
          `Duplicate metric ID prefix '${prefix}'. Each metric must have a unique prefix.`,
        );
        return false;
      }
      if (prefix) {
        seenPrefixes.add(prefix);
      }
    }

    for (const metricsNode of metricsNodes) {
      if (!metricsNode.validate()) {
        this.setValidationError(
          `Metrics node '${metricsNode.state.metricIdPrefix || '(unnamed)'}' is invalid`,
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
    return 'Trace Summary';
  }

  nodeDetails(): NodeDetailsAttrs {
    const details: m.Child[] = [NodeTitle(this.getTitle())];

    const metricsNodes = this.getAllMetricsNodes();
    if (metricsNodes.length === 0) {
      details.push(NodeDetailsMessage('No metrics connected'));
      return {
        content: m('.pf-trace-summary-node-details', details),
      };
    }

    details.push(
      m(
        'div',
        `${metricsNodes.length} metric${metricsNodes.length !== 1 ? 's' : ''}: `,
        metricsNodes.map((mn, i) => [
          ColumnName(mn.state.metricIdPrefix || '(unnamed)'),
          i < metricsNodes.length - 1 ? ', ' : '',
        ]),
      ),
    );

    return {
      content: m('.pf-trace-summary-node-details', details),
    };
  }

  nodeSpecificModify(): NodeModifyAttrs {
    const metricsNodes = this.getAllMetricsNodes();

    const sections: NodeModifyAttrs['sections'] = [];

    if (metricsNodes.length === 0) {
      sections.push({
        content: m(
          '.pf-trace-summary-empty',
          'Connect Metrics nodes to build a trace summary.',
        ),
      });
    } else {
      sections.push({
        content: m(Accordion, {
          items: metricsNodes.map((mn) => ({
            id: mn.nodeId,
            header: this.renderMetricHeader(mn),
            content: this.renderMetricContent(mn),
          })),
        }),
      });
    }

    const canExport = this.validate() && this.state.trace?.engine !== undefined;

    return {
      info: 'Bundle multiple metrics into a single trace summary. Connect Metrics nodes as inputs to include them in the summary.',
      sections,
      bottomRightButtons: [
        {
          label: 'Export',
          icon: 'download',
          onclick: () => this.showExportModal(),
          disabled: !canExport,
        },
      ],
    };
  }

  private renderMetricHeader(mn: MetricsNode): m.Children {
    const prefix = mn.state.metricIdPrefix || '(unnamed)';
    const valueCount = mn.state.valueColumns.length;
    return m('.pf-trace-summary-metric-header', [
      ColumnName(prefix),
      m(
        'span.pf-trace-summary-metric-badge',
        `${valueCount} value${valueCount !== 1 ? 's' : ''}`,
      ),
    ]);
  }

  private renderMetricContent(mn: MetricsNode): m.Children {
    const dimensions = mn.getDimensions();
    const source = mn.primaryInput?.getTitle() ?? '(no source)';

    return m('.pf-trace-summary-metric-detail', [
      m('.pf-trace-summary-detail-row', [
        m('span.pf-trace-summary-detail-label', 'Source'),
        m('span', source),
      ]),
      m('.pf-trace-summary-detail-row', [
        m('span.pf-trace-summary-detail-label', 'Dimensions'),
        dimensions.length > 0
          ? m(
              'span',
              dimensions.map((d, i) => [
                ColumnName(d),
                i < dimensions.length - 1 ? ', ' : '',
              ]),
            )
          : m('span.pf-trace-summary-detail-none', 'none'),
      ]),
      m('.pf-trace-summary-detail-row', [
        m('span.pf-trace-summary-detail-label', 'Values'),
      ]),
      ...mn.state.valueColumns.map((vc) =>
        m('.pf-trace-summary-value-item', [
          ColumnName(vc.column),
          m(
            'span.pf-trace-summary-value-meta',
            `Unit: ${enumKeyToLabel(vc.unit)}`,
          ),
          m(
            'span.pf-trace-summary-value-meta',
            `Polarity: ${enumKeyToLabel(vc.polarity)}`,
          ),
        ]),
      ),
    ]);
  }

  private async showExportModal(): Promise<void> {
    const spec = this.getTraceSummarySpec();
    if (spec === undefined) return;

    // Embed query trees for self-contained export.
    for (const templateSpec of spec.metricTemplateSpec) {
      const metricsNode = this.getAllMetricsNodes().find(
        (mn) => mn.state.metricIdPrefix === templateSpec.idPrefix,
      );
      if (metricsNode?.primaryInput !== undefined) {
        const allQueries = getStructuredQueries(metricsNode.primaryInput);
        if (!(allQueries instanceof Error)) {
          const embedded = buildEmbeddedQueryTree(allQueries);
          if (embedded !== undefined) {
            templateSpec.query = embedded;
          }
        }
      }
    }

    const textproto = await traceSummarySpecToText(spec);

    showModal({
      title: 'Export Trace Summary',
      className: 'pf-trace-summary-export-modal',
      content: () =>
        m(
          'div',
          m(CodeSnippet, {
            text: textproto,
            language: 'textproto',
            downloadFileName: 'trace_summary_spec.pbtxt',
          }),
        ),
      buttons: [{text: 'Close'}],
    });
  }

  nodeInfo(): m.Children {
    return loadNodeDoc('trace_summary');
  }

  clone(): QueryNode {
    return new TraceSummaryNode({
      onchange: this.state.onchange,
    });
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    return undefined;
  }

  customResultsPanel(): m.Children {
    const trace = this.state.trace;
    if (trace === undefined) return undefined;
    return m(TraceSummaryResultsPanel, {
      node: this,
      trace,
      onchange: () => this.state.onchange?.(),
    });
  }

  /**
   * Builds a TraceSummarySpec from all connected Metrics nodes.
   */
  getTraceSummarySpec(): protos.TraceSummarySpec | undefined {
    if (!this.validate()) return undefined;

    const metricsNodes = this.getAllMetricsNodes();
    const templateSpecs: protos.TraceMetricV2TemplateSpec[] = [];

    for (const metricsNode of metricsNodes) {
      const templateSpec = metricsNode.getMetricTemplateSpec();
      if (templateSpec !== undefined) {
        templateSpecs.push(templateSpec);
      }
    }

    if (templateSpecs.length === 0) return undefined;

    const spec = new protos.TraceSummarySpec();
    spec.metricTemplateSpec = templateSpecs;
    return spec;
  }

  serializeState(): TraceSummarySerializedState {
    return {};
  }

  static deserializeState(
    _state: TraceSummarySerializedState,
  ): TraceSummaryNodeState {
    return {};
  }
}
