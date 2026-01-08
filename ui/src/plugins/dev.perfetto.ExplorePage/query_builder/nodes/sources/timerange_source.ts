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
  NodeType,
  nextNodeId,
} from '../../../query_node';
import {
  ColumnInfo,
  columnInfoFromSqlColumn,
  newColumnInfoList,
} from '../../column_info';
import {time, TimeSpan, Time} from '../../../../../base/time';
import {PerfettoSqlTypes} from '../../../../../trace_processor/perfetto_sql_type';
import {Trace} from '../../../../../public/trace';
import {Button, ButtonVariant} from '../../../../../widgets/button';
import {Switch} from '../../../../../widgets/switch';
import {Anchor} from '../../../../../widgets/anchor';
import {StructuredQueryBuilder} from '../../structured_query_builder';
import protos from '../../../../../protos';
import {InlineField} from '../../widgets';
import {Callout} from '../../../../../widgets/callout';
import {NodeIssues} from '../../node_issues';
import {NodeModifyAttrs, NodeDetailsAttrs} from '../../node_explorer_types';
import {loadNodeDoc} from '../../node_doc_loader';
import {NodeTitle} from '../../node_styling_widgets';

// Poll interval for dynamic mode selection updates (in milliseconds)
const SELECTION_POLL_INTERVAL_MS = 200;

export interface TimeRangeSourceSerializedState {
  start?: string;
  end?: string;
  isDynamic?: boolean;
  comment?: string;
}

export interface TimeRangeSourceState extends QueryNodeState {
  start?: time;
  end?: time;
  isDynamic?: boolean;
  trace: Trace;
  onchange?: () => void;
}

export class TimeRangeSourceNode implements QueryNode {
  readonly nodeId: string;
  readonly state: TimeRangeSourceState;
  readonly finalCols: ColumnInfo[];
  nextNodes: QueryNode[] = [];
  private selectionCheckInterval?: number;

  constructor(attrs: TimeRangeSourceState) {
    this.nodeId = nextNodeId();
    this.state = {
      ...attrs,
      isDynamic: attrs.isDynamic ?? false,
    };

    // Initialize columns: id, ts, dur
    this.finalCols = newColumnInfoList(
      [
        columnInfoFromSqlColumn({name: 'id', type: PerfettoSqlTypes.INT}),
        columnInfoFromSqlColumn({name: 'ts', type: PerfettoSqlTypes.TIMESTAMP}),
        columnInfoFromSqlColumn({name: 'dur', type: PerfettoSqlTypes.DURATION}),
      ],
      true,
    );

    // If dynamic mode is enabled, subscribe to selection changes and
    // immediately populate from current selection
    if (this.state.isDynamic) {
      this.subscribeToSelectionChanges();
      this.updateFromSelection();
    }
  }

  get type() {
    return NodeType.kTimeRangeSource;
  }

  validate(): boolean {
    // Initialize issues if not present
    if (!this.state.issues) {
      this.state.issues = new NodeIssues();
    }
    this.state.issues.queryError = undefined;

    if (this.state.start === undefined || this.state.end === undefined) {
      this.state.issues.queryError = new Error(
        'Time range not set. Use "Update from Timeline" or enable Dynamic mode to sync with timeline selection.',
      );
      return false;
    }

    if (this.state.end < this.state.start) {
      this.state.issues.queryError = new Error(
        'End time must be greater than or equal to start time.',
      );
      return false;
    }

    return true;
  }

  clone(): QueryNode {
    const stateCopy: TimeRangeSourceState = {
      start: this.state.start,
      end: this.state.end,
      isDynamic: false, // Clone always creates a static snapshot
      trace: this.state.trace,
      onchange: this.state.onchange,
    };
    return new TimeRangeSourceNode(stateCopy);
  }

  getTitle(): string {
    return this.state.isDynamic ? 'Current time range' : 'Time range';
  }

  nodeDetails(): NodeDetailsAttrs {
    return {
      content: NodeTitle(this.getTitle()),
    };
  }

  serializeState(): TimeRangeSourceSerializedState {
    // Don't serialize start/end for dynamic mode - they'll be populated
    // from the current selection when deserialized
    if (this.state.isDynamic) {
      return {
        isDynamic: true,
      };
    }
    return {
      start: this.state.start?.toString(),
      end: this.state.end?.toString(),
      isDynamic: false,
    };
  }

