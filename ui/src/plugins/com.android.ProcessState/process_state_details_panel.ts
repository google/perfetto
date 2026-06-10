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
import type {Trace} from '../../public/trace';
import type {Row} from '../../trace_processor/query_result';
import {NUM} from '../../trace_processor/query_result';
import type {TrackEventDetailsPanel} from '../../public/details_panel';
import type {TrackEventSelection} from '../../public/selection';
import {Button} from '../../widgets/button';
import {Intent} from '../../widgets/common';
import {DetailsShell} from '../../widgets/details_shell';
import {ProcessGraph} from './process_graph';
import type {EdgeSel} from './process_graph';
import {InMemoryDataSource} from '../../components/widgets/datagrid/in_memory_data_source';
import {gridCard} from './grid_helpers';
import {procStateName, schedGroupName, capabilityNames} from './enums';

// Details panel shown when a snapshot slice on the "Process state" timeline
// track is selected. Renders the same interactive process-relationship graph as
// the full-page explorer (so you can peek the wiring inline), plus a button to
// jump to the full explorer at this exact snapshot for the grids + drill-down.
export class ProcessStateDetailsPanel implements TrackEventDetailsPanel {
  private readonly trace: Trace;
  private snapshotId?: number;
  private processes: Row[] = [];
  private reason = 0;
  private selectedPid?: number;
  private selectedEdge?: EdgeSel;
  private nodeRows: Row[] = [];
  private nodeDs?: InMemoryDataSource;
  private edgeRows: Row[] = [];
  private edgeDs?: InMemoryDataSource;
  private edgeNames: Row[] = [];
  private edgeNamesDs?: InMemoryDataSource;
  // Draggable horizontal split: graph width as a fraction of the panel (0..1).
  private graphFrac = 0.62;
  private dragging = false;
  private dragX = 0;
  private dragFrac = 0;
  private splitW = 0;

  constructor(trace: Trace) {
    this.trace = trace;
  }

  async load(sel: TrackEventSelection) {
    // sel.eventId is the slice id, which the track's dataset maps directly to
    // android_process_state_snapshot.id.
    this.snapshotId = sel.eventId;
    this.selectedPid = undefined;
    this.selectedEdge = undefined;
    const meta = await this.trace.engine.query(`
      SELECT oom_adj_reason AS reason
      FROM android_process_state_snapshot WHERE id = ${sel.eventId}`);
    const mit = meta.iter({reason: NUM});
    this.reason = mit.valid() ? mit.reason : 0;
    // Same projection the full page uses, so the graph is identical.
    const q = await this.trace.engine.query(`
      SELECT pid, process_name AS name, uid, cur_adj, adj_type,
             adj_source_pid, cur_proc_state, cur_sched_group, cur_capability,
             is_frozen, persistent, cached_adj
      FROM android_process_state_process
      WHERE snapshot_id = ${sel.eventId}
      ORDER BY cur_adj`);
    const cols = q.columns();
    const it = q.iter({});
    const rows: Row[] = [];
    for (; it.valid(); it.next()) {
      const r: Row = {};
      for (const c of cols) r[c] = it.get(c);
      rows.push(r);
    }
    this.processes = rows;
  }

