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
import {LONG, NUM, STR_NULL} from '../../trace_processor/query_result';
import {InMemoryDataSource} from '../../components/widgets/datagrid/in_memory_data_source';
import {type DiffStatus, type EdgeSel, sameEdge} from './process_graph';

// URI of the single snapshot timeline track. The slice id IS the snapshot id,
// so a snapshot id is a valid event id on this track — letting the explorer
// keep the timeline slice highlight in sync with the scrubber.
export const SNAPSHOT_TRACK_URI = '/process_state_snapshots';

// Process fields compared in diff mode; a change is shown in the field's own
// grid column as "old → new". (All already arrive as display strings.)
const DIFF_COLS = ['oom_score', 'proc_state', 'capabilities'];

export interface SnapshotInfo {
  readonly id: number;
  readonly ts: bigint;
  // OomChangeReasonEnum name (already resolved by the importer); undefined for a
  // one-shot dumpsys snapshot.
  readonly reason?: string;
}

// Single source of truth for the ProcessState explorer: created once per trace
// and shared by the (one) details-panel surface. Holds all selection / view
// state and the loaded snapshot data; views are thin renderers over it and
// mutate only through its methods, which load lazily and redraw. No global
// state.
export class ProcessStateController {
  readonly trace: Trace;

  // ---- view state (the shared selection) ----
  snapshots: ReadonlyArray<SnapshotInfo> = [];
  snapshotId?: number;
  selectedPid?: number;
  selectedEdge?: EdgeSel;
  diffOn = false;
  baselineId?: number;
  // When true (the default), the diff baseline follows the current snapshot's
  // immediately-previous one as you scrub ("what did this event change"). When
  // false, the baseline is pinned to a chosen snapshot (cumulative "since X").
  baselineFollowsPrev = true;
  tab: 'current' | 'procs' = 'procs';

  // ---- loaded data for the current snapshot ----
  processes: Row[] = [];
  procCols: string[] = [];
  // Union of current + baseline-only "removed" processes (== processes when not
  // diffing); what the graph and the process list are drawn from.
  graphProcesses: Row[] = [];
  procDs?: InMemoryDataSource;
  diffNodes = new Map<number, DiffStatus>();

  // ---- selected-process detail ----
  stateRows: Row[] = [];
  stateDs?: InMemoryDataSource;
  hostedSvc: Row[] = [];
  hostedSvcCols: string[] = [];
  hostedSvcDs?: InMemoryDataSource;
  hostedProv: Row[] = [];
  hostedProvCols: string[] = [];
  hostedProvDs?: InMemoryDataSource;
  outAll: Row[] = [];
  outDs?: InMemoryDataSource;
  inAll: Row[] = [];
  inDs?: InMemoryDataSource;
  selfAll: Row[] = [];
  selfDs?: InMemoryDataSource;

  // ---- selected-edge detail ----
  edgeRows: Row[] = [];
  edgeDs?: InMemoryDataSource;
  edgeNames: Row[] = [];
  edgeNamesDs?: InMemoryDataSource;

  private loadToken = 0;
  private snapshotsLoaded = false;

  constructor(trace: Trace) {
    this.trace = trace;
  }

  reasonOf(id: number | undefined): string | undefined {
    return this.snapshots.find((s) => s.id === id)?.reason ?? undefined;
  }

  // Loads the snapshot list (once) and selects `id`, or the latest if absent.
  async ensureLoaded(id?: number) {
    if (!this.snapshotsLoaded) {
      const q = await this.trace.engine.query(
        `SELECT id, ts, reason FROM _ps_snapshot ORDER BY ts`,
      );
      const snaps: SnapshotInfo[] = [];
      const it = q.iter({id: NUM, ts: LONG, reason: STR_NULL});
      for (; it.valid(); it.next()) {
        snaps.push({id: it.id, ts: it.ts, reason: it.reason ?? undefined});
      }
      this.snapshots = snaps;
      this.snapshotsLoaded = true;
    }
    const want =
      id ??
      this.snapshotId ??
      (this.snapshots.length > 0
        ? this.snapshots[this.snapshots.length - 1].id
        : undefined);
    if (want !== undefined && want !== this.snapshotId) {
      await this.setSnapshot(want);
    } else if (this.processes.length === 0 && want !== undefined) {
      await this.setSnapshot(want);
    }
  }

