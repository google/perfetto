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
import {
  LONG,
  NUM,
  NUM_NULL,
  STR_NULL,
} from '../../trace_processor/query_result';
import {InMemoryDataSource} from '../../components/widgets/datagrid/in_memory_data_source';
import type {Column} from '../../components/widgets/datagrid/model';
import {type PassRow, transitionsSql} from './aggregators';
import {type DiffStatus, type EdgeSel, sameEdge} from './process_graph';

// URI of the root "By reason" pass track. Each instant/slice id on this track
// IS the OomAdjuster pass seq_id, letting the explorer keep the timeline
// instant highlight in sync with the pass navigation buttons.
export const REASON_TRACK_URI = 'com.android.ProcessState#reason';
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
  readonly seqId?: number;
  readonly isFullUpdate?: boolean;
  readonly topPid?: number;
  readonly topProcState?: string;
  readonly targetPids?: string;
}

export interface SliceContext {
  readonly id: number;
  readonly ts: bigint;
  readonly dur: bigint;
  readonly upid: number;
  readonly pid: number;
  readonly uid?: number | null;
  readonly processName?: string | null;
  readonly packageName?: string | null;
  readonly state: string;
  readonly prevState?: string | null;
  readonly oomScore?: number | null;
  readonly prevOomScore?: number | null;
  readonly capabilityFlags?: number | null;
  readonly reason?: string | null;
  readonly seqId?: number | null;
  readonly hostingType?: string | null;
  readonly hostingName?: string | null;
  readonly triggerType?: string | null;
  readonly bindApplicationDelayMs?: bigint | null;
  readonly processStartDelayMs?: bigint | null;
  readonly exitReason?: string | null;
  readonly exitSubreason?: string | null;
}

// Single source of truth for the ProcessState explorer: created once per trace
// and shared by the details-panel surfaces. Holds all selection / view state
// and the loaded snapshot data; views are thin renderers over it and mutate
// only through its methods, which load lazily and redraw. No global state.
export class ProcessStateController {
  readonly trace: Trace;

  // ---- persisted layout & column state across reason/slice selections ----
  vertical = false;
  splitPercentHorizontal = 55;
  splitPercentVertical = 55;
  lastOpenPercentHorizontal = 55;
  lastOpenPercentVertical = 55;
  transitionColumns?: ReadonlyArray<Column>;
  procColumns?: ReadonlyArray<Column>;
  recordColumns: {[key: string]: ReadonlyArray<Column>} = {};

  // ---- view state (the shared selection) ----
  snapshots: ReadonlyArray<SnapshotInfo> = [];
  private initialBaselineId?: number;
  snapshotId?: number;
  selectedPid?: number;
  selectedEdge?: EdgeSel;
  sliceContext?: SliceContext;
  diffOn = true;
  baselineId?: number;
  // When true (the default), the diff baseline follows the current snapshot's
  // immediately-previous one as you scrub ("what did this event change"). When
  // false, the baseline is pinned to a chosen snapshot (cumulative "since X").
  baselineFollowsPrev = true;
  tab: 'transitions' | 'triggers' | 'current' | 'procs' = 'transitions';

  // ---- loaded data for the current snapshot ----
  processes: Row[] = [];
  procCols: string[] = [];
  // Union of current + baseline-only "removed" processes (== processes when not
  // diffing); what the graph and the process list are drawn from.
  graphProcesses: Row[] = [];
  procDs?: InMemoryDataSource;
  diffNodes = new Map<number, DiffStatus>();

  // ---- pass-level transitions, triggers & changed processes ----
  transitionRows: Row[] = [];
  transitionDs?: InMemoryDataSource;
  passInfoRows: Row[] = [];
  passInfoDs?: InMemoryDataSource;
  triggerRows: Row[] = [];
  triggerDs?: InMemoryDataSource;
  passChangeRows: Row[] = [];
  passChangeDs?: InMemoryDataSource;

  // ---- selected-process detail ----
  sliceInfoRows: Row[] = [];
  sliceInfoDs?: InMemoryDataSource;
  stateRows: Row[] = [];
  stateDs?: InMemoryDataSource;
  procTriggerRows: Row[] = [];
  procTriggerDs?: InMemoryDataSource;
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
  edgeProvRows: Row[] = [];
  edgeProvDs?: InMemoryDataSource;
  edgeSvcRows: Row[] = [];
  edgeSvcDs?: InMemoryDataSource;

