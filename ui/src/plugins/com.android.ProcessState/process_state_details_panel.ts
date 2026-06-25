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
import type {Column} from '../../components/widgets/datagrid/model';
import type {InMemoryDataSource} from '../../components/widgets/datagrid/in_memory_data_source';
import {Button} from '../../widgets/button';
import {Select} from '../../widgets/select';
import {DetailsShell} from '../../widgets/details_shell';
import {SplitPanel} from '../../widgets/split_panel';
import {ProcessGraph} from './process_graph';
import {gridCard, gridSchema} from './grid_helpers';
import type {ProcessStateController} from './process_state_controller';

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
// detail pane (Current / Process list, all binding tables) fill a SplitPanel
// whose orientation is responsive — side-by-side while the drawer is short,
// stacked once it is taller than half the viewport. All state lives in the
// shared ProcessStateController.
export class ProcessStateDetailsPanel implements TrackEventDetailsPanel {
  private readonly c: ProcessStateController;
  private vertical = false;
  // Per-grid persisted visible columns.
  private procColumns?: ReadonlyArray<Column>;
  private recordColumns: {[key: string]: ReadonlyArray<Column>} = {};

  constructor(controller: ProcessStateController) {
    this.c = controller;
  }

  async load(sel: TrackEventSelection) {
    // The slice id IS the snapshot id; point the shared controller at it.
    await this.c.ensureLoaded(sel.eventId);
  }

  // Pick the split orientation from the drawer's height. Stable across the flip
  // (the box keeps the same height either way), so it doesn't oscillate.
  private measure(dom: HTMLElement) {
    const v = dom.clientHeight > window.innerHeight * VERTICAL_THRESHOLD;
    if (v !== this.vertical) {
      this.vertical = v;
      m.redraw();
    }
  }

  render() {
    const c = this.c;
    if (c.snapshotId === undefined) {
      return m(DetailsShell, {title: 'Process state'}, m('span', 'Loading…'));
    }
    const i = c.snapshots.findIndex((s) => s.id === c.snapshotId);
    const n = c.snapshots.length;
    const reason = c.reasonOf(c.snapshotId) ?? 'snapshot';
    return m(
      DetailsShell,
      {
        title: 'Process state',
        description: `#${i + 1}/${n} · ${reason} · ${c.processes.length} procs`,
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
          direction: this.vertical ? 'vertical' : 'horizontal',
          initialSplit: {percent: 55},
          minSize: 120,
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
            onSelect: (pid: number) => c.select(pid),
            onEdgeSelect: (e) => c.selectEdge(e),
            onDeselect: () => c.deselect(),
          }),
          secondPanel: m('.pf-ps-bottom', [
            m('.pf-ps-tabs', [
              this.tabButton('current', 'Current'),
              this.tabButton('procs', 'Process list'),
            ]),
            c.tab === 'procs'
              ? m('.pf-ps-tabbody', this.renderProcessList())
              : m('.pf-ps-tabbody.pf-ps-tabbody--scroll', this.renderCurrent()),
          ]),
        }),
      ),
    );
  }

  // Snapshot nav (prev/next, which re-select the matching timeline slice so the
  // highlight stays in sync) + diff toggle + baseline picker.
  private renderControls(i: number, n: number): m.Children {
    const c = this.c;
    const go = (idx: number) => {
      const s = c.snapshots[idx];
      if (s !== undefined) c.goToSnapshot(s.id);
    };
    return [
      m(Button, {
        icon: 'chevron_left',
        compact: true,
        disabled: i <= 0,
        title: 'Previous snapshot',
        onclick: () => go(i - 1),
      }),
      m(Button, {
        icon: 'chevron_right',
        compact: true,
        disabled: i < 0 || i >= n - 1,
        title: 'Next snapshot',
        onclick: () => go(i + 1),
      }),
      n >= 2 &&
        m(Button, {
          label: 'Diff',
          icon: 'difference',
          compact: true,
          active: c.diffOn,
          title: 'Highlight what changed vs a baseline snapshot',
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
                  `vs #${c.snapshots.findIndex((x) => x.id === s.id) + 1}`,
                ),
              ),
          ],
        ),
    ];
  }

  private tabButton(tab: 'current' | 'procs', label: string): m.Children {
    return m(
      'button.pf-ps-tab',
      {
        className: this.c.tab === tab ? 'pf-ps-tab--on' : '',
        onclick: () => this.c.setTab(tab),
      },
      label,
    );
  }

  private renderProcessList(): m.Children {
    const c = this.c;
    if (c.procDs === undefined) return m('.pf-ps-none', 'Loading…');
    const visible =
      this.procColumns ??
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
      rootSchema: 'root',
      data: c.procDs,
      fillHeight: true,
      columns: visible,
      onColumnsChanged: (cols) => {
        this.procColumns = cols;
      },
    });
  }

  private renderCurrent(): m.Children {
    const c = this.c;
    const onPid = (pid: number) => c.select(pid);
    if (c.selectedEdge) {
      const e = c.selectedEdge;
      const nameCol = e.kind === 'provider' ? 'authority' : 'service';
      return m('.pf-ps-detailpane', [
        gridCard(
          e.kind === 'provider'
            ? 'content-provider binding'
            : 'service binding',
          ['client_pid', 'host_pid', 'connections', 'foreground'],
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
        'Click a node or an edge in the graph (or a row in Process list).',
      );
    }
    const pid = c.selectedPid;
    const p = c.graphProcesses.find((r) => Number(r['pid']) === pid);
    return m('.pf-ps-detailpane', [
      m('.pf-ps-detail-h', [
        m('span.pf-ps-detail-title', c.nameOf(pid)),
        m('span.pf-ps-detail-sub', `pid ${pid} · uid ${p?.['uid'] ?? '—'}`),
      ]),
      gridCard(
        'process state',
        ['property', 'value'],
        c.stateRows,
        c.stateDs,
        onPid,
      ),
      this.recordCard(
        'hosted services',
        'hostedSvc',
        c.hostedSvcCols,
        c.hostedSvc,
        c.hostedSvcDs,
      ),
      c.hostedProv.length > 0 &&
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
      this.recordColumns[key] ?? allCols.map((x) => ({id: x, field: x}));
    return m('.pf-ps-card', [
      m('.pf-ps-card-h', title),
      m(DataGrid, {
        schema: gridSchema(allCols, (pid) => this.c.select(pid)),
        rootSchema: 'root',
        data: ds,
        columns: visible,
        onColumnsChanged: (cols) => {
          this.recordColumns[key] = cols;
        },
      }),
    ]);
  }
}
