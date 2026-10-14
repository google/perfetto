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
import type {TrackEventDetailsPanel} from '../../public/details_panel';
import type {TrackEventSelection} from '../../public/selection';
import type {Row} from '../../trace_processor/query_result';
import {DataGrid} from '../../components/widgets/datagrid/datagrid';
import type {InMemoryDataSource} from '../../components/widgets/datagrid/in_memory_data_source';
import {Button} from '../../widgets/button';
import {Select} from '../../widgets/select';
import {DetailsShell} from '../../widgets/details_shell';
import {SplitPanel} from '../../widgets/split_panel';
import {type PassRow, transitionsGridConfig} from './aggregators';
import {ProcessGraph} from './process_graph';
import {gridCard, gridSchema} from './grid_helpers';
import type {
  ProcessStateController,
  SliceContext,
} from './process_state_controller';

// Process-list columns shown by default (all already display-ready strings/ints
// from the importer — no enum mapping needed here).
const DEFAULT_PROC_COLS = [
  'pid',
  'name',
  'uid',
  'oom_score',
  'proc_state',
  'capabilities',
  'persistent',
];

// Above this fraction of the viewport height the drawer is "tall" (close to full
// page): stack the graph above the details, like HeapDumpExplorer. Below it the
// drawer is short: lay them out side-by-side.
const VERTICAL_THRESHOLD = 0.5;

// A diffed cell is encoded as "old → new"; render it amber. Everything else is
// shown as-is (values already arrive as display strings).
function deltaRenderer(value: unknown): m.Children {
  if (typeof value === 'string' && value.includes(' → ')) {
    return m('span.pf-ps-diff-changed', value);
  }
  return value === null || value === undefined ? '' : String(value);
}

// The whole explorer, in the timeline details panel (there is no separate page).
// Snapshot nav + diff controls sit in the shell header; the graph and a tabbed
// detail pane (Transitions / Triggers / Selected / Process list, all binding tables)
// fill a SplitPanel whose orientation is responsive — side-by-side while the
// drawer is short, stacked once it is taller than half the viewport. All state
// lives in the shared ProcessStateController.
export class ProcessStateDetailsPanel implements TrackEventDetailsPanel {
  private readonly c: ProcessStateController;
  private readonly initialSlice?: SliceContext;
  private readonly initialPass?: PassRow;

  constructor(
    controller: ProcessStateController,
    initialSlice?: SliceContext,
    initialPass?: PassRow,
  ) {
    this.c = controller;
    this.initialSlice = initialSlice;
    this.initialPass = initialPass;
  }

  private get splitPercent(): number {
    return this.c.vertical
      ? this.c.splitPercentVertical
      : this.c.splitPercentHorizontal;
  }

  private set splitPercent(v: number) {
    if (this.c.vertical) {
      this.c.splitPercentVertical = v;
    } else {
      this.c.splitPercentHorizontal = v;
    }
  }

  private get lastOpenPercent(): number {
    return this.c.vertical
      ? this.c.lastOpenPercentVertical
      : this.c.lastOpenPercentHorizontal;
  }

  private set lastOpenPercent(v: number) {
    if (this.c.vertical) {
      this.c.lastOpenPercentVertical = v;
    } else {
      this.c.lastOpenPercentHorizontal = v;
    }
  }

  async load(sel: TrackEventSelection) {
    if (this.initialSlice !== undefined) {
      await this.c.loadForProcessSlice(this.initialSlice);
      return;
    }
    if (this.initialPass !== undefined) {
      await this.c.loadForPass(this.initialPass);
      return;
    }
    await this.c.ensureLoaded(sel.eventId);
  }

  // Pick the split orientation from the drawer's height. Stable across the flip
  // (the box keeps the same height either way), so it doesn't oscillate.
  private measure(dom: HTMLElement) {
    const v = dom.clientHeight > window.innerHeight * VERTICAL_THRESHOLD;
    if (v !== this.c.vertical) {
      this.c.vertical = v;
      m.redraw();
    }
  }

  private expandPropertiesIfCollapsed() {
    if (this.splitPercent >= 88) {
      this.splitPercent = this.lastOpenPercent < 85 ? this.lastOpenPercent : 55;
    }
  }

  private togglePropertiesPanel() {
    if (this.splitPercent >= 88) {
      this.splitPercent = this.lastOpenPercent < 85 ? this.lastOpenPercent : 55;
    } else {
      this.lastOpenPercent = this.splitPercent;
      this.splitPercent = 92;
    }
  }