  private loadToken = 0;
  private snapshotsLoaded = false;

  constructor(trace: Trace) {
    this.trace = trace;
  }

  snapshotOf(id: number | undefined): SnapshotInfo | undefined {
    return this.snapshots.find((s) => s.id === id);
  }

  reasonOf(id: number | undefined): string | undefined {
    return this.snapshotOf(id)?.reason ?? undefined;
  }

  private async ensureSnapshotsList() {
    if (this.snapshotsLoaded) return;

    const initQ = await this.trace.engine.query(`
      SELECT id FROM _ps_snapshot ORDER BY ts, id LIMIT 1
    `);
    if (initQ.numRows() > 0) {
      this.initialBaselineId = initQ.firstRow({id: NUM}).id;
    }

    // Prefer passes present on the "By reason" track (where seq_id is in
    // _android_process_state_intervals) so pass navigation and the "By reason"
    // instants stay 1-to-1. Fall back to all _ps_snapshot rows if the trace
    // has no interval passes.
    const byReasonQ = await this.trace.engine.tryQuery(`
      SELECT
        s.id,
        p.ts,
        coalesce(p.reason, replace(s.reason, 'OOM_ADJ_REASON_', '')) AS reason,
        s.seq_id,
        s.is_full_update,
        s.top_pid,
        s.top_proc_state,
        s.target_pids
      FROM (
        SELECT
          seq_id,
          min(ts) AS ts,
          replace(reason, 'OOM_ADJ_REASON_', '') AS reason
        FROM _android_process_state_intervals
        WHERE seq_id IS NOT NULL
        GROUP BY seq_id
      ) p
      JOIN _ps_snapshot s ON s.seq_id = p.seq_id
      ORDER BY p.ts, s.id
    `);

    const q =
      byReasonQ.ok && byReasonQ.value.numRows() > 0
        ? byReasonQ.value
        : await this.trace.engine.query(`
            SELECT
              id,
              ts,
              replace(reason, 'OOM_ADJ_REASON_', '') AS reason,
              seq_id,
              is_full_update,
              top_pid,
              top_proc_state,
              target_pids
            FROM _ps_snapshot
            ORDER BY ts, id
          `);

    const snaps: SnapshotInfo[] = [];
    const it = q.iter({
      id: NUM,
      ts: LONG,
      reason: STR_NULL,
      seq_id: NUM_NULL,
      is_full_update: NUM_NULL,
      top_pid: NUM_NULL,
      top_proc_state: STR_NULL,
      target_pids: STR_NULL,
    });
    for (; it.valid(); it.next()) {
      snaps.push({
        id: it.id,
        ts: it.ts,
        reason: it.reason ?? undefined,
        seqId: it.seq_id ?? undefined,
        isFullUpdate:
          it.is_full_update !== null ? Boolean(it.is_full_update) : undefined,
        topPid: it.top_pid ?? undefined,
        topProcState: it.top_proc_state ?? undefined,
        targetPids: it.target_pids ?? undefined,
      });
    }
    this.snapshots = snaps;
    this.snapshotsLoaded = true;
  }

