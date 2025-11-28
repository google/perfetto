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
  createFinalColumns,
} from '../../../query_node';
import {ColumnInfo, columnInfoFromName} from '../../column_info';
import {time, TimeSpan, Time} from '../../../../../base/time';
import {Trace} from '../../../../../public/trace';
import {Button, ButtonVariant} from '../../../../../widgets/button';
import {TextInput} from '../../../../../widgets/text_input';
import {Switch} from '../../../../../widgets/switch';
import {StructuredQueryBuilder} from '../../structured_query_builder';
import protos from '../../../../../protos';
import {ListItem} from '../../widgets';
import {showModal} from '../../../../../widgets/modal';
import {Callout} from '../../../../../widgets/callout';
import {NodeIssues} from '../../node_issues';
import {NodeModifyAttrs} from '../../node_explorer_types';

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
    this.finalCols = createFinalColumns([
      columnInfoFromName('id'),
      columnInfoFromName('ts'),
      columnInfoFromName('dur'),
    ]);

    // If dynamic mode is enabled, subscribe to selection changes
    if (this.state.isDynamic) {
      this.subscribeToSelectionChanges();
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
        'Time range not set. Use "Update from Selection" or enable Dynamic mode to sync with timeline selection.',
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

  serializeState(): TimeRangeSourceSerializedState {
    return {
      start: this.state.start?.toString(),
      end: this.state.end?.toString(),
      isDynamic: this.state.isDynamic,
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

  private static generateSql(start: time, dur: bigint): string {
    return `SELECT 0 AS id, ${start} AS ts, ${dur} AS dur`;
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (!this.validate()) {
      return undefined;
    }

    // Type narrowing - validate() already checked that start and end are defined
    const start = this.state.start;
    const end = this.state.end;
    if (start === undefined || end === undefined) {
      return undefined;
    }

    const dur = end - start;

    const sql = TimeRangeSourceNode.generateSql(start, dur);

    return StructuredQueryBuilder.fromSql(
      sql,
      [], // no dependencies
      ['id', 'ts', 'dur'], // column names
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

  private showEditStartModal(): void {
    let tempValue = this.state.start?.toString() ?? '';

    showModal({
      title: 'Edit Start Time',
      content: () =>
        m(
          'div',
          m(TextInput, {
            value: tempValue,
            oninput: (e: Event) => {
              tempValue = (e.target as HTMLInputElement).value;
            },
            placeholder: 'Start timestamp (ns)',
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
            try {
              const parsed = BigInt(tempValue.trim());
              this.state.start = Time.fromRaw(parsed);
              this.state.onchange?.();
            } catch (e) {
              console.warn('Invalid start timestamp:', tempValue, e);
            }
          },
        },
      ],
    });
  }

  private showEditEndModal(): void {
    let tempValue = this.state.end?.toString() ?? '';

    showModal({
      title: 'Edit End Time',
      content: () =>
        m(
          'div',
          m(TextInput, {
            value: tempValue,
            oninput: (e: Event) => {
              tempValue = (e.target as HTMLInputElement).value;
            },
            placeholder: 'End timestamp (ns)',
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
            try {
              const parsed = BigInt(tempValue.trim());
              this.state.end = Time.fromRaw(parsed);
              this.state.onchange?.();
            } catch (e) {
              console.warn('Invalid end timestamp:', tempValue, e);
            }
          },
        },
      ],
    });
  }

  private showEditDurationModal(): void {
    const currentDur =
      this.state.start && this.state.end
        ? this.state.end - this.state.start
        : 0n;
    let tempValue = currentDur.toString();

    showModal({
      title: 'Edit Duration',
      content: () =>
        m(
          'div',
          m(TextInput, {
            value: tempValue,
            oninput: (e: Event) => {
              tempValue = (e.target as HTMLInputElement).value;
            },
            placeholder: 'Duration (ns)',
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
            try {
              const parsed = BigInt(tempValue.trim());
              if (this.state.start !== undefined) {
                // Keep start fixed, update end based on duration
                this.state.end = Time.fromRaw(this.state.start + parsed);
                this.state.onchange?.();
              }
            } catch (e) {
              console.warn('Invalid duration:', tempValue, e);
            }
          },
        },
      ],
    });
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

    // Dynamic mode helper text section
    if (isDynamic) {
      sections.push({
        content: m(
          '.pf-timerange-dynamic-info',
          'Dynamic mode: Your timeline selection will automatically update this node. Go back to the timeline and select a time range to see it here.',
        ),
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
            label: 'Update from Selection',
            onclick: () => this.updateFromSelection(),
            variant: ButtonVariant.Outlined,
          }),
      ),
    });

    // Time values section
    sections.push({
      content: m(
        '.pf-timerange-list',
        m(ListItem, {
          icon: 'start',
          name: 'Start (ns)',
          description: this.state.start?.toString() ?? 'Not set',
          actions: !isDynamic
            ? [
                {
                  icon: 'edit',
                  onclick: () => this.showEditStartModal(),
                },
              ]
            : undefined,
        }),
        m(ListItem, {
          icon: 'stop',
          name: 'End (ns)',
          description: this.state.end?.toString() ?? 'Not set',
          actions: !isDynamic
            ? [
                {
                  icon: 'edit',
                  onclick: () => this.showEditEndModal(),
                },
              ]
            : undefined,
        }),
        isValid &&
          m(ListItem, {
            icon: 'timelapse',
            name: 'Duration (ns)',
            description: dur.toString(),
            actions: !isDynamic
              ? [
                  {
                    icon: 'edit',
                    onclick: () => this.showEditDurationModal(),
                  },
                ]
              : undefined,
          }),
      ),
    });

    return {sections};
  }

  nodeInfo(): m.Children {
    if (!this.validate()) {
      return m(
        'div',
        m(
          '.pf-node-info-empty',
          'No time range set. Use "Update from Selection" or enter times manually.',
        ),
      );
    }

    const start = this.state.start;
    const end = this.state.end;
    if (start === undefined || end === undefined) {
      return m('div', m('.pf-node-info-empty', 'No time range set.'));
    }

    const dur = end - start;
    const isDynamic = this.state.isDynamic ?? false;
    const title = isDynamic ? 'Current time selection' : 'Time selection';

    return m(
      'div',
      m('h3.pf-timerange-info-title', title),
      m(
        'table.pf-table.pf-table-striped',
        m(
          'thead',
          m('tr', m('th', 'Column'), m('th', 'Value'), m('th', 'Description')),
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
        `Mode: ${this.state.isDynamic ? 'Dynamic (synced with selection)' : 'Static'}`,
      ),
    );
  }

  // Cleanup when node is destroyed. This is called by CleanupManager
  // when the node is deleted from the graph.
  dispose() {
    this.unsubscribeFromSelectionChanges();
  }
}