  // Move to a snapshot AND highlight its slice on the timeline, so the scrubber
  // and the timeline selection stay in sync. setSnapshot updates snapshotId
  // synchronously, so the selectTrackEvent below re-enters load()/ensureLoaded
  // as a no-op (no reload loop).
  goToSnapshot(id: number) {
    this.setSnapshot(id).catch((e) => console.error('ProcessState', e));
    this.trace.selection.selectTrackEvent(SNAPSHOT_TRACK_URI, id);
  }

  async setSnapshot(id: number) {
    this.snapshotId = id;
    this.selectedPid = undefined;
    this.selectedEdge = undefined;
    this.clearSelectedData();
    const token = ++this.loadToken;
    const q = await this.trace.engine.query(`
      SELECT * FROM _ps_process WHERE snapshot_id = ${id} ORDER BY oom_score`);
    if (token !== this.loadToken) return;
    this.procCols = q.columns();
    this.processes = this.rowsOf(q);
    // In follow mode the baseline tracks the new snapshot's previous one; a
    // pinned baseline stays put (but never equal to the current snapshot).
    if (this.baselineFollowsPrev || this.baselineId === id) {
      this.baselineId = this.prevSnapshotId(id);
    }
    await this.refreshDiff(token);
    m.redraw();
  }

  prevSnapshotId(id: number): number | undefined {
    const i = this.snapshots.findIndex((s) => s.id === id);
    return i > 0 ? this.snapshots[i - 1].id : undefined;
  }

  // ---- selection ----

  select(pid: number) {
    if (this.selectedPid === pid && this.selectedEdge === undefined) {
      this.selectedPid = undefined;
      this.clearSelectedData();
      this.tab = 'procs';
      m.redraw();
      return;
    }
    this.selectedPid = pid;
    this.selectedEdge = undefined;
    this.tab = 'current';
    this.loadSelected(pid)
      .then(() => m.redraw())
      .catch((e) => console.error('ProcessState', e));
  }

  selectEdge(e: EdgeSel) {
    if (this.selectedEdge !== undefined && sameEdge(this.selectedEdge, e)) {
      this.selectedEdge = undefined;
      this.tab = this.selectedPid !== undefined ? 'current' : 'procs';
      m.redraw();
      return;
    }
    this.selectedEdge = e;
    this.buildEdge(e);
    this.tab = 'current';
    m.redraw();
  }

  deselect() {
    this.selectedPid = undefined;
    this.selectedEdge = undefined;
    this.clearSelectedData();
    this.tab = 'procs';
    m.redraw();
  }

  setTab(tab: 'current' | 'procs') {
    this.tab = tab;
    m.redraw();
  }

  toggleDiff() {
    this.diffOn = !this.diffOn;
    // Default to the follow-previous view each time diff is (re)enabled.
    if (
      this.diffOn &&
      this.baselineFollowsPrev &&
      this.snapshotId !== undefined
    ) {
      this.baselineId = this.prevSnapshotId(this.snapshotId);
    }
    this.refreshDiff(this.loadToken)
      .then(() => m.redraw())
      .catch((e) => console.error('ProcessState', e));
  }

  // Switch back to following the previous snapshot (the auto baseline).
  followPrevBaseline() {
    this.baselineFollowsPrev = true;
    if (this.snapshotId !== undefined) {
      this.baselineId = this.prevSnapshotId(this.snapshotId);
    }
    this.refreshDiff(this.loadToken)
      .then(() => m.redraw())
      .catch((e) => console.error('ProcessState', e));
  }

  // Pin the baseline to a specific snapshot (cumulative diff since that point).
  setBaseline(id: number) {
    this.baselineFollowsPrev = false;
    this.baselineId = id;
    this.refreshDiff(this.loadToken)
      .then(() => m.redraw())
      .catch((e) => console.error('ProcessState', e));
  }

  // ---- data loading ----

  private clearSelectedData() {
    this.stateRows = [];
    this.hostedSvc = this.hostedProv = [];
    this.outAll = this.inAll = this.selfAll = [];
    this.stateDs = this.hostedSvcDs = this.hostedProvDs = undefined;
    this.outDs = this.inDs = this.selfDs = undefined;
  }

