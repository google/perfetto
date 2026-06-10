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
import {DataGrid} from '../../components/widgets/datagrid/datagrid';
import type {Column} from '../../components/widgets/datagrid/model';
import {InMemoryDataSource} from '../../components/widgets/datagrid/in_memory_data_source';
import {
  procStateName,
  schedGroupName,
  capabilityNames,
  fgsTypeNames,
  hostingTypeNames,
} from './enums';
import type {Trace} from '../../public/trace';
import type {Row} from '../../trace_processor/query_result';
import {NUM, NUM_NULL, STR_NULL} from '../../trace_processor/query_result';
import {ProcessGraph} from './process_graph';
import type {EdgeSel} from './process_graph';
import {gridSchema, gridCard} from './grid_helpers';

export interface ProcessStatePageAttrs {
  readonly trace: Trace;
  readonly subpage?: string;
}

interface Snapshot {
  readonly id: number;
  readonly ts: bigint;
  readonly reason: number;
  // Device-wide context (GlobalState).
  readonly isAwake: boolean;
  readonly unlocking: boolean;
  readonly shade: boolean;
  readonly memNormal: boolean;
  readonly topProcState: number | null;
  readonly homePid: number | null;
  readonly heavyPid: number | null;
  readonly prevPid: number | null;
  readonly dozingPid: number | null;
  readonly idleAllowlist: string;
}

// Default visible columns for the Processes grid (the rest of the ~78 columns
// are addable via the DataGrid's "Add column" menu).
const DEFAULT_PROC_COLS = [
  'pid',
  'name',
  'uid',
  'cur_adj',
  'adj_type',
  'cur_proc_state',
  'cur_sched_group',
  'cur_capability',
  'is_frozen',
  'persistent',
  'cached_adj',
  'adj_source_pid',
];
// Default visible columns for the per-connection (binding / provider-binding)
// and hosted-service record grids; the rest of each table is addable.
const DEFAULT_BINDING_COLS = [
  'client_pid',
  'service',
  'flag_foreground_service',
  'flag_above_client',
  'flag_include_capabilities',
  'flag_important',
  'effective_proc_state',
  'client_uid',
];
const DEFAULT_PROVBIND_COLS = [
  'client_pid',
  'authority',
  'stable_count',
  'unstable_count',
  'dead',
  'waiting',
];
const DEFAULT_SERVICE_COLS = [
  'short_name',
  'is_foreground',
  'foreground_service_type',
  'start_requested',
  'is_short_fgs',
  'execute_nesting',
  'restart_count',
  'crash_count',
];
const DEFAULT_PROVIDER_COLS = [
  'authority',
  'package_name',
  'external_handle_count',
  'launched',
];
const DEFAULT_UID_COLS = [
  'uid',
  'cur_proc_state',
  'cur_capability',
  'idle',
  'restriction_level',
  'standby_bucket',
];