  render() {
    const c = this.c;
    if (c.snapshotId === undefined) {
      return m(
        DetailsShell,
        {title: 'OomAdjuster pass'},
        m('span', 'Loading…'),
      );
    }
    const i = c.snapshots.findIndex((s) => s.id === c.snapshotId);
    const n = c.snapshots.length;
    const snap = c.snapshotOf(c.snapshotId);
    const reason = snap?.reason ?? this.initialPass?.reason ?? 'N/A';
    const seqStr = snap?.seqId !== undefined ? ` · seq #${snap.seqId}` : '';
    const sliceDesc =
      c.sliceContext !== undefined
        ? ` · ${c.sliceContext.processName ?? c.sliceContext.pid}: ${
            c.sliceContext.prevState ? `${c.sliceContext.prevState} → ` : ''
          }${c.sliceContext.state}`
        : '';
    return m(
      DetailsShell,
      {
        title: 'OomAdjuster pass',
        description: `${reason}${seqStr} · Pass #${i + 1}/${n} · ${c.transitionRows.length} transition${c.transitionRows.length === 1 ? '' : 's'} · ${c.processes.length} procs${sliceDesc}`,
        buttons: this.renderControls(i, n),
        className: 'pf-ps-detailpanel',
      },
      m(
        '.pf-ps-splitwrap',
        {
          oncreate: (v: m.VnodeDOM) => this.measure(v.dom as HTMLElement),
          onupdate: (v: m.VnodeDOM) => this.measure(v.dom as HTMLElement),
        },
        m(SplitPanel, {
          direction: c.vertical ? 'vertical' : 'horizontal',
          split: {percent: this.splitPercent},
          minSize: 36,
          onResize: (pct) => {
            this.splitPercent = pct;
            if (pct > 12 && pct < 85) {
              this.lastOpenPercent = pct;
            }
          },
          firstPanel: m(ProcessGraph, {
            trace: c.trace,
            processes: c.graphProcesses,
            bindingsQuery: c.snapshotId,
            diffNodes: c.diffOn ? c.diffNodes : undefined,
            diffBaseline:
              c.diffOn && c.baselineId !== undefined ? c.baselineId : undefined,
            selectedPids:
              c.selectedPid !== undefined
                ? new Set([c.selectedPid])
                : undefined,
            selectedEdge: c.selectedEdge,
            onSelect: (pid: number) => {
              this.expandPropertiesIfCollapsed();
              c.select(pid);
            },
            onEdgeSelect: (e) => {
              this.expandPropertiesIfCollapsed();
              c.selectEdge(e);
            },
            onDeselect: () => c.deselect(),
          }),
          secondPanel: m('.pf-ps-bottom', [
            m('.pf-ps-tabs', [
              this.tabButton(
                'transitions',
                `Transitions (${c.transitionRows.length})`,
              ),
              this.tabButton('triggers', `Triggers (${c.triggerRows.length})`),
              this.tabButton('current', 'Selected'),
              this.tabButton('procs', `Process list (${c.processes.length})`),
            ]),
            c.tab === 'transitions'
              ? m('.pf-ps-tabbody', this.renderTransitions())
              : c.tab === 'procs'
                ? m('.pf-ps-tabbody', this.renderProcessList())
                : c.tab === 'triggers'
                  ? m(
                      '.pf-ps-tabbody.pf-ps-tabbody--scroll',
                      this.renderTriggers(),
                    )
                  : m(
                      '.pf-ps-tabbody.pf-ps-tabbody--scroll',
                      this.renderCurrent(),
                    ),
          ]),
        }),
      ),
    );
  }