  // Recomputes graphProcesses / diffNodes / the process-list rows for the
  // current diff state.
  private async refreshDiff(token: number) {
    const id = this.snapshotId;
    if (id === undefined) return;
    if (!this.diffOn || this.baselineId === undefined) {
      this.diffNodes = new Map();
      this.graphProcesses = this.processes;
      this.procDs = new InMemoryDataSource(this.processes);
      return;
    }
    const bq = await this.trace.engine.query(
      `SELECT * FROM _ps_process WHERE snapshot_id = ${this.baselineId}`,
    );
    if (token !== this.loadToken) return;
    const base = this.rowsOf(bq);
    const baseByPid = new Map(base.map((r) => [Number(r['pid']), r]));
    const curByPid = new Map(this.processes.map((r) => [Number(r['pid']), r]));
    const nodes = new Map<number, DiffStatus>();
    // The process list shows the CURRENT snapshot; a changed field is rewritten
    // in place to "old → new" (encoded in the cell's own value so it survives
    // the DataGrid's column projection). Removed processes aren't in the current
    // snapshot, so they appear only as ghosts in the graph, not the list.
    const listRows: Row[] = [];
    for (const r of this.processes) {
      const pid = Number(r['pid']);
      const b = baseByPid.get(pid);
      if (b === undefined) {
        nodes.set(pid, 'added');
        listRows.push({...r});
        continue;
      }
      const out: Row = {...r};
      let changed = false;
      for (const col of DIFF_COLS) {
        if (String(b[col] ?? '') !== String(r[col] ?? '')) {
          out[col] = `${fmt(b[col])} → ${fmt(r[col])}`;
          changed = true;
        }
      }
      if (changed) nodes.set(pid, 'changed');
      listRows.push(out);
    }
    const removed: Row[] = [];
    for (const r of base) {
      if (!curByPid.has(Number(r['pid']))) {
        nodes.set(Number(r['pid']), 'removed');
        removed.push(r);
      }
    }
    this.diffNodes = nodes;
    this.graphProcesses = [...this.processes, ...removed];
    this.procDs = new InMemoryDataSource(listRows);
  }

  private async loadSelected(pid: number) {
    const id = this.snapshotId;
    if (id === undefined) return;
    const token = this.loadToken;
    const outQ = await this.trace.engine.query(`
      SELECT s.owning_pid AS server_pid, s.name AS service,
             MAX(b.foreground) AS fg, COUNT(*) AS n
      FROM _ps_service_binding b
      LEFT JOIN _ps_service s
        ON s.snapshot_id = b.snapshot_id AND s.service_id = b.service_id
      WHERE b.snapshot_id = ${id} AND b.client_pid = ${pid}
            AND s.owning_pid != ${pid}
      GROUP BY s.owning_pid, b.service_id`);
    const provOutQ = await this.trace.engine.query(`
      SELECT p.owning_pid AS server_pid, p.authority, COUNT(*) AS n
      FROM _ps_provider_binding pb
      JOIN _ps_provider p
        ON p.snapshot_id = pb.snapshot_id AND p.provider_id = pb.provider_id
      WHERE pb.snapshot_id = ${id} AND pb.client_pid = ${pid}
            AND p.owning_pid != ${pid}
      GROUP BY p.owning_pid, pb.provider_id`);
    const inQ = await this.trace.engine.query(`
      SELECT b.client_pid, s.name AS service,
             MAX(b.foreground) AS fg, COUNT(*) AS n
      FROM _ps_service_binding b
      JOIN _ps_service s
        ON s.snapshot_id = b.snapshot_id AND s.service_id = b.service_id
      WHERE b.snapshot_id = ${id} AND s.owning_pid = ${pid}
            AND b.client_pid != ${pid}
      GROUP BY b.client_pid, b.service_id`);
    const provInQ = await this.trace.engine.query(`
      SELECT pb.client_pid, p.authority, COUNT(*) AS n
      FROM _ps_provider_binding pb
      JOIN _ps_provider p
        ON p.snapshot_id = pb.snapshot_id AND p.provider_id = pb.provider_id
      WHERE pb.snapshot_id = ${id} AND p.owning_pid = ${pid}
            AND pb.client_pid != ${pid}
      GROUP BY pb.client_pid, pb.provider_id`);
    const selfSvcQ = await this.trace.engine.query(`
      SELECT s.name AS service, MAX(b.foreground) AS fg, COUNT(*) AS n
      FROM _ps_service_binding b
      JOIN _ps_service s
        ON s.snapshot_id = b.snapshot_id AND s.service_id = b.service_id
      WHERE b.snapshot_id = ${id} AND b.client_pid = ${pid}
            AND s.owning_pid = ${pid}
      GROUP BY b.service_id`);
    const selfProvQ = await this.trace.engine.query(`
      SELECT p.authority, COUNT(*) AS n
      FROM _ps_provider_binding pb
      JOIN _ps_provider p
        ON p.snapshot_id = pb.snapshot_id AND p.provider_id = pb.provider_id
      WHERE pb.snapshot_id = ${id} AND pb.client_pid = ${pid}
            AND p.owning_pid = ${pid}
      GROUP BY pb.provider_id`);
    const hsvcQ = await this.trace.engine.query(
      `SELECT * FROM _ps_service WHERE snapshot_id = ${id} AND owning_pid = ${pid}`,
    );
    const hprovQ = await this.trace.engine.query(
      `SELECT * FROM _ps_provider WHERE snapshot_id = ${id} AND owning_pid = ${pid}`,
    );
    if (token !== this.loadToken) return;

    this.outAll = [
      ...this.bindRows(outQ, 'server_pid', 'service', true),
      ...this.bindRows(provOutQ, 'server_pid', 'authority', false),
    ];
    this.inAll = [
      ...this.bindRows(inQ, 'client_pid', 'service', true),
      ...this.bindRows(provInQ, 'client_pid', 'authority', false),
    ];
    this.selfAll = [
      ...this.selfRows(selfSvcQ, 'service', true),
      ...this.selfRows(selfProvQ, 'authority', false),
    ];
    this.hostedSvc = this.rowsOf(hsvcQ);
    this.hostedSvcCols = hsvcQ.columns();
    this.hostedProv = this.rowsOf(hprovQ);
    this.hostedProvCols = hprovQ.columns();

    this.buildStateCard(pid);
    this.outDs = new InMemoryDataSource(this.outAll);
    this.inDs = new InMemoryDataSource(this.inAll);
    this.selfDs = new InMemoryDataSource(this.selfAll);
    this.hostedSvcDs = new InMemoryDataSource(this.hostedSvc);
    this.hostedProvDs = new InMemoryDataSource(this.hostedProv);
  }