  render() {
    if (this.snapshotId === undefined) {
      return m(DetailsShell, {title: 'Process state'}, m('span', 'Loading…'));
    }
    const id = this.snapshotId;
    return m(
      DetailsShell,
      {
        title: 'Process state snapshot',
        description: `${this.processes.length} processes · oom_adj_reason ${this.reason}`,
        buttons: m(Button, {
          label: 'Open full explorer ↗',
          intent: Intent.Primary,
          onclick: () => {
            // Deep-link the explorer page to this snapshot id (parsed by
            // ProcessStatePage from its subpage).
            this.trace.navigate(`#!/process_state/${id}`);
          },
        }),
        className: 'pf-ps-detailpanel',
      },
      m(
        '.pf-ps-panelsplit',
        {
          oncreate: (v: m.VnodeDOM) => {
            this.splitW = v.dom.clientWidth;
          },
          onupdate: (v: m.VnodeDOM) => {
            this.splitW = v.dom.clientWidth;
          },
        },
        [
          m(
            '.pf-ps-detailgraph',
            {style: `flex: 0 0 ${(this.graphFrac * 100).toFixed(1)}%`},
            m(ProcessGraph, {
              trace: this.trace,
              processes: this.processes,
              bindingsQuery: id,
              selectedPids:
                this.selectedPid !== undefined
                  ? new Set([this.selectedPid])
                  : undefined,
              // The inline panel keeps single-selection (multi-select lives in the
              // full explorer), so additive shift-clicks just replace.
              onSelect: (pid: number) => {
                this.selectedPid = pid;
                this.selectedEdge = undefined;
                this.buildNode(pid);
                m.redraw();
              },
              onEdgeSelect: (e) => {
                this.selectedEdge = e;
                this.buildEdge(e);
                m.redraw();
              },
              onDeselect: () => {
                this.selectedPid = undefined;
                this.selectedEdge = undefined;
                m.redraw();
              },
            }),
          ),
          m('.pf-ps-vresize', {
            onpointerdown: (e: PointerEvent) => {
              this.dragging = true;
              this.dragX = e.clientX;
              this.dragFrac = this.graphFrac;
              (e.currentTarget as Element).setPointerCapture(e.pointerId);
            },
            onpointermove: (e: PointerEvent) => {
              if (this.dragging && this.splitW > 0) {
                const f =
                  this.dragFrac + (e.clientX - this.dragX) / this.splitW;
                this.graphFrac = Math.max(0.25, Math.min(0.8, f));
              }
            },
            onpointerup: () => {
              this.dragging = false;
            },
          }),
          m('.pf-ps-panelprops', this.renderProps()),
        ],
      ),
    );
  }

  private nameOf(pid: number): string {
    const r = this.processes.find((p) => Number(p['pid']) === pid);
    return r ? String(r['name'] ?? pid).replace(/^.*\//, '') : String(pid);
  }

  private buildNode(pid: number) {
    const p = this.processes.find((r) => Number(r['pid']) === pid);
    const v = (k: string) =>
      !p || p[k] === null || p[k] === undefined ? '—' : String(p[k]);
    this.nodeRows = [
      {property: 'cur adj', value: `${v('cur_adj')} (${v('adj_type')})`},
      {
        property: 'proc state',
        value: p ? procStateName(p['cur_proc_state']) : '—',
      },
      {
        property: 'sched group',
        value: p ? schedGroupName(p['cur_sched_group']) : '—',
      },
      {
        property: 'capability',
        value: p ? capabilityNames(p['cur_capability']) : '—',
      },
      {property: 'frozen', value: p && Number(p['is_frozen']) ? 'yes' : 'no'},
      {property: 'adj source', value: v('adj_source_pid')},
    ];
    this.nodeDs = new InMemoryDataSource(this.nodeRows);
  }

  private buildEdge(e: EdgeSel) {
    this.edgeRows = [
      {
        client_pid: e.from,
        host_pid: e.to,
        connections: e.count,
        foreground: e.fg ? 'yes' : 'no',
      },
    ];
    const col = e.kind === 'provider' ? 'authority' : 'service';
    this.edgeNames = (e.names ? e.names.split(',') : []).map((n) => ({
      [col]: n,
    }));
    this.edgeDs = new InMemoryDataSource(this.edgeRows);
    this.edgeNamesDs = new InMemoryDataSource(this.edgeNames);
  }

  private renderProps(): m.Children {
    const onPid = () => {}; // panel rows aren't navigation targets
    if (this.selectedEdge) {
      const e = this.selectedEdge;
      const nameCol = e.kind === 'provider' ? 'authority' : 'service';
      return m('.pf-ps-detailpane', [
        gridCard(
          e.kind === 'provider'
            ? 'content-provider binding'
            : 'service binding',
          ['client_pid', 'host_pid', 'connections', 'foreground'],
          this.edgeRows,
          this.edgeDs,
          onPid,
        ),
        gridCard(
          e.kind === 'provider' ? 'authorities' : 'services',
          [nameCol],
          this.edgeNames,
          this.edgeNamesDs,
          onPid,
        ),
      ]);
    }
    if (this.selectedPid !== undefined) {
      return m('.pf-ps-detailpane', [
        m('.pf-ps-detail-h', [
          m('span.pf-ps-detail-title', this.nameOf(this.selectedPid)),
          m('span.pf-ps-detail-sub', `pid ${this.selectedPid}`),
        ]),
        gridCard(
          'process state',
          ['property', 'value'],
          this.nodeRows,
          this.nodeDs,
          onPid,
        ),
        m(
          '.pf-ps-none',
          'Open the full explorer for how-this-adj-was-computed & bindings.',
        ),
      ]);
    }
    return m('.pf-ps-none', 'Click a node or an edge for details.');
  }
}