  static deserializeState(
    trace: Trace,
    serialized: TimeRangeSourceSerializedState,
  ): TimeRangeSourceState {
    return {
      trace,
      start: serialized.start
        ? Time.fromRaw(BigInt(serialized.start))
        : undefined,
      end: serialized.end ? Time.fromRaw(BigInt(serialized.end)) : undefined,
      isDynamic: serialized.isDynamic ?? false,
    };
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    // For dynamic nodes without start/end set, we can still generate a query
    // that uses trace_start() and trace_end() - the backend handles this.
    // Only validate for static nodes or when we have explicit values.
    const start = this.state.start;
    const end = this.state.end;

    // If both are set, calculate duration
    if (start !== undefined && end !== undefined) {
      if (end < start) {
        // Invalid range - can't generate query
        return undefined;
      }
      const dur = end - start;
      return StructuredQueryBuilder.fromTimeRange(start, dur, this.nodeId);
    }

    // If only start is set, let backend calculate dur from trace_end()
    if (start !== undefined) {
      return StructuredQueryBuilder.fromTimeRange(
        start,
        undefined,
        this.nodeId,
      );
    }

    // If only end is set without start, we cannot generate a meaningful query
    if (end !== undefined) {
      return undefined;
    }

    // If neither is set (dynamic node), let backend use trace bounds
    return StructuredQueryBuilder.fromTimeRange(
      undefined,
      undefined,
      this.nodeId,
    );
  }

  getTimeRange(): TimeSpan | undefined {
    if (!this.validate()) {
      return undefined;
    }
    const start = this.state.start;
    const end = this.state.end;
    if (start === undefined || end === undefined) {
      return undefined;
    }
    return new TimeSpan(start, end);
  }

  private subscribeToSelectionChanges() {
    this.selectionCheckInterval = window.setInterval(() => {
      this.updateFromSelection();
    }, SELECTION_POLL_INTERVAL_MS);
  }

  private unsubscribeFromSelectionChanges() {
    if (this.selectionCheckInterval !== undefined) {
      window.clearInterval(this.selectionCheckInterval);
      this.selectionCheckInterval = undefined;
    }
  }

  private updateFromSelection() {
    // Get selection time span, or fall back to full trace if no selection
    let timeSpan = this.state.trace.selection.getTimeSpanOfSelection();
    if (!timeSpan) {
      // No selection - use full trace
      timeSpan = new TimeSpan(
        this.state.trace.traceInfo.start,
        this.state.trace.traceInfo.end,
      );
    }

    // Only update if the values have actually changed to avoid unnecessary redraws
    if (
      this.state.start === timeSpan.start &&
      this.state.end === timeSpan.end
    ) {
      return; // No change needed
    }

    this.state.start = timeSpan.start;
    this.state.end = timeSpan.end;
    this.state.onchange?.();
    m.redraw();
  }

  private toggleDynamicMode() {
    this.state.isDynamic = !this.state.isDynamic;

    if (this.state.isDynamic) {
      this.subscribeToSelectionChanges();
      this.updateFromSelection(); // Immediately sync with current selection
    } else {
      this.unsubscribeFromSelectionChanges();
    }

    this.state.onchange?.();
    m.redraw();
  }