  // Map a binding/provider summary query to {pid,kind,name,fg,n} grid rows.
  private bindRows(
    q: ReturnType<Trace['engine']['query']> extends Promise<infer R>
      ? R
      : never,
    pidCol: string,
    nameCol: string,
    isSvc: boolean,
  ): Row[] {
    return this.rowsOf(q).map((b) => ({
      pid: Number(b[pidCol] ?? 0),
      kind: isSvc ? 'service' : 'provider',
      name: String(b[nameCol] ?? ''),
      fg: isSvc && Number(b['fg']) ? 'fg' : '',
      n: Number(b['n'] ?? 1),
    }));
  }
  private selfRows(
    q: ReturnType<Trace['engine']['query']> extends Promise<infer R>
      ? R
      : never,
    nameCol: string,
    isSvc: boolean,
  ): Row[] {
    return this.rowsOf(q).map((b) => ({
      kind: isSvc ? 'service' : 'provider',
      name: String(b[nameCol] ?? ''),
      fg: isSvc && Number(b['fg']) ? 'fg' : '',
      n: Number(b['n'] ?? 1),
    }));
  }

  private buildStateCard(pid: number) {
    const p = this.graphProcesses.find((r) => Number(r['pid']) === pid);
    // Empty cells show the SQL value NULL (not "—"/"none"/"no") so a missing
    // value is visibly distinct from a real 0/false.
    const v = (k: string) =>
      !p || p[k] === null || p[k] === undefined ? 'NULL' : String(p[k]);
    const persistent =
      !p || p['persistent'] === null || p['persistent'] === undefined
        ? 'NULL'
        : Number(p['persistent'])
          ? 'yes'
          : 'no';
    this.stateRows = [
      {property: 'oom adj', value: v('oom_score')},
      {property: 'proc state', value: v('proc_state')},
      {property: 'capabilities', value: v('capabilities')},
      {property: 'persistent', value: persistent},
    ];
    this.stateDs = new InMemoryDataSource(this.stateRows);
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

  nameOf(pid: number): string {
    const r = this.graphProcesses.find((p) => Number(p['pid']) === pid);
    return r ? String(r['name'] ?? pid).replace(/^.*\//, '') : String(pid);
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
}

// "old → new" cell formatting helper for diffed columns (all display strings).
function fmt(v: unknown): string {
  return v === null || v === undefined ? '' : String(v);
}