  // Snapshot nav (prev/next, which re-select the matching instant on the "By reason"
  // track so the highlight stays in sync) + diff toggle + baseline picker.
  private renderControls(i: number, n: number): m.Children {
    const c = this.c;
    const propsCollapsed = this.splitPercent >= 88;
    const go = (idx: number) => {
      const s = c.snapshots[idx];
      if (s !== undefined) {
        if (c.sliceContext !== undefined) {
          c.setSnapshot(s.id, c.selectedPid).catch((e) =>
            console.error('ProcessState', e),
          );
        } else {
          c.goToSnapshot(s.id);
        }
      }
    };
    return [
      c.sliceContext !== undefined &&
        c.snapshotId !== undefined &&
        m(Button, {
          label: 'Select on By reason',
          icon: 'timeline',
          compact: true,
          title: 'Select this OomAdjuster pass instant on the By reason track',
          onclick: () =>
            c.snapshotId !== undefined && c.goToSnapshot(c.snapshotId),
        }),
      m(Button, {
        icon: 'chevron_left',
        compact: true,
        disabled: i <= 0,
        title: 'Previous OomAdjuster pass',
        onclick: () => go(i - 1),
      }),
      m(Button, {
        icon: 'chevron_right',
        compact: true,
        disabled: i < 0 || i >= n - 1,
        title: 'Next OomAdjuster pass',
        onclick: () => go(i + 1),
      }),
      n >= 2 &&
        m(Button, {
          label: 'Diff',
          icon: 'difference',
          compact: true,
          active: c.diffOn,
          title: 'Highlight what changed vs a baseline pass',
          onclick: () => c.toggleDiff(),
        }),
      n >= 2 &&
        c.diffOn &&
        m(
          Select,
          {
            title: 'Baseline to compare against',
            onchange: (e: Event) => {
              const v = (e.target as HTMLSelectElement).value;
              if (v === 'prev') c.followPrevBaseline();
              else c.setBaseline(Number(v));
            },
          },
          [
            m(
              'option',
              {value: 'prev', selected: c.baselineFollowsPrev},
              'vs previous (auto)',
            ),
            ...c.snapshots
              .filter((s) => s.id !== c.snapshotId)
              .map((s) =>
                m(
                  'option',
                  {
                    value: s.id,
                    selected: !c.baselineFollowsPrev && s.id === c.baselineId,
                  },
                  `vs #${c.snapshots.findIndex((x) => x.id === s.id) + 1}${
                    s.seqId !== undefined ? ` (seq #${s.seqId})` : ''
                  }`,
                ),
              ),
          ],
        ),
      m(Button, {
        label: 'Properties',
        icon: c.vertical ? 'bottom_panel_open' : 'right_panel_open',
        compact: true,
        active: !propsCollapsed,
        title: propsCollapsed
          ? 'Expand properties panel'
          : 'Minimize properties panel to header',
        onclick: () => this.togglePropertiesPanel(),
      }),
    ];
  }

  private tabButton(
    tab: 'transitions' | 'triggers' | 'current' | 'procs',
    label: string,
  ): m.Children {
    return m(
      'button.pf-ps-tab',
      {
        className: this.c.tab === tab ? 'pf-ps-tab--on' : '',
        onclick: () => {
          this.expandPropertiesIfCollapsed();
          this.c.setTab(tab);
        },
      },
      label,
    );
  }

  private renderTransitions(): m.Children {
    const c = this.c;
    if (c.transitionDs === undefined) return m('.pf-ps-none', 'Loading…');
    if (c.transitionRows.length === 0 && c.triggerRows.length > 0) {
      return this.renderTriggers();
    }
    const {schema, initialColumns} = transitionsGridConfig(c.trace, (pid) =>
      c.select(pid),
    );
    return m(DataGrid, {
      fillHeight: true,
      schema,
      initialColumns,
      columns: c.transitionColumns,
      data: c.transitionDs,
      onColumnsChanged: (cols) => {
        c.transitionColumns = cols;
      },
    });
  }

  private renderTriggers(): m.Children {
    const c = this.c;
    const onPid = (pid: number) => c.select(pid);
    return m('.pf-ps-detailpane', [
      gridCard(
        'oom adjuster pass summary',
        ['property', 'value'],
        c.passInfoRows,
        c.passInfoDs,
        onPid,
      ),
      gridCard(
        'causal trigger events (what triggered this oom adjust pass)',
        [
          'kind',
          'action',
          'component',
          'target_pid',
          'caller_pid',
          'detail',
          'service_id',
          'bind_id',
          'intent_bind_id',
          'provider_id',
          'ts',
        ],
        c.triggerRows,
        c.triggerDs,
        onPid,
      ),
      gridCard(
        'processes changed in this pass (snapshot diff)',
        ['pid', 'name', 'change', 'proc_state', 'oom_score', 'capabilities'],
        c.passChangeRows,
        c.passChangeDs,
        onPid,
      ),
    ]);
  }

  private renderProcessList(): m.Children {
    const c = this.c;
    if (c.procDs === undefined) return m('.pf-ps-none', 'Loading…');
    const visible =
      c.procColumns ??
      DEFAULT_PROC_COLS.filter((x) => c.procCols.includes(x)).map((x) => ({
        id: x,
        field: x,
      }));
    // In diff mode, the changed columns carry "old → new" strings; colour them.
    const renderers = c.diffOn
      ? {
          oom_score: deltaRenderer,
          proc_state: deltaRenderer,
          capabilities: deltaRenderer,
        }
      : undefined;
    return m(DataGrid, {
      schema: gridSchema(c.procCols, (pid) => c.select(pid), renderers),
      data: c.procDs,
      fillHeight: true,
      columns: visible,
      onColumnsChanged: (cols) => {
        c.procColumns = cols;
      },
    });
  }

  private renderCurrent(): m.Children {
    const c = this.c;
    const onPid = (pid: number) => c.select(pid);
    if (c.selectedEdge) {
      const e = c.selectedEdge;
      const provCols = [
        'client_pid',
        'client_name',
        'host_pid',
        'host_name',
        'authority',
        'component_name',
        'stability',
        'connections',
      ];
      const svcCols = [
        'client_pid',
        'client_name',
        'host_pid',
        'host_name',
        'service',
        'foreground',
        'flags',
        'intent_action',
        'connections',
      ];
      const hasProv = c.edgeProvRows.length > 0;
      const hasSvc = c.edgeSvcRows.length > 0;
      if (hasProv || hasSvc) {
        const provCard = gridCard(
          'content-provider bindings',
          provCols,
          c.edgeProvRows,
          c.edgeProvDs,
          onPid,
        );
        const svcCard = gridCard(
          'service bindings',
          svcCols,
          c.edgeSvcRows,
          c.edgeSvcDs,
          onPid,
        );
        if (e.kind === 'provider') {
          return m('.pf-ps-detailpane', [provCard, hasSvc && svcCard]);
        }
        return m('.pf-ps-detailpane', [svcCard, hasProv && provCard]);
      }
      const nameCol = e.kind === 'provider' ? 'authority' : 'service';
      return m('.pf-ps-detailpane', [
        gridCard(
          e.kind === 'provider'
            ? 'content-provider binding'
            : 'service binding',
          [
            'client_pid',
            'client_name',
            'host_pid',
            'host_name',
            'connections',
            'foreground',
          ],
          c.edgeRows,
          c.edgeDs,
          onPid,
        ),
        gridCard(
          e.kind === 'provider' ? 'authorities' : 'services',
          [nameCol],
          c.edgeNames,
          c.edgeNamesDs,
          onPid,
        ),
      ]);
    }
    if (c.selectedPid === undefined) {
      return m(
        '.pf-ps-none',
        'Click a node or an edge in the graph (or a row in Triggers / Process list).',
      );
    }
    const pid = c.selectedPid;
    const p = c.graphProcesses.find((r) => Number(r['pid']) === pid);
    return m('.pf-ps-detailpane', [
      m('.pf-ps-detail-h', [
        m('span.pf-ps-detail-title', c.nameOf(pid)),
        m('span.pf-ps-detail-sub', `pid ${pid} · uid ${p?.['uid'] ?? '—'}`),
      ]),
      c.sliceInfoRows.length > 0 &&
        gridCard(
          'selected timeline slice transition',
          ['property', 'value'],
          c.sliceInfoRows,
          c.sliceInfoDs,
          onPid,
        ),
      gridCard(
        'process state in snapshot',
        ['property', 'value'],
        c.stateRows,
        c.stateDs,
        onPid,
      ),
      gridCard(
        'causal trigger events in this pass',
        [
          'kind',
          'action',
          'component',
          'target_pid',
          'caller_pid',
          'detail',
          'service_id',
          'bind_id',
          'provider_id',
        ],
        c.procTriggerRows,
        c.procTriggerDs,
        onPid,
      ),
      this.recordCard(
        'hosted services',
        'hostedSvc',
        c.hostedSvcCols,
        c.hostedSvc,
        c.hostedSvcDs,
      ),
      this.recordCard(
        'hosted providers',
        'hostedProv',
        c.hostedProvCols,
        c.hostedProv,
        c.hostedProvDs,
      ),
      gridCard(
        'outgoing bindings',
        ['pid', 'kind', 'name', 'fg', 'n'],
        c.outAll,
        c.outDs,
        onPid,
      ),
      gridCard(
        'incoming bindings',
        ['pid', 'kind', 'name', 'fg', 'n'],
        c.inAll,
        c.inDs,
        onPid,
      ),
      gridCard(
        'self bindings',
        ['kind', 'name', 'fg', 'n'],
        c.selfAll,
        c.selfDs,
        onPid,
      ),
    ]);
  }

  private recordCard(
    title: string,
    key: string,
    allCols: string[],
    rows: ReadonlyArray<Row>,
    ds?: InMemoryDataSource,
  ): m.Children {
    if (!rows.length || ds === undefined) {
      return m('.pf-ps-card', [
        m('.pf-ps-card-h', title),
        m('.pf-ps-card-b', m('.pf-ps-none', '— none —')),
      ]);
    }
    const visible =
      this.c.recordColumns[key] ?? allCols.map((x) => ({id: x, field: x}));
    return m('.pf-ps-card', [
      m('.pf-ps-card-h', title),
      m(DataGrid, {
        schema: gridSchema(allCols, (pid) => this.c.select(pid)),
        data: ds,
        columns: visible,
        onColumnsChanged: (cols) => {
          this.c.recordColumns[key] = cols;
        },
      }),
    ]);
  }
}