  // Loads the snapshot list (once) and selects `id`, or the latest if absent.
  async ensureLoaded(id?: number) {
    this.sliceContext = undefined;
    await this.ensureSnapshotsList();
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

  // Finds the snapshot corresponding to an instant/slice on a "By reason" track
  // and loads its binding graph, transitions breakdown, and causal triggers.
  async loadForPass(pass: PassRow) {
    const hadSliceContext = this.sliceContext !== undefined;
    this.sliceContext = undefined;
    await this.ensureSnapshotsList();
    if (this.snapshots.length === 0) return;

    const seqId = Number(pass.seq_id);
    let target = this.snapshots.find((s) => s.seqId === seqId);
    if (target === undefined) {
      target = this.snapshots.find((s) => s.id === Number(pass.id));
    }
    if (target === undefined) {
      for (const s of this.snapshots) {
        if (s.ts <= pass.ts) {
          target = s;
        } else {
          break;
        }
      }
      if (target === undefined) {
        target = this.snapshots[0];
      }
    }

    if (
      target.id === this.snapshotId &&
      this.processes.length > 0 &&
      !hadSliceContext
    ) {
      return;
    }

    this.diffOn = true;
    this.tab = 'transitions';
    await this.setSnapshot(target.id);
  }

  // Finds the snapshot corresponding to a per-process state slice (by seq_id
  // first, then by timestamp) and focuses the graph on that process.
  async loadForProcessSlice(slice: SliceContext) {
    this.sliceContext = slice;
    await this.ensureSnapshotsList();
    if (this.snapshots.length === 0) return;

    let target: SnapshotInfo | undefined;
    if (slice.seqId !== null && slice.seqId !== undefined) {
      target = this.snapshots.find((s) => s.seqId === slice.seqId);
    }
    if (target === undefined) {
      // Find the latest snapshot at or before slice.ts (or the earliest if slice.ts precedes all).
      for (const s of this.snapshots) {
        if (s.ts <= slice.ts) {
          target = s;
        } else {
          break;
        }
      }
      if (target === undefined) {
        target = this.snapshots[0];
      }
    }

    this.diffOn = true;
    await this.setSnapshot(target.id, slice.pid);
  }

  // Move to a snapshot AND highlight its instant on the "By reason" track, so
  // the pass navigation buttons and the timeline selection stay in sync.
  goToSnapshot(id: number) {
    this.sliceContext = undefined;
    const snap = this.snapshotOf(id);
    this.setSnapshot(id).catch((e) => console.error('ProcessState', e));
    const eventId = snap?.seqId ?? id;
    this.trace.selection.selectTrackEvent(REASON_TRACK_URI, eventId);
  }

  async setSnapshot(id: number, focusPid?: number) {
    this.snapshotId = id;
    const keepPid = focusPid;
    this.selectedPid = keepPid;
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
    await this.loadPassTriggers(id, token);
    if (token !== this.loadToken) return;
    await this.refreshDiff(token);
    if (token !== this.loadToken) return;
    if (keepPid !== undefined) {
      this.tab = 'current';
      await this.loadSelected(keepPid);
    } else if (this.tab === 'current') {
      this.tab = 'transitions';
    }
    m.redraw();
  }

  prevSnapshotId(id: number): number | undefined {
    const i = this.snapshots.findIndex((s) => s.id === id);
    if (i > 0) return this.snapshots[i - 1].id;
    if (
      i === 0 &&
      this.initialBaselineId !== undefined &&
      this.initialBaselineId !== id
    ) {
      return this.initialBaselineId;
    }
    return undefined;
  }

  nextSnapshotId(id: number): number | undefined {
    const i = this.snapshots.findIndex((s) => s.id === id);
    return i >= 0 && i < this.snapshots.length - 1
      ? this.snapshots[i + 1].id
      : undefined;
  }

  // ---- selection ----

  select(pid: number) {
    if (this.selectedPid === pid && this.selectedEdge === undefined) {
      this.selectedPid = undefined;
      this.clearSelectedData();
      this.tab = 'transitions';
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
      this.tab = this.selectedPid !== undefined ? 'current' : 'transitions';
      m.redraw();
      return;
    }
    this.selectedEdge = e;
    this.buildEdge(e);
    this.tab = 'current';
    m.redraw();
    this.loadEdgeDetails(e)
      .then(() => m.redraw())
      .catch((err) => console.error('ProcessState', err));
  }

  deselect() {
    this.selectedPid = undefined;
    this.selectedEdge = undefined;
    this.clearSelectedData();
    this.tab = 'transitions';
    m.redraw();
  }

  setTab(tab: 'transitions' | 'triggers' | 'current' | 'procs') {
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
    this.sliceInfoRows = [];
    this.stateRows = [];
    this.procTriggerRows = [];
    this.hostedSvc = this.hostedProv = [];
    this.outAll = this.inAll = this.selfAll = [];
    this.edgeProvRows = this.edgeSvcRows = [];
    this.sliceInfoDs = this.stateDs = this.procTriggerDs = undefined;
    this.hostedSvcDs = this.hostedProvDs = undefined;
    this.outDs = this.inDs = this.selfDs = undefined;
    this.edgeProvDs = this.edgeSvcDs = undefined;
  }

  private async loadPassTriggers(id: number, token: number) {
    const snap = this.snapshotOf(id);
    if (snap === undefined) {
      this.transitionRows = [];
      this.passInfoRows = [];
      this.triggerRows = [];
      this.transitionDs = undefined;
      this.passInfoDs = undefined;
      this.triggerDs = undefined;
      return;
    }

    if (snap.seqId !== undefined) {
      const transRes = await this.trace.engine.tryQuery(
        transitionsSql(`i.seq_id = ${snap.seqId} ORDER BY i.ts, i.id`),
      );
      if (token !== this.loadToken) return;
      this.transitionRows = transRes.ok ? this.rowsOf(transRes.value) : [];
    } else {
      this.transitionRows = [];
    }
    this.transitionDs = new InMemoryDataSource(this.transitionRows);

    const prevId = this.prevSnapshotId(id);
    const prevSnap = prevId !== undefined ? this.snapshotOf(prevId) : undefined;
    const prevTs = prevSnap ? prevSnap.ts : snap.ts - 50_000_000n;
    const nextId = this.nextSnapshotId(id);
    const nextSnap = nextId !== undefined ? this.snapshotOf(nextId) : undefined;
    const upperTs = nextSnap ? nextSnap.ts : snap.ts + 10_000_000n;

    let tq;
    if (snap.seqId !== undefined) {
      tq = await this.trace.engine.query(`
        SELECT
          ts,
          kind,
          action,
          component_name AS component,
          target_pid,
          caller_pid,
          detail,
          service_id,
          bind_id,
          intent_bind_id,
          provider_id,
          seq_id
        FROM _ps_trigger_event
        WHERE (
          seq_id = ${snap.seqId}
          OR (seq_id IS NULL AND ts >= ${prevTs} AND ts <= ${snap.ts})
        )
        AND kind != 'oom_adjuster_pass'
        ORDER BY ts, id
      `);
    } else {
      tq = await this.trace.engine.query(`
        SELECT
          ts,
          kind,
          action,
          component_name AS component,
          target_pid,
          caller_pid,
          detail,
          service_id,
          bind_id,
          intent_bind_id,
          provider_id,
          seq_id
        FROM _ps_trigger_event
        WHERE ts >= ${prevTs} AND ts <= ${upperTs}
          AND kind != 'oom_adjuster_pass'
        ORDER BY ts, id
      `);
    }
    if (token !== this.loadToken) return;
    this.triggerRows = this.rowsOf(tq);
    this.triggerDs = new InMemoryDataSource(this.triggerRows);

    const info: Row[] = [
      {property: 'snapshot id', value: String(snap.id)},
      {property: 'timestamp (ns)', value: String(snap.ts)},
      {property: 'oom adj reason', value: snap.reason ?? 'dump'},
      {
        property: 'adj seq_id',
        value: snap.seqId !== undefined ? String(snap.seqId) : 'NULL',
      },
      {
        property: 'update type',
        value:
          snap.isFullUpdate === undefined
            ? 'NULL'
            : snap.isFullUpdate
              ? 'full update'
              : 'partial (target Set)',
      },
      {
        property: 'top process',
        value:
          snap.topPid !== undefined
            ? `${this.nameOf(snap.topPid)} (pid ${snap.topPid})`
            : 'NULL',
      },
      {
        property: 'top proc state',
        value: snap.topProcState ?? 'NULL',
      },
      {
        property: 'enqueued target pids',
        value: snap.targetPids ?? 'all',
      },
    ];
    const provTriggers = this.triggerRows.filter((r) =>
      String(r['kind'] ?? '').startsWith('provider_'),
    );
    if (provTriggers.length > 0) {
      const auths = Array.from(
        new Set(
          provTriggers
            .map((r) => String(r['component'] ?? ''))
            .filter((s) => s.length > 0),
        ),
      );
      const details = Array.from(
        new Set(
          provTriggers
            .map((r) => String(r['detail'] ?? ''))
            .filter((s) => s.length > 0),
        ),
      );
      if (auths.length > 0) {
        info.push({property: 'provider authority', value: auths.join(', ')});
      }
      if (details.length > 0) {
        info.push({property: 'provider details', value: details.join(', ')});
      }
    } else if (this.triggerRows.length > 0) {
      const comps = Array.from(
        new Set(
          this.triggerRows
            .map((r) => String(r['component'] ?? ''))
            .filter((s) => s.length > 0),
        ),
      );
      if (comps.length > 0) {
        info.push({property: 'trigger component', value: comps.join(', ')});
      }
    }
    this.passInfoRows = info;
    this.passInfoDs = new InMemoryDataSource(info);
  }

  // Recomputes graphProcesses / diffNodes / the process-list rows for the
  // current diff state, and also populates passChangeRows for the current pass.
  private async refreshDiff(token: number) {
    const id = this.snapshotId;
    if (id === undefined) return;

    // Always compute changes relative to the baseline (or immediate prev snapshot)
    const compareId = this.baselineId ?? this.prevSnapshotId(id);
    if (compareId === undefined) {
      const nodes = new Map<number, DiffStatus>();
      for (const tr of this.transitionRows) {
        const pid = Number(tr['pid']);
        if (Number.isFinite(pid) && pid > 0) {
          nodes.set(pid, 'changed');
        }
      }
      this.diffNodes = this.diffOn ? nodes : new Map();
      this.graphProcesses = this.processes;
      this.procDs = new InMemoryDataSource(this.processes);
      this.passChangeRows = [];
      this.passChangeDs = new InMemoryDataSource([]);
      return;
    }

    const bq = await this.trace.engine.query(
      `SELECT * FROM _ps_process WHERE snapshot_id = ${compareId}`,
    );
    if (token !== this.loadToken) return;
    const base = this.rowsOf(bq);
    const baseByPid = new Map(base.map((r) => [Number(r['pid']), r]));
    const curByPid = new Map(this.processes.map((r) => [Number(r['pid']), r]));
    const nodes = new Map<number, DiffStatus>();
    const listRows: Row[] = [];
    const changes: Row[] = [];

    for (const r of this.processes) {
      const pid = Number(r['pid']);
      const b = baseByPid.get(pid);
      if (b === undefined) {
        nodes.set(pid, 'added');
        listRows.push({...r});
        changes.push({
          pid,
          name: r['name'],
          change: 'added',
          proc_state: fmt(r['proc_state']),
          oom_score: fmt(r['oom_score']),
          capabilities: fmt(r['capabilities']),
        });
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
      if (changed) {
        nodes.set(pid, 'changed');
        changes.push({
          pid,
          name: r['name'],
          change: 'changed',
          proc_state: out['proc_state'],
          oom_score: out['oom_score'],
          capabilities: out['capabilities'],
        });
      }
      listRows.push(this.diffOn ? out : {...r});
    }
    const removed: Row[] = [];
    for (const r of base) {
      if (!curByPid.has(Number(r['pid']))) {
        const pid = Number(r['pid']);
        nodes.set(pid, 'removed');
        removed.push(r);
        changes.push({
          pid,
          name: r['name'],
          change: 'removed',
          proc_state: `${fmt(r['proc_state'])} → EXITED`,
          oom_score: `${fmt(r['oom_score'])} → —`,
          capabilities: fmt(r['capabilities']),
        });
      }
    }

    if (this.baselineFollowsPrev) {
      for (const tr of this.transitionRows) {
        const pid = Number(tr['pid']);
        if (Number.isFinite(pid) && pid > 0 && !nodes.has(pid)) {
          nodes.set(pid, 'changed');
        }
      }
    }

    this.passChangeRows = changes;
    this.passChangeDs = new InMemoryDataSource(changes);

    if (!this.diffOn) {
      this.diffNodes = new Map();
      this.graphProcesses = this.processes;
      this.procDs = new InMemoryDataSource(this.processes);
      return;
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
      SELECT p.owning_pid AS server_pid, p.authority, p.component_name,
             MAX(pb.stable) AS stable, COUNT(*) AS n
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
      SELECT pb.client_pid, p.authority, p.component_name,
             MAX(pb.stable) AS stable, COUNT(*) AS n
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
      SELECT p.authority, p.component_name,
             MAX(pb.stable) AS stable, COUNT(*) AS n
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

    // Filter triggers directly involving this pid (or show all pass triggers if
    // this process was affected transitively via bindings).
    const directTriggers = this.triggerRows.filter(
      (r) => Number(r['target_pid']) === pid || Number(r['caller_pid']) === pid,
    );
    this.procTriggerRows =
      directTriggers.length > 0 ? directTriggers : this.triggerRows;
    this.procTriggerDs = new InMemoryDataSource(this.procTriggerRows);

    this.buildStateCard(pid);
    this.buildSliceInfoCard(pid);

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
    return this.rowsOf(q).map((b) => {
      const auth = String(b['authority'] ?? '');
      const comp = String(b['component_name'] ?? '');
      const name = isSvc
        ? String(b[nameCol] ?? '')
        : auth && comp && auth !== comp
          ? `${auth} (${comp})`
          : auth || comp || String(b[nameCol] ?? '');
      const fg = isSvc
        ? Number(b['fg'])
          ? 'fg'
          : ''
        : b['stable'] === null || b['stable'] === undefined
          ? ''
          : Number(b['stable'])
            ? 'stable'
            : 'unstable';
      return {
        pid: Number(b[pidCol] ?? 0),
        kind: isSvc ? 'service' : 'provider',
        name,
        fg,
        n: Number(b['n'] ?? 1),
      };
    });
  }
  private selfRows(
    q: ReturnType<Trace['engine']['query']> extends Promise<infer R>
      ? R
      : never,
    nameCol: string,
    isSvc: boolean,
  ): Row[] {
    return this.rowsOf(q).map((b) => {
      const auth = String(b['authority'] ?? '');
      const comp = String(b['component_name'] ?? '');
      const name = isSvc
        ? String(b[nameCol] ?? '')
        : auth && comp && auth !== comp
          ? `${auth} (${comp})`
          : auth || comp || String(b[nameCol] ?? '');
      const fg = isSvc
        ? Number(b['fg'])
          ? 'fg'
          : ''
        : b['stable'] === null || b['stable'] === undefined
          ? ''
          : Number(b['stable'])
            ? 'stable'
            : 'unstable';
      return {
        kind: isSvc ? 'service' : 'provider',
        name,
        fg,
        n: Number(b['n'] ?? 1),
      };
    });
  }

  private buildSliceInfoCard(pid: number) {
    const s = this.sliceContext;
    if (s === undefined || s.pid !== pid) {
      this.sliceInfoRows = [];
      this.sliceInfoDs = undefined;
      return;
    }
    const rows: Row[] = [
      {
        property: 'state transition',
        value: s.prevState ? `${s.prevState} → ${s.state}` : s.state,
      },
      {
        property: 'oom score transition',
        value:
          s.oomScore !== null && s.oomScore !== undefined
            ? s.prevOomScore !== null && s.prevOomScore !== undefined
              ? `${s.prevOomScore} → ${s.oomScore}`
              : String(s.oomScore)
            : 'NULL',
      },
      {property: 'oom change reason', value: s.reason ?? 'NULL'},
      {
        property: 'adj seq_id',
        value:
          s.seqId !== null && s.seqId !== undefined ? String(s.seqId) : 'NULL',
      },
      {property: 'slice ts (ns)', value: String(s.ts)},
      {property: 'slice dur (ns)', value: String(s.dur)},
    ];
    if (s.hostingType) {
      rows.push({property: 'hosting type', value: s.hostingType});
    }
    if (s.hostingName) {
      rows.push({property: 'hosting name', value: s.hostingName});
    }
    if (s.triggerType) {
      rows.push({property: 'trigger type', value: s.triggerType});
    }
    const provTriggers = this.procTriggerRows.filter((r) =>
      String(r['kind'] ?? '').startsWith('provider_'),
    );
    if (provTriggers.length > 0) {
      const auths = Array.from(
        new Set(
          provTriggers
            .map((r) => String(r['component'] ?? ''))
            .filter((x) => x.length > 0),
        ),
      );
      const details = Array.from(
        new Set(
          provTriggers
            .map((r) => String(r['detail'] ?? ''))
            .filter((x) => x.length > 0),
        ),
      );
      if (auths.length > 0) {
        rows.push({property: 'provider authority', value: auths.join(', ')});
      }
      if (details.length > 0) {
        rows.push({property: 'provider details', value: details.join(', ')});
      }
    }
    if (
      s.bindApplicationDelayMs !== null &&
      s.bindApplicationDelayMs !== undefined
    ) {
      rows.push({
        property: 'bindApplication delay (ms)',
        value: String(s.bindApplicationDelayMs),
      });
    }
    if (s.processStartDelayMs !== null && s.processStartDelayMs !== undefined) {
      rows.push({
        property: 'process start delay (ms)',
        value: String(s.processStartDelayMs),
      });
    }
    if (s.exitReason) {
      rows.push({property: 'exit reason', value: s.exitReason});
    }
    if (s.exitSubreason) {
      rows.push({property: 'exit subreason', value: s.exitSubreason});
    }
    this.sliceInfoRows = rows;
    this.sliceInfoDs = new InMemoryDataSource(rows);
  }

  private buildStateCard(pid: number) {
    const p = this.graphProcesses.find((r) => Number(r['pid']) === pid);
    const changeRow = this.passChangeRows.find((r) => Number(r['pid']) === pid);
    // Empty cells show the SQL value NULL (not "—"/"none"/"no") so a missing
    // value is visibly distinct from a real 0/false.
    const v = (k: string) => {
      if (this.diffOn && changeRow && changeRow[k] !== undefined) {
        return String(changeRow[k]);
      }
      return !p || p[k] === null || p[k] === undefined ? 'NULL' : String(p[k]);
    };
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
        client_name: this.nameOf(e.from),
        host_pid: e.to,
        host_name: this.nameOf(e.to),
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
    this.edgeProvRows = [];
    this.edgeProvDs = undefined;
    this.edgeSvcRows = [];
    this.edgeSvcDs = undefined;
  }

  private async loadEdgeDetails(e: EdgeSel) {
    const id = this.snapshotId;
    if (id === undefined) return;
    const token = this.loadToken;
    const snapIds =
      this.diffOn && this.baselineId !== undefined && this.baselineId !== id
        ? `${id}, ${this.baselineId}`
        : `${id}`;

    const provQ = await this.trace.engine.query(`
      SELECT
        pb.client_pid,
        p.owning_pid AS host_pid,
        COALESCE(p.authority, 'NULL') AS authority,
        COALESCE(p.component_name, 'NULL') AS component_name,
        CASE WHEN MAX(pb.stable) > 0 THEN 'stable' ELSE 'unstable' END AS stability,
        COUNT(DISTINCT pb.bind_id) AS connections
      FROM _ps_provider_binding pb
      JOIN _ps_provider p
        ON p.snapshot_id = pb.snapshot_id AND p.provider_id = pb.provider_id
      WHERE pb.snapshot_id IN (${snapIds})
        AND pb.client_pid = ${e.from}
        AND p.owning_pid = ${e.to}
      GROUP BY p.authority, p.component_name
    `);
    const svcQ = await this.trace.engine.query(`
      SELECT
        b.client_pid,
        s.owning_pid AS host_pid,
        COALESCE(s.name, 'NULL') AS service,
        CASE WHEN MAX(b.foreground) > 0 THEN 'yes' ELSE 'no' END AS foreground,
        COALESCE(MAX(b.flags), 'NULL') AS flags,
        COALESCE(MAX(b.intent_action), 'NULL') AS intent_action,
        COUNT(DISTINCT b.bind_id) AS connections
      FROM _ps_service_binding b
      JOIN _ps_service s
        ON s.snapshot_id = b.snapshot_id AND s.service_id = b.service_id
      WHERE b.snapshot_id IN (${snapIds})
        AND b.client_pid = ${e.from}
        AND s.owning_pid = ${e.to}
      GROUP BY s.name
    `);
    if (token !== this.loadToken) return;
    if (this.selectedEdge === undefined || !sameEdge(this.selectedEdge, e)) {
      return;
    }

    this.edgeProvRows = this.rowsOf(provQ).map((r) => ({
      client_pid: Number(r['client_pid']),
      client_name: this.nameOf(Number(r['client_pid'])),
      host_pid: Number(r['host_pid']),
      host_name: this.nameOf(Number(r['host_pid'])),
      authority: String(r['authority'] ?? 'NULL'),
      component_name: String(r['component_name'] ?? 'NULL'),
      stability: String(r['stability'] ?? 'stable'),
      connections: Number(r['connections'] ?? 1),
    }));
    this.edgeProvDs = new InMemoryDataSource(this.edgeProvRows);

    this.edgeSvcRows = this.rowsOf(svcQ).map((r) => ({
      client_pid: Number(r['client_pid']),
      client_name: this.nameOf(Number(r['client_pid'])),
      host_pid: Number(r['host_pid']),
      host_name: this.nameOf(Number(r['host_pid'])),
      service: String(r['service'] ?? 'NULL'),
      foreground: String(r['foreground'] ?? 'no'),
      flags: String(r['flags'] ?? 'NULL'),
      intent_action: String(r['intent_action'] ?? 'NULL'),
      connections: Number(r['connections'] ?? 1),
    }));
    this.edgeSvcDs = new InMemoryDataSource(this.edgeSvcRows);
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