  nodeSpecificModify(): NodeModifyAttrs {
    const isDynamic = this.state.isDynamic ?? false;
    const isValid = this.validate();
    const dur =
      isValid && this.state.start !== undefined && this.state.end !== undefined
        ? this.state.end - this.state.start
        : 0n;
    const error = this.state.issues?.queryError;

    const sections: NodeModifyAttrs['sections'] = [];

    // Error message section
    if (error) {
      sections.push({
        content: m(Callout, {icon: 'error'}, error.message),
      });
    }

    // Mode selection section
    sections.push({
      content: m(
        '.pf-timerange-mode-row',
        m(Switch, {
          checked: isDynamic,
          label: isDynamic ? 'Dynamic (syncs with selection)' : 'Static',
          onchange: () => this.toggleDynamicMode(),
        }),
        !isDynamic &&
          m(Button, {
            label: 'Update from Timeline',
            onclick: () => this.updateFromSelection(),
            variant: ButtonVariant.Outlined,
          }),
      ),
    });

    // Time values section
    sections.push({
      content: m(
        '.pf-timerange-list',
        m(InlineField, {
          label: 'Start (ns)',
          icon: 'start',
          value: this.state.start?.toString() ?? 'Not set',
          editable: !isDynamic,
          placeholder: 'Start timestamp (ns)',
          type: 'number',
          validate: (value: string) => {
            if (value === 'Not set') return true;
            try {
              BigInt(value.trim());
              return true;
            } catch {
              return false;
            }
          },
          errorMessage: 'Must be a valid integer timestamp',
          onchange: (value: string) => {
            try {
              const parsed = BigInt(value.trim());
              this.state.start = Time.fromRaw(parsed);
            } catch (e) {
              // Keep current value if invalid
            }
            this.state.onchange?.();
          },
        }),
        m(InlineField, {
          label: 'End (ns)',
          icon: 'stop',
          value: this.state.end?.toString() ?? 'Not set',
          editable: !isDynamic,
          placeholder: 'End timestamp (ns)',
          type: 'number',
          validate: (value: string) => {
            if (value === 'Not set') return true;
            try {
              BigInt(value.trim());
              return true;
            } catch {
              return false;
            }
          },
          errorMessage: 'Must be a valid integer timestamp',
          onchange: (value: string) => {
            try {
              const parsed = BigInt(value.trim());
              this.state.end = Time.fromRaw(parsed);
            } catch (e) {
              // Keep current value if invalid
            }
            this.state.onchange?.();
          },
        }),
        isValid &&
          m(InlineField, {
            label: 'Duration (ns)',
            icon: 'timelapse',
            value: dur.toString(),
            editable: !isDynamic,
            placeholder: 'Duration (ns)',
            type: 'number',
            validate: (value: string) => {
              try {
                BigInt(value.trim());
                return true;
              } catch {
                return false;
              }
            },
            errorMessage: 'Must be a valid integer duration',
            onchange: (value: string) => {
              try {
                const parsed = BigInt(value.trim());
                if (this.state.start !== undefined) {
                  // Keep start fixed, update end based on duration
                  this.state.end = Time.fromRaw(this.state.start + parsed);
                }
              } catch (e) {
                // Keep current value if invalid
              }
              this.state.onchange?.();
            },
          }),
      ),
    });

    // Build info content based on dynamic mode
    const info = isDynamic
      ? [
          'Time range allows you to make a selection in the ',
          m(Anchor, {href: '#!/viewer'}, 'timeline'),
          ' and use it as a source node in the graph. Dynamic mode: Your timeline selection will automatically update this node. Go back to the timeline and select a time range to see it here.',
        ]
      : [
          'Time range allows you to make a selection in the ',
          m(Anchor, {href: '#!/viewer'}, 'timeline'),
          ' and use it as a source node in the graph.',
        ];

    return {
      info,
      sections,
    };
  }

  nodeInfo(): m.Children {
    // Show general documentation
    const docContent = loadNodeDoc('timerange_source');

    // If valid, also show current time range data
    if (this.validate()) {
      const start = this.state.start;
      const end = this.state.end;
      if (start !== undefined && end !== undefined) {
        const dur = end - start;
        const isDynamic = this.state.isDynamic ?? false;
        const title = isDynamic ? 'Current time selection' : 'Time selection';

        return m(
          'div',
          docContent,
          m(
            '.pf-timerange-current-data',
            m('h2', title),
            m(
              'table.pf-table.pf-table-striped',
              m(
                'thead',
                m(
                  'tr',
                  m('th', 'Column'),
                  m('th', 'Value'),
                  m('th', 'Description'),
                ),
              ),
              m(
                'tbody',
                m(
                  'tr',
                  m('td', 'id'),
                  m('td', '0'),
                  m('td', 'Row identifier (always 0)'),
                ),
                m(
                  'tr',
                  m('td', 'ts'),
                  m('td', start.toString()),
                  m('td', 'Start timestamp (ns)'),
                ),
                m(
                  'tr',
                  m('td', 'dur'),
                  m('td', dur.toString()),
                  m('td', 'Duration (ns)'),
                ),
              ),
            ),
            m(
              '.pf-timerange-info-mode',
              `Mode: ${isDynamic ? 'Dynamic (synced with selection)' : 'Static'}`,
            ),
          ),
        );
      }
    }

    return docContent;
  }

  // Cleanup when node is destroyed. This is called by CleanupManager
  // when the node is deleted from the graph.
  dispose() {
    this.unsubscribeFromSelectionChanges();
  }
}