export class ProcessStatePage
  implements m.ClassComponent<ProcessStatePageAttrs>
{
  private trace!: Trace;
  private snapshots: Snapshot[] = [];
  private idx = 0;
  // Multi-selection of nodes (shift-click toggles). selectedPid mirrors the sole
  // member when exactly one is selected (it drives the single-node detail view).
  private selectedPids = new Set<number>();
  private selectedPid?: number;
  private loadToken = 0;
  // Snapshot id requested via deep link (#!/process_state/<id>), honored once
  // snapshots load. Lets the details-panel "Open full explorer" button land on
  // the same snapshot the user was looking at in the timeline.
  private wantSnapshotId?: number;

  // Cached per-(snapshot,pid) data sources so the grids keep their sort/filter.
  private procRows: Row[] = [];
  private procDs?: InMemoryDataSource;
  private procCols: string[] = [];
  // Visible columns of the Processes grid; undefined = the curated default.
  // Persisted here (controlled) so add/hide/reorder survive tab switches.
  private procColumns?: ReadonlyArray<Column>;
  private inRows: Row[] = [];
  private outRows: Row[] = [];
  private provOutRows: Row[] = [];
  private provInRows: Row[] = [];
  // Combined (service + provider) binding rows per direction, as sortable grids.
  private outAll: Row[] = [];
  private inAll: Row[] = [];
  private outDs?: InMemoryDataSource;
  private inDs?: InMemoryDataSource;
  // oom-adj props / hosted components, as grid rows.
  private oomAdjRows: Row[] = [];
  private oomAdjDs?: InMemoryDataSource;
  // Per-grid visible-column state (id -> Column[]), so add/hide/reorder of any
  // record grid (hosted services/providers, uid) persists across redraws.
  private recordColumns: {[key: string]: ReadonlyArray<Column>} = {};
  // Which bottom tab is showing.
  private tab: 'procs' | 'detail' = 'procs';
  // Graph area height (px), draggable via the splitter.
  private graphPx = 360;
  private resizing = false;
  private rsY = 0;
  private rsPx = 0;
  private rsMax = 9999; // upper clamp for graphPx, computed from page height
  // The clicked binding edge (drives the detail panel when set).
  private selectedEdge?: EdgeSel;
  // Full connection record(s) for the clicked edge (every binding column).
  private edgeRows: Row[] = [];
  private edgeCols: string[] = [];
  private edgeDs?: InMemoryDataSource;
  private edgeColumns?: ReadonlyArray<Column>;
  // Full records the selected process hosts (its own services / providers) + its
  // uid record — every column, addable via the grid's "Add column" menu.
  private hostedSvc: Row[] = [];
  private hostedSvcCols: string[] = [];
  private hostedSvcDs?: InMemoryDataSource;
  private hostedProv: Row[] = [];
  private hostedProvCols: string[] = [];
  private hostedProvDs?: InMemoryDataSource;
  private uidRow: Row[] = [];
  private uidCols: string[] = [];
  private uidDs?: InMemoryDataSource;

  oninit(vnode: m.Vnode<ProcessStatePageAttrs>) {
    this.trace = vnode.attrs.trace;
    const sub = vnode.attrs.subpage?.replace(/^\//, '');
    if (sub !== undefined && sub !== '' && !Number.isNaN(Number(sub))) {
      this.wantSnapshotId = Number(sub);
    }
    this.loadSnapshots().catch((e) => console.error('ProcessState', e));
  }

  private async loadSnapshots() {
    const q = await this.trace.engine.query(
      `SELECT id, ts, oom_adj_reason AS reason, is_awake, unlocking,
              expanded_notification_shade AS shade,
              last_memory_level_normal AS mem_normal,
              top_process_state, home_pid, heavy_weight_pid, previous_pid,
              dozing_ui_pid, idle_allowlist_appids
       FROM android_process_state_snapshot ORDER BY ts`,
    );
    const it = q.iter({
      id: NUM,
      ts: NUM,
      reason: NUM,
      is_awake: NUM,
      unlocking: NUM,
      shade: NUM,
      mem_normal: NUM,
      top_process_state: NUM_NULL,
      home_pid: NUM_NULL,
      heavy_weight_pid: NUM_NULL,
      previous_pid: NUM_NULL,
      dozing_ui_pid: NUM_NULL,
      idle_allowlist_appids: STR_NULL,
    });
    const out: Snapshot[] = [];
    for (; it.valid(); it.next()) {
      out.push({
        id: it.id,
        ts: BigInt(it.ts),
        reason: it.reason,
        isAwake: it.is_awake > 0,
        unlocking: it.unlocking > 0,
        shade: it.shade > 0,
        memNormal: it.mem_normal > 0,
        topProcState: it.top_process_state,
        homePid: it.home_pid,
        heavyPid: it.heavy_weight_pid,
        prevPid: it.previous_pid,
        dozingPid: it.dozing_ui_pid,
        idleAllowlist: it.idle_allowlist_appids ?? '',
      });
    }
    this.snapshots = out;
    // Honor a deep-linked snapshot id; otherwise default to the latest.
    const wanted =
      this.wantSnapshotId !== undefined
        ? out.findIndex((s) => s.id === this.wantSnapshotId)
        : -1;
    this.idx = wanted >= 0 ? wanted : Math.max(0, out.length - 1);
    await this.loadSnapshot();
  }

  private async loadSnapshot() {
    const snap = this.snapshots[this.idx];
    if (snap === undefined) return;
    const token = ++this.loadToken;
    // Every column of the process table is available (the grid defaults to a
    // curated subset and the rest are addable via the DataGrid's "Add column"
    // menu). process_name is also aliased to `name` for the graph + default view.
    const q = await this.trace.engine.query(`
      SELECT *, process_name AS name
      FROM android_process_state_process
      WHERE snapshot_id = ${snap.id}
      ORDER BY cur_adj`);
    if (token !== this.loadToken) return;
    const rows: Row[] = [];
    const it = q.iter({});
    this.procCols = q.columns();
    for (; it.valid(); it.next()) {
      const r: Row = {};
      for (const c of this.procCols) r[c] = it.get(c);
      rows.push(r);
    }
    this.procRows = rows;
    this.procDs = new InMemoryDataSource(rows);

    // A selection only makes sense within the snapshot it was made in. As you
    // scrub time: drop any selected process that no longer exists (its edges go
    // with it); ones that still exist stay selected and the graph re-renders them
    // in their new tier with updated edges.
    if (this.selectedPids.size > 0) {
      const present = new Set(rows.map((r) => Number(r['pid'])));
      for (const pid of [...this.selectedPids]) {
        if (!present.has(pid)) this.selectedPids.delete(pid);
      }
      this.syncPrimary();
      if (
        this.selectedPids.size === 0 &&
        this.tab === 'detail' &&
        this.selectedEdge === undefined
      ) {
        this.tab = 'procs';
      }
    }

    await this.loadSelected();
    m.redraw();
  }

  private async loadSelected() {
    const snap = this.snapshots[this.idx];
    if (snap === undefined || this.selectedPid === undefined) {
      this.inRows = this.outRows = this.provOutRows = this.provInRows = [];
      this.hostedSvc = this.hostedProv = this.uidRow = [];
      return;
    }
    const token = this.loadToken;
    const pid = this.selectedPid;
    // These are SUMMARY views (one row per peer↔service/provider), so multiple
    // ConnectionRecords between the same pair collapse into a single row with a
    // binding count `n`. Click the edge in the graph for the per-record detail.
    // Outbound: services THIS pid is a client of (who it depends on).
    const outQ = await this.trace.engine.query(`
      SELECT s.owning_pid AS server_pid, s.short_name AS service,
             MAX(b.flag_foreground_service) AS fg, COUNT(*) AS n
      FROM android_process_state_binding b
      LEFT JOIN android_process_state_service s
        ON s.snapshot_id = b.snapshot_id AND s.service_id = b.service_id
      WHERE b.snapshot_id = ${snap.id} AND b.client_pid = ${pid}
      GROUP BY s.owning_pid, b.service_id`);
    // Inbound: clients bound to services HOSTED by this pid (who depends on us).
    const inQ = await this.trace.engine.query(`
      SELECT b.client_pid, s.short_name AS service,
             MAX(b.flag_foreground_service) AS fg, COUNT(*) AS n
      FROM android_process_state_binding b
      JOIN android_process_state_service s
        ON s.snapshot_id = b.snapshot_id AND s.service_id = b.service_id
      WHERE b.snapshot_id = ${snap.id} AND s.owning_pid = ${pid}
      GROUP BY b.client_pid, b.service_id`);
    // Outbound content-provider deps: providers THIS pid is a client of.
    const provOutQ = await this.trace.engine.query(`
      SELECT p.owning_pid AS server_pid, p.authority, COUNT(*) AS n
      FROM android_process_state_provider_binding pb
      JOIN android_process_state_provider p
        ON p.snapshot_id = pb.snapshot_id AND p.provider_id = pb.provider_id
      WHERE pb.snapshot_id = ${snap.id} AND pb.client_pid = ${pid}
      GROUP BY p.owning_pid, pb.provider_id`);
    // Inbound: clients of providers HOSTED by this pid.
    const provInQ = await this.trace.engine.query(`
      SELECT pb.client_pid, p.authority, COUNT(*) AS n
      FROM android_process_state_provider_binding pb
      JOIN android_process_state_provider p
        ON p.snapshot_id = pb.snapshot_id AND p.provider_id = pb.provider_id
      WHERE pb.snapshot_id = ${snap.id} AND p.owning_pid = ${pid}
      GROUP BY pb.client_pid, pb.provider_id`);
    if (token !== this.loadToken) return;
    this.outRows = this.rowsOf(outQ);
    this.inRows = this.rowsOf(inQ);
    this.provOutRows = this.rowsOf(provOutQ);
    this.provInRows = this.rowsOf(provInQ);

    // Full records this process hosts (its own services / providers) + its uid
    // record — every column, so the detail grids can surface all of them.
    const hsvcQ = await this.trace.engine.query(`
      SELECT * FROM android_process_state_service
      WHERE snapshot_id = ${snap.id} AND owning_pid = ${pid}`);
    const hprovQ = await this.trace.engine.query(`
      SELECT * FROM android_process_state_provider
      WHERE snapshot_id = ${snap.id} AND owning_pid = ${pid}`);
    const uidQ = await this.trace.engine.query(`
      SELECT * FROM android_process_state_uid
      WHERE snapshot_id = ${snap.id} AND uid = (
        SELECT uid FROM android_process_state_process
        WHERE snapshot_id = ${snap.id} AND pid = ${pid} LIMIT 1)`);
    if (token !== this.loadToken) return;
    this.hostedSvc = this.rowsOf(hsvcQ);
    this.hostedSvcCols = hsvcQ.columns();
    this.hostedProv = this.rowsOf(hprovQ);
    this.hostedProvCols = hprovQ.columns();
    this.uidRow = this.rowsOf(uidQ);
    this.uidCols = uidQ.columns();

    this.buildDetailGrids(pid);
  }

  // Turn the selected process's facts into grid rows + cached data sources, so
  // every detail section is a sortable/filterable DataGrid.
  private buildDetailGrids(pid: number) {
    const p = this.procRows.find((r) => Number(r['pid']) === pid);
    const v = (k: string) =>
      !p || p[k] === null || p[k] === undefined ? '—' : String(p[k]);
    // Show the human-readable enum/flag names (raw value still in the grid tab).
    this.oomAdjRows = [
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
      {
        property: 'fg service types',
        value: p ? fgsTypeNames(p['fg_service_types']) : '—',
      },
      {
        property: 'hosting',
        value: p ? hostingTypeNames(p['hosting_component_types']) : '—',
      },
      {property: 'frozen', value: p && Number(p['is_frozen']) ? 'yes' : 'no'},
      {
        property: 'persistent',
        value: p && Number(p['persistent']) ? 'yes' : 'no',
      },
      {
        property: 'background restricted',
        value: p && Number(p['background_restricted']) ? 'yes' : 'no',
      },
      {property: 'adj source', value: v('adj_source_pid')},
    ];
    // `n` = number of underlying ConnectionRecords collapsed into this row.
    this.outAll = [
      ...this.outRows.map((b) => ({
        pid: Number(b['server_pid'] ?? 0),
        kind: 'service',
        name: String(b['service'] ?? ''),
        fg: Number(b['fg']) ? 'fg' : '',
        n: Number(b['n'] ?? 1),
      })),
      ...this.provOutRows.map((b) => ({
        pid: Number(b['server_pid'] ?? 0),
        kind: 'provider',
        name: String(b['authority'] ?? ''),
        fg: '',
        n: Number(b['n'] ?? 1),
      })),
    ];
    this.inAll = [
      ...this.inRows.map((b) => ({
        pid: Number(b['client_pid'] ?? 0),
        kind: 'service',
        name: String(b['service'] ?? ''),
        fg: Number(b['fg']) ? 'fg' : '',
        n: Number(b['n'] ?? 1),
      })),
      ...this.provInRows.map((b) => ({
        pid: Number(b['client_pid'] ?? 0),
        kind: 'provider',
        name: String(b['authority'] ?? ''),
        fg: '',
        n: Number(b['n'] ?? 1),
      })),
    ];
    this.oomAdjDs = new InMemoryDataSource(this.oomAdjRows);
    this.hostedSvcDs = new InMemoryDataSource(this.hostedSvc);
    this.hostedProvDs = new InMemoryDataSource(this.hostedProv);
    this.uidDs = new InMemoryDataSource(this.uidRow);
    this.outDs = new InMemoryDataSource(this.outAll);
    this.inDs = new InMemoryDataSource(this.inAll);
  }

  private rowsOf(
    q: ReturnType<Trace['engine']['query']> extends Promise<infer R>
      ? R
      : never,
  ): Row[] {
    const cols = q.columns();
    const it = q.iter({});
    const rows: Row[] = [];
    for (; it.valid(); it.next()) {
      const r: Row = {};
      for (const c of cols) r[c] = it.get(c);
      rows.push(r);
    }
    return rows;
  }

  private seeking = false;

  private setIdx(i: number) {
    const clamped = Math.min(this.snapshots.length - 1, Math.max(0, i));
    if (clamped === this.idx) return;
    this.idx = clamped;
    this.loadSnapshot().catch((e) => console.error(e));
  }

  private step(d: number) {
    this.setIdx(this.idx + d);
  }

  // Map a fractional x (0..1) along the seek bar to the snapshot nearest in time.
  private seekTo(frac: number) {
    const n = this.snapshots.length;
    if (n === 0) return;
    const t0 = this.snapshots[0].ts;
    const t1 = this.snapshots[n - 1].ts;
    const target =
      t0 + BigInt(Math.round(Number(t1 - t0) * Math.min(1, Math.max(0, frac))));
    // nearest snapshot by ts
    let best = 0;
    let bestD = -1n;
    for (let i = 0; i < n; i++) {
      const d =
        this.snapshots[i].ts > target
          ? this.snapshots[i].ts - target
          : target - this.snapshots[i].ts;
      if (bestD < 0n || d < bestD) {
        bestD = d;
        best = i;
      }
    }
    this.setIdx(best);
  }

  private onSeek(ev: PointerEvent, down: boolean) {
    if (down) {
      this.seeking = true;
      (ev.currentTarget as Element).setPointerCapture(ev.pointerId);
    }
    if (!this.seeking) return;
    const rect = (ev.currentTarget as Element).getBoundingClientRect();
    this.seekTo((ev.clientX - rect.left) / rect.width);
  }

  private select(pid: number, additive = false) {
    if (additive) {
      // shift-click toggles membership in the multi-selection.
      if (this.selectedPids.has(pid)) this.selectedPids.delete(pid);
      else this.selectedPids.add(pid);
    } else if (this.selectedPids.size === 1 && this.selectedPids.has(pid)) {
      // plain-clicking the only selected node clears the selection.
      this.selectedPids = new Set();
    } else {
      this.selectedPids = new Set([pid]);
    }
    this.syncPrimary();
    this.selectedEdge = undefined;
    this.tab = this.selectedPids.size > 0 ? 'detail' : 'procs';
    this.loadSelected()
      .then(() => m.redraw())
      .catch((e) => console.error(e));
  }

  // selectedPid (the single-node detail target) is the sole selected pid, or
  // undefined when zero or many are selected.
  private syncPrimary() {
    this.selectedPid =
      this.selectedPids.size === 1 ? [...this.selectedPids][0] : undefined;
  }

  private selectEdge(e: EdgeSel) {
    this.selectedEdge = e;
    this.selectedPids = new Set();
    this.selectedPid = undefined;
    this.edgeColumns = undefined; // reset visible columns for the new connection
    this.tab = 'detail';
    this.loadEdge(e)
      .then(() => m.redraw())
      .catch((err) => console.error(err));
  }

  // Load the actual connection record row(s) behind a clicked edge — every
  // column of the binding / provider-binding table, so the detail grid can
  // surface all of them (flags, effective state, client uid, …) on demand.
  private async loadEdge(e: EdgeSel) {
    const snap = this.snapshots[this.idx];
    if (snap === undefined) {
      this.edgeRows = [];
      this.edgeCols = [];
      this.edgeDs = undefined;
      return;
    }
    const q =
      e.kind === 'provider'
        ? await this.trace.engine.query(`
          SELECT pb.*, p.authority, p.owning_pid AS host_pid
          FROM android_process_state_provider_binding pb
          LEFT JOIN android_process_state_provider p
            ON p.snapshot_id = pb.snapshot_id AND p.provider_id = pb.provider_id
          WHERE pb.snapshot_id = ${snap.id} AND pb.client_pid = ${e.from}
                AND p.owning_pid = ${e.to}`)
        : await this.trace.engine.query(`
          SELECT b.*, s.short_name AS service, s.owning_pid AS host_pid
          FROM android_process_state_binding b
          LEFT JOIN android_process_state_service s
            ON s.snapshot_id = b.snapshot_id AND s.service_id = b.service_id
          WHERE b.snapshot_id = ${snap.id} AND b.client_pid = ${e.from}
                AND s.owning_pid = ${e.to}`);
    this.edgeCols = q.columns();
    this.edgeRows = this.rowsOf(q);
    this.edgeDs = new InMemoryDataSource(this.edgeRows);
  }

  // Clicking empty graph space clears any node/edge selection.
  private deselect() {
    if (this.selectedPids.size === 0 && this.selectedEdge === undefined) return;
    this.selectedPids = new Set();
    this.selectedPid = undefined;
    this.selectedEdge = undefined;
    this.tab = 'procs';
    m.redraw();
  }

  private nameOf(pid: number): string {
    const r = this.procRows.find((p) => Number(p['pid']) === pid);
    return r ? String(r['name'] ?? pid).replace(/^.*\//, '') : String(pid);
  }

  private tabBtn(id: 'procs' | 'detail', label: string): m.Children {
    return m(
      'button.pf-ps-tab' + (this.tab === id ? '.pf-ps-tab--on' : ''),
      {
        onclick: () => {
          this.tab = id;
        },
      },
      label,
    );
  }

  view() {
    if (this.snapshots.length === 0) {
      return m(
        '.pf-ps-page',
        m('.pf-ps-empty', 'Loading process-state snapshots…'),
      );
    }
    const snap = this.snapshots[this.idx];
    const hasSel =
      this.selectedPids.size > 0 || this.selectedEdge !== undefined;
    return m('.pf-ps-page', [
      this.renderSeek(snap),
      this.procDs &&
        m(
          '.pf-ps-graphwrap',
          {style: `flex: 0 0 ${this.graphPx}px`},
          m(ProcessGraph, {
            processes: this.procRows,
            bindingsQuery: snap.id,
            trace: this.trace,
            selectedPids: this.selectedPids,
            onSelect: (pid, additive) => this.select(pid, additive),
            onEdgeSelect: (e) => this.selectEdge(e),
            onDeselect: () => this.deselect(),
          }),
        ),
      m('.pf-ps-resize', {
        onpointerdown: (e: PointerEvent) => {
          this.resizing = true;
          this.rsY = e.clientY;
          this.rsPx = this.graphPx;
          // Cap the graph so the seek bar + tabs + tables always stay on screen.
          const page = (e.currentTarget as HTMLElement).parentElement;
          this.rsMax = page ? Math.max(160, page.clientHeight - 230) : 9999;
          (e.currentTarget as Element).setPointerCapture(e.pointerId);
        },
        onpointermove: (e: PointerEvent) => {
          if (this.resizing) {
            this.graphPx = Math.min(
              this.rsMax,
              Math.max(140, this.rsPx + (e.clientY - this.rsY)),
            );
          }
        },
        onpointerup: () => {
          this.resizing = false;
        },
      }),
      m('.pf-ps-bottom', [
        m('.pf-ps-tabs', [
          this.tabBtn('procs', `Processes · ${this.procRows.length}`),
          hasSel
            ? this.tabBtn(
                'detail',
                this.selectedEdge
                  ? '▸ binding'
                  : this.selectedPids.size > 1
                    ? `▸ ${this.selectedPids.size} selected`
                    : `▸ pid ${this.selectedPid}`,
              )
            : null,
        ]),
        m(
          '.pf-ps-tabbody' +
            (this.tab === 'detail' ? '.pf-ps-tabbody--scroll' : ''),
          this.tabBody(),
        ),
      ]),
    ]);
  }

  // Winscope-style seek bar: scrub across the snapshots over time. A tick per
  // snapshot, a draggable playhead, and the device-wide context of the snapshot
  // you're on — so navigating time and reading state happen in one place.
  private renderSeek(snap: Snapshot): m.Children {
    const n = this.snapshots.length;
    const t0 = this.snapshots[0].ts;
    const span = Number(this.snapshots[n - 1].ts - t0) || 1;
    const pct = (s: Snapshot) => (Number(s.ts - t0) / span) * 100;
    return m('.pf-ps-seekwrap', [
      m('.pf-ps-seekrow', [
        m('button.pf-ps-step', {onclick: () => this.step(-1)}, '‹'),
        m(
          '.pf-ps-seek',
          {
            onpointerdown: (e: PointerEvent) => this.onSeek(e, true),
            onpointermove: (e: PointerEvent) => this.onSeek(e, false),
            onpointerup: () => {
              this.seeking = false;
            },
            onpointerleave: () => {
              this.seeking = false;
            },
          },
          [
            m('.pf-ps-seek-line'),
            ...this.snapshots.map((s, i) =>
              m(
                '.pf-ps-seek-tick' +
                  (i === this.idx ? '.pf-ps-seek-tick--on' : ''),
                {style: `left:${pct(s)}%`},
              ),
            ),
            m('.pf-ps-seek-head', {style: `left:${pct(snap)}%`}),
          ],
        ),
        m('button.pf-ps-step', {onclick: () => this.step(1)}, '›'),
        m(
          'span.pf-ps-pos',
          `${this.idx + 1}/${n} · reason ${snap.reason} · ${this.procRows.length} procs`,
        ),
      ]),
    ]);
  }

  private tabBody(): m.Children {
    if (this.tab === 'detail') return this.renderDetail();
    const visible =
      this.procColumns ??
      DEFAULT_PROC_COLS.filter((c) => this.procCols.includes(c)).map((c) => ({
        id: c,
        field: c,
      }));
    return this.procDs
      ? m(DataGrid, {
          // Full schema = every process column (addable via "Add column");
          // `columns` is just the visible/default subset.
          schema: gridSchema(this.procCols, (pid) => this.select(pid)),
          rootSchema: 'root',
          data: this.procDs,
          fillHeight: true,
          columns: visible,
          onColumnsChanged: (cols) => {
            this.procColumns = cols;
          },
        })
      : m('.pf-ps-none', 'Loading…');
  }

  // ----- structured detail panel: a clicked edge OR a selected process -----

  private renderDetail(): m.Children {
    if (this.selectedEdge) return this.renderEdgeDetail(this.selectedEdge);
    if (this.selectedPids.size > 1) return this.renderMultiDetail();
    if (this.selectedPid !== undefined) {
      return this.renderProcessDetail(this.selectedPid);
    }
    return m(
      '.pf-ps-none',
      'Click a node or an edge in the graph (or a row in Processes) to inspect it. ' +
        'Shift-click nodes to select several.',
    );
  }

  // Several nodes selected (shift-click): a grid of just those processes.
  private renderMultiDetail(): m.Children {
    const rows = this.procRows.filter((r) =>
      this.selectedPids.has(Number(r['pid'])),
    );
    const ds = new InMemoryDataSource(rows);
    return m('.pf-ps-detailpane', [
      m('.pf-ps-detail-h', [
        m(
          'span.pf-ps-detail-title',
          `${this.selectedPids.size} processes selected`,
        ),
        m(
          'span.pf-ps-detail-sub',
          'shift-click a node to add/remove · click empty space to clear',
        ),
      ]),
      this.gridCard(
        'selection',
        DEFAULT_PROC_COLS.filter((c) => this.procCols.includes(c)),
        rows,
        ds,
      ),
    ]);
  }

  private gridCard(
    title: string,
    cols: string[],
    rows: Row[],
    ds?: InMemoryDataSource,
  ): m.Children {
    return gridCard(title, cols, rows, ds, (pid) => this.select(pid));
  }

  // A labelled card showing a FULL table record: every column of `allCols` is
  // addable via the grid's "Add column" menu; `defaultCols` are shown initially.
  // `key` namespaces the persisted visible-column state in this.recordColumns.
  private recordCard(
    title: string,
    key: string,
    allCols: string[],
    defaultCols: string[],
    rows: Row[],
    ds?: InMemoryDataSource,
  ): m.Children {
    if (!rows.length || ds === undefined) {
      return m('.pf-ps-card', [
        m('.pf-ps-card-h', title),
        m('.pf-ps-card-b', m('.pf-ps-none', '— none —')),
      ]);
    }
    const visible =
      this.recordColumns[key] ??
      defaultCols
        .filter((c) => allCols.includes(c))
        .map((c) => ({id: c, field: c}));
    return m('.pf-ps-card', [
      m('.pf-ps-card-h', title),
      m(DataGrid, {
        schema: gridSchema(allCols, (pid) => this.select(pid)),
        rootSchema: 'root',
        data: ds,
        columns: visible,
        onColumnsChanged: (cols) => {
          this.recordColumns[key] = cols;
        },
      }),
    ]);
  }

  private renderProcessDetail(pid: number): m.Children {
    const p = this.procRows.find((r) => Number(r['pid']) === pid);
    if (!p) return m('.pf-ps-none', `pid ${pid} not present in this snapshot.`);
    return m('.pf-ps-detailpane', [
      m('.pf-ps-detail-h', [
        m('span.pf-ps-detail-title', this.nameOf(pid)),
        m('span.pf-ps-detail-sub', `pid ${pid} \u00b7 uid ${p['uid']}`),
      ]),
      m('.pf-ps-cards', [
        m('.pf-ps-col', [
          this.gridCard(
            'process state',
            ['property', 'value'],
            this.oomAdjRows,
            this.oomAdjDs,
          ),
          this.recordCard(
            'uid record',
            'uid',
            this.uidCols,
            DEFAULT_UID_COLS,
            this.uidRow,
            this.uidDs,
          ),
          this.recordCard(
            'hosted services',
            'hostedSvc',
            this.hostedSvcCols,
            DEFAULT_SERVICE_COLS,
            this.hostedSvc,
            this.hostedSvcDs,
          ),
          this.recordCard(
            'hosted providers',
            'hostedProv',
            this.hostedProvCols,
            DEFAULT_PROVIDER_COLS,
            this.hostedProv,
            this.hostedProvDs,
          ),
        ]),
        m('.pf-ps-col', [
          this.gridCard(
            'depends on \u2014 outbound',
            ['pid', 'kind', 'name', 'fg', 'n'],
            this.outAll,
            this.outDs,
          ),
          this.gridCard(
            'depended on by \u2014 inbound',
            ['pid', 'kind', 'name', 'fg', 'n'],
            this.inAll,
            this.inDs,
          ),
        ]),
      ]),
    ]);
  }

  private renderEdgeDetail(e: EdgeSel): m.Children {
    const provider = e.kind === 'provider';
    const def = provider ? DEFAULT_PROVBIND_COLS : DEFAULT_BINDING_COLS;
    const visible =
      this.edgeColumns ??
      def
        .filter((c) => this.edgeCols.includes(c))
        .map((c) => ({id: c, field: c}));
    const nm = this.nameOf(e.from);
    const host = this.nameOf(e.to);
    return m('.pf-ps-detailpane', [
      m('.pf-ps-detail-h', [
        m(
          'span.pf-ps-detail-title',
          `${provider ? 'content-provider' : 'service'} binding` +
            ` · ${this.edgeRows.length}`,
        ),
        m(
          'span.pf-ps-detail-sub',
          `${nm} (${e.from}) → ${host} (${e.to}) · every column addable`,
        ),
      ]),
      m('.pf-ps-card', [
        m(
          '.pf-ps-card-h',
          provider
            ? 'content-provider binding record'
            : 'service binding record',
        ),
        this.edgeRows.length && this.edgeDs
          ? m(DataGrid, {
              // Full record schema — all binding columns addable via the menu.
              schema: gridSchema(this.edgeCols, (pid) => this.select(pid)),
              rootSchema: 'root',
              data: this.edgeDs,
              columns: visible,
              onColumnsChanged: (cols) => {
                this.edgeColumns = cols;
              },
            })
          : m('.pf-ps-card-b', m('.pf-ps-none', '— none —')),
      ]),
    ]);
  }
}
