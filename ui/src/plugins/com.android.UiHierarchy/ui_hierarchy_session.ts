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
import {Time} from '../../base/time';
import type {Trace} from '../../public/trace';
import QueryPagePlugin from '../dev.perfetto.QueryPage';
import {
  computeVersionDiffs,
  type NodeVersionChange,
  queryNodesAt,
  queryNodeVersions,
  querySnapshots,
  queryWindows,
  type UiHierarchyNode,
  type UiHierarchySnapshot,
  type UiHierarchyWindow,
} from './ui_hierarchy_data';
import {
  diffBaseIndex,
  diffSections,
  diffTrees,
  NO_DIFF,
  type TreeDiff,
} from './ui_hierarchy_diff';
import type {InputCanvas} from './ui_hierarchy_input';
import {
  displayName,
  type PropSection,
  type PropTarget,
  uiNodeRows,
} from './ui_hierarchy_props';
import {
  type ProcessGroup,
  processNodeId,
  type WindowOwner,
} from './ui_hierarchy_screen';
import {
  loadScreenState,
  ScreenState,
  type ScreenTreeMode,
} from './ui_hierarchy_screen_state';
import {
  indexAtOrBefore,
  querySfSnapshots,
  type SfLayer,
  sfNodeId,
  type SfSnapshot,
} from './ui_hierarchy_sf';
import {
  hasTransactions,
  layerTransactionRows,
  queryTransactionDetails,
  queryTransactionLog,
  senderRows,
  type TransactionDetails,
  type TransactionLog,
  transactionSection,
  transactionsQuery,
} from './ui_hierarchy_transactions';
import {
  queryTransitions,
  type Transition,
  type TransitionSpan,
  transitionSpans,
  transitionStart,
  transitionTypeName,
} from './ui_hierarchy_transitions';
import {
  displayOf,
  queryWmSnapshots,
  WM_KIND_DISPLAY,
  WM_KIND_WINDOW,
  type WmSnapshot,
} from './ui_hierarchy_wm';

export type {ScreenTreeMode} from './ui_hierarchy_screen_state';

export interface UiHierarchyViewOptions {
  projection3d: boolean;
  onlyVisible: boolean;
  onlyClickable: boolean;
  shading: 'solid' | 'gradient' | 'wireframe';
  explode: number; // 0..1
  rotation: number; // 0..1
  searchQuery: string;
  kindFilter?: number;
  // Mark what changed since the previous snapshot.
  showDiff: boolean;
  // With showDiff: only the changed nodes (and their ancestors).
  changedOnly: boolean;
  // Window level: draw the other on-screen windows as faint outlines.
  showScreenContext: boolean;
  // Screen level: draw the input windows and their touchable regions
  // instead of the windows (or layers).
  showInput: boolean;
}

function defaultOptions(): UiHierarchyViewOptions {
  return {
    projection3d: false,
    onlyVisible: false,
    onlyClickable: false,
    shading: 'gradient',
    explode: 0.3,
    rotation: 0.5,
    searchQuery: '',
    kindFilter: undefined,
    showDiff: false,
    changedOnly: false,
    showScreenContext: true,
    showInput: false,
  };
}

// The nodes of the current snapshot at one level, plus the per-level view
// state (selection, filters, collapsed and hidden nodes), which is kept when
// moving between levels.
export class HierarchyLevel {
  nodes: ReadonlyArray<UiHierarchyNode> = [];
  byNodeId: ReadonlyMap<string, UiHierarchyNode> = new Map();
  selectedNodeId?: string;
  readonly options = defaultOptions();
  // Node ids collapsed in the hierarchy tree.
  readonly collapsed = new Set<string>();
  // Node ids hidden from the layout canvas (they stay in the tree, dimmed).
  // Ids persist across snapshots so hidden nodes stay hidden while scrubbing.
  readonly hidden = new Set<string>();

  setNodes(nodes: ReadonlyArray<UiHierarchyNode>): void {
    this.nodes = nodes;
    this.byNodeId = new Map(nodes.map((n) => [n.nodeId, n]));
  }

  get selectedNode(): UiHierarchyNode | undefined {
    return this.selectedNodeId !== undefined
      ? this.byNodeId.get(this.selectedNodeId)
      : undefined;
  }
}

// 'screen': all WindowManager windows. 'window': the UI hierarchy of one
// window.
export type UiHierarchyLevelKind = 'screen' | 'window';

// The nodes of a hierarchy tree.
export interface TreeSource {
  readonly nodes: ReadonlyArray<UiHierarchyNode>;
  readonly byNodeId: ReadonlyMap<string, UiHierarchyNode>;
}

export class UiHierarchySession {
  readonly trace: Trace;
  level: UiHierarchyLevelKind = 'window';

  // Window level.
  windows: UiHierarchyWindow[] = [];
  selectedProcessUpid?: number;
  selectedWindowKey?: string;
  snapshots: UiHierarchySnapshot[] = [];
  index = 0;
  readonly win = new HierarchyLevel();
  nodeVersions: UiHierarchyNode[] = [];
  nodeVersionChanges: NodeVersionChange[] = [];
  // The previous snapshot's nodes, when the diff is shown.
  private winPrevNodes?: ReadonlyArray<UiHierarchyNode>;
  private winDiffCache?: {
    readonly cur: ReadonlyArray<UiHierarchyNode>;
    readonly prev: ReadonlyArray<UiHierarchyNode>;
    readonly diff: TreeDiff;
  };

  // Screen level.
  wmSnapshots: WmSnapshot[] = [];
  wmIndex = 0;
  readonly screen = new HierarchyLevel();
  treeMode: ScreenTreeMode = 'processes';
  // The WM display id picked in the breadcrumb, if any.
  selectedDisplayId?: number;
  // The owner key of the process picked in the breadcrumb, if any.
  processFilter?: string;
  private treeSourceCache?: TreeSource;
  private screenDiffCache?: {
    readonly cur: ReadonlyArray<UiHierarchyNode>;
    readonly prev: ReadonlyArray<UiHierarchyNode>;
    readonly diff: TreeDiff;
  };
  sfSnapshots: SfSnapshot[] = [];
  transitions: Transition[] = [];
  // Whether the trace has SurfaceFlinger transactions.
  hasTransactionData = false;
  // The transactions of transactionRange, once loaded.
  private transactionLog?: TransactionLog;
  // What the transactions of the selected node set (see
  // transactionDetailsFor()), by range and layers.
  private transactionDetails?: {
    readonly key: string;
    readonly details: TransactionDetails;
  };
  private transactionDetailsLoading?: string;
  private transactionSectionCache?: {
    readonly node: UiHierarchyNode;
    readonly state: ScreenState;
    readonly log: TransactionLog;
    readonly details?: TransactionDetails;
    readonly since: string;
    readonly sections: PropSection[];
  };
  // With the diff shown, compare with the snapshot at or before this time
  // instead of the previous one (see compareTransition()).
  diffBase?: {readonly ts: bigint; readonly label: string};
  private spanCache?: {
    readonly snaps: ReadonlyArray<{readonly ts: bigint}>;
    readonly spans: TransitionSpan[];
  };
  // The current WM snapshot's state, and the previous one's when the diff
  // is shown.
  private state = ScreenState.empty();
  private prevState?: ScreenState;

  loading = false;
  playing = false;
  playbackRate = 1;
  private playTimer?: number;

  private loadToken = 0;
  private nodeToken = 0;
  private wmToken = 0;

  constructor(trace: Trace) {
    this.trace = trace;
    this.screen.options.onlyVisible = true;
  }

  get hasScreenLevel(): boolean {
    return this.wmSnapshots.length > 0;
  }

  // Whether the trace has anything to show: UI hierarchy windows, or
  // WindowManager state for the Screen level.
  get hasData(): boolean {
    return this.windows.length > 0 || this.hasScreenLevel;
  }

  get current(): HierarchyLevel {
    return this.level === 'screen' ? this.screen : this.win;
  }

  // ---------------------------------------------------------------------------
  // Timeline (for the current level)

  get timelineLength(): number {
    return this.level === 'screen'
      ? this.wmSnapshots.length
      : this.snapshots.length;
  }

  get timelineIndex(): number {
    return this.level === 'screen' ? this.wmIndex : this.index;
  }

  get currentTs(): bigint | undefined {
    return this.level === 'screen'
      ? this.wmSnapshots[this.wmIndex]?.ts
      : this.snapshots[this.index]?.ts;
  }

  // The transitions on the current level's timeline.
  get timelineTransitions(): TransitionSpan[] {
    const snaps = this.level === 'screen' ? this.wmSnapshots : this.snapshots;
    if (this.spanCache?.snaps !== snaps) {
      this.spanCache = {snaps, spans: transitionSpans(this.transitions, snaps)};
    }
    return this.spanCache.spans;
  }

  private timelineTs(i: number): bigint | undefined {
    return this.level === 'screen'
      ? this.wmSnapshots[i]?.ts
      : this.snapshots[i]?.ts;
  }

  async setIndex(i: number): Promise<void> {
    if (this.level === 'screen') {
      await this.loadWmIndex(i);
    } else {
      await this.loadIndex(i, ++this.loadToken);
    }
  }

  async prevSnapshot(): Promise<void> {
    if (this.timelineIndex > 0) await this.setIndex(this.timelineIndex - 1);
  }

  async nextSnapshot(): Promise<void> {
    if (this.timelineIndex < this.timelineLength - 1) {
      await this.setIndex(this.timelineIndex + 1);
    }
  }

  togglePlay(): void {
    if (this.playing) {
      this.pause();
    } else {
      this.play();
    }
  }

  play(): void {
    if (this.timelineLength <= 1) return;
    this.playing = true;
    this.scheduleNextPlayFrame();
    m.redraw();
  }

  pause(): void {
    this.playing = false;
    if (this.playTimer !== undefined) {
      clearTimeout(this.playTimer);
      this.playTimer = undefined;
    }
    m.redraw();
  }

  setPlaybackRate(rate: number): void {
    this.playbackRate = rate;
    if (this.playing) {
      if (this.playTimer !== undefined) {
        clearTimeout(this.playTimer);
      }
      this.scheduleNextPlayFrame();
    }
    m.redraw();
  }

  private scheduleNextPlayFrame(): void {
    if (!this.playing) return;
    const currentTs = this.timelineTs(this.timelineIndex);
    const nextTs = this.timelineTs(this.timelineIndex + 1);
    let delayMs = 250;
    if (currentTs !== undefined && nextTs !== undefined && nextTs > currentTs) {
      const deltaMs = Number(nextTs - currentTs) / 1_000_000;
      delayMs = Math.max(30, Math.min(1500, deltaMs / this.playbackRate));
    } else {
      delayMs = 250 / this.playbackRate;
    }

    this.playTimer = window.setTimeout(async () => {
      if (!this.playing) return;
      if (this.timelineIndex < this.timelineLength - 1) {
        await this.setIndex(this.timelineIndex + 1);
      } else {
        await this.setIndex(0);
      }
      this.scheduleNextPlayFrame();
    }, delayMs);
  }

  // ---------------------------------------------------------------------------
  // Window level

  get selectedWindow(): UiHierarchyWindow | undefined {
    return (
      this.windows.find((w) => w.key === this.selectedWindowKey) ??
      this.windows[0]
    );
  }

  get currentSnapshot(): UiHierarchySnapshot | undefined {
    return this.snapshots[this.index];
  }

  async init(): Promise<void> {
    this.windows = await queryWindows(this.trace.engine);
    this.wmSnapshots = await queryWmSnapshots(this.trace.engine);
    // Shell transitions come from their own data source, so they can be
    // present (and shown under the Window level scrubber) without WM data.
    this.transitions = await queryTransitions(this.trace.engine);

    if (this.hasScreenLevel) {
      [this.sfSnapshots, this.hasTransactionData] = await Promise.all([
        querySfSnapshots(this.trace.engine),
        hasTransactions(this.trace.engine),
      ]);
      this.level = 'screen';
      await this.loadWmIndex(0);
      return;
    }
    if (this.windows.length === 0) return;
    const def =
      this.windows.find((w) => w.title.includes('NotificationShade')) ??
      this.windows.find(
        (w) =>
          !w.title.includes('DropTarget') &&
          !w.title.includes('Overlay') &&
          !w.title.includes('ScreenDecor'),
      ) ??
      this.windows[0];
    await this.setWindow(def.key);
  }

  // Shows the UI hierarchy of window `key`, at the snapshot nearest to `ts`
  // (default: the current time).
  async setWindow(key: string, ts?: bigint): Promise<void> {
    this.pause();
    const prevTs = ts ?? this.currentTs;
    this.level = 'window';
    const token = ++this.loadToken;
    this.selectedWindowKey = key;
    const win = this.selectedWindow;
    if (!win) return;
    this.selectedProcessUpid = win.upid;

    const snapshots = await querySnapshots(
      this.trace.engine,
      win.upid,
      win.windowId,
    );
    if (token !== this.loadToken) return;
    this.snapshots = snapshots;

    if (prevTs !== undefined) {
      await this.setNearestTs(prevTs);
    } else {
      await this.loadIndex(0, token);
    }
  }

  async setNearestTs(ts: bigint): Promise<void> {
    if (this.snapshots.length === 0) return;
    await this.loadIndex(nearestIndex(this.snapshots, ts), ++this.loadToken);
  }

  private async loadIndex(i: number, token: number): Promise<void> {
    if (token !== this.loadToken) return;
    if (this.snapshots.length === 0) {
      this.index = 0;
      this.win.setNodes([]);
      this.loading = false;
      m.redraw();
      return;
    }

    this.index = Math.max(0, Math.min(i, this.snapshots.length - 1));
    const snap = this.snapshots[this.index];
    const win = this.selectedWindow;
    if (!win) return;

    this.loading = true;
    try {
      const baseIdx = this.win.options.showDiff
        ? diffBaseIndex(this.index, this.pinnedBaseIndex(this.snapshots))
        : undefined;
      const prevSnap =
        baseIdx !== undefined ? this.snapshots[baseIdx] : undefined;
      const [nodes, prevNodes] = await Promise.all([
        queryNodesAt(this.trace.engine, win.windowId, snap.ts),
        prevSnap !== undefined
          ? queryNodesAt(this.trace.engine, win.windowId, prevSnap.ts)
          : undefined,
      ]);
      if (token !== this.loadToken) return;

      this.win.setNodes(nodes);
      this.winPrevNodes = prevNodes;
      await this.syncScreenTo(snap.ts);
      if (token !== this.loadToken) return;
      await this.loadTransactionLog();
      if (token !== this.loadToken) return;

      // Preserve selected node if still present in this snapshot
      let targetNodeId = this.win.selectedNodeId;
      if (targetNodeId === undefined || !this.win.byNodeId.has(targetNodeId)) {
        // Find default visible node
        const def = nodes.find((n) => n.isVisible) ?? nodes[0];
        targetNodeId = def?.nodeId;
      }

      if (targetNodeId !== undefined) {
        await this.selectNode(targetNodeId);
      } else {
        this.win.selectedNodeId = undefined;
        this.nodeVersions = [];
        this.nodeVersionChanges = [];
      }
    } finally {
      if (token === this.loadToken) {
        this.loading = false;
      }
    }
    m.redraw();
  }

  // What changed in the open window's tree since the previous snapshot.
  get winDiff(): TreeDiff {
    const prev = this.winPrevNodes;
    if (prev === undefined || !this.win.options.showDiff) return NO_DIFF;
    const cur = this.win.nodes;
    if (this.winDiffCache?.cur !== cur || this.winDiffCache.prev !== prev) {
      const diff = diffTrees(prev, cur, (n) => uiNodeSections(n));
      this.winDiffCache = {cur, prev, diff};
    }
    return this.winDiffCache.diff;
  }

  // The properties of View/Compose node `n`, with the changes since the
  // previous snapshot when the diff is shown.
  winSections(n: UiHierarchyNode): PropSection[] {
    const cur = uiNodeSections(n);
    const before = this.win.options.showDiff
      ? this.winPrevNodes?.find((p) => p.nodeId === n.nodeId)
      : undefined;
    return before !== undefined
      ? diffSections(uiNodeSections(before), cur)
      : cur;
  }

  // What changed at the current level since the previous snapshot.
  get treeDiff(): TreeDiff {
    return this.level === 'screen' ? this.screenDiff : this.winDiff;
  }

  // Shows or hides the changes since the previous snapshot at the current
  // level, loading that snapshot. Hiding them also drops a pinned base.
  async setShowDiff(show: boolean): Promise<void> {
    this.current.options.showDiff = show;
    if (!show) this.diffBase = undefined;
    await this.setIndex(this.timelineIndex);
  }

  // The index on `snaps` of the pinned diff base, if any.
  private pinnedBaseIndex(
    snaps: ReadonlyArray<{readonly ts: bigint}>,
  ): number | undefined {
    const base = this.diffBase;
    if (base === undefined || snaps.length === 0) return undefined;
    // Before the first snapshot: nothing to compare with at this level.
    if (base.ts < snaps[0].ts) return undefined;
    return indexAtOrBefore(snaps, base.ts);
  }

  // The snapshot the current one is compared with on the current level:
  // the pinned base or the previous one. The diff shows what changed since
  // then, the Transactions section who changed it.
  private get comparisonBase():
    | {readonly index: number; readonly label: string; readonly pinned: boolean}
    | undefined {
    const snaps = this.level === 'screen' ? this.wmSnapshots : this.snapshots;
    const pinned = this.pinnedBaseIndex(snaps);
    const index = diffBaseIndex(this.timelineIndex, pinned);
    if (index === undefined) return undefined;
    return index === pinned && this.diffBase !== undefined
      ? {index, label: this.diffBase.label, pinned: true}
      : {index, label: 'previous', pinned: false};
  }

  // What the current snapshot is compared with when the diff is shown.
  get diffComparison():
    | {readonly index: number; readonly label: string; readonly pinned: boolean}
    | undefined {
    return this.current.options.showDiff ? this.comparisonBase : undefined;
  }

  // The time range of the transactions explaining the current snapshot:
  // after the comparison base, up to the current snapshot.
  private get transactionRange():
    {readonly after: bigint; readonly upTo: bigint} | undefined {
    const base = this.comparisonBase;
    const upTo = this.currentTs;
    if (base === undefined || upTo === undefined) return undefined;
    const after = this.timelineTs(base.index);
    return after !== undefined ? {after, upTo} : undefined;
  }

  // Loads the transactions of transactionRange, unless already loaded.
  private async loadTransactionLog(): Promise<void> {
    if (!this.hasTransactionData) return;
    const range = this.transactionRange;
    const log = this.transactionLog;
    if (range === undefined || isLogOf(log, range)) return;
    const next = await queryTransactionLog(
      this.trace.engine,
      range.after,
      range.upTo,
    );
    // Moved on meanwhile.
    const now = this.transactionRange;
    if (now !== undefined && isLogOf(next, now)) this.transactionLog = next;
  }

  // Who changed tree node `n` since the comparison base: the transactions
  // of its layers, or on a display, of every layer by sender.
  private transactionSections(n: UiHierarchyNode): PropSection[] {
    const base = this.comparisonBase;
    const range = this.transactionRange;
    const log = this.transactionLog;
    if (base === undefined || range === undefined || !isLogOf(log, range)) {
      return [];
    }
    const since = `snapshot ${base.index + 1} · ${base.label}`;
    if (n.kind === WM_KIND_DISPLAY) {
      return [transactionSection(senderRows(log, since))];
    }
    const ids = this.state.transactionLayerIds(n);
    if (ids.size === 0) return [];
    const details = this.transactionDetailsFor(range, ids);
    // Rebuilt only when something changed: the detail rows can be many.
    const c = this.transactionSectionCache;
    if (
      c?.node === n &&
      c.state === this.state &&
      c.log === log &&
      c.details === details &&
      c.since === since
    ) {
      return c.sections;
    }
    const rows = layerTransactionRows(log, ids, {
      since,
      transitions: this.transitions,
      layerName: (id) => this.state.sfLayers.byId.get(id)?.name,
      details,
    });
    const sections: PropSection[] = [
      {
        ...transactionSection(rows),
        action: {
          text: 'Query',
          title: 'Everything these transactions set, in a query results tab',
          target: {
            kind: 'query',
            title: `Transactions: ${displayName(n.name)}`,
            sql: transactionsQuery(ids, range.after, range.upTo),
          },
        },
      },
    ];
    this.transactionSectionCache = {
      node: n,
      state: this.state,
      log,
      details,
      since,
      sections,
    };
    return sections;
  }

  // What the transactions on `layerIds` in `range` set, once loaded;
  // starts loading it otherwise.
  private transactionDetailsFor(
    range: {readonly after: bigint; readonly upTo: bigint},
    layerIds: ReadonlySet<number>,
  ): TransactionDetails | undefined {
    const key = [
      range.after,
      range.upTo,
      ...[...layerIds].sort((a, b) => a - b),
    ].join(',');
    const cached = this.transactionDetails;
    if (cached?.key === key) return cached.details;
    if (this.transactionDetailsLoading === key) return undefined;
    this.transactionDetailsLoading = key;
    queryTransactionDetails(
      this.trace.engine,
      range.after,
      range.upTo,
      layerIds,
    ).then((details) => {
      if (this.transactionDetailsLoading !== key) return;
      this.transactionDetailsLoading = undefined;
      this.transactionDetails = {key, details};
      m.redraw();
    });
    return undefined;
  }

  // Shows what transition `span` changed: its last snapshot, compared with
  // the one before it started.
  async compareTransition(span: TransitionSpan): Promise<void> {
    const snaps = this.level === 'screen' ? this.wmSnapshots : this.snapshots;
    const before = snaps[Math.max(0, span.first - 1)];
    if (before === undefined) return;
    this.pause();
    this.diffBase = {
      ts: before.ts,
      label: `before ${transitionTypeName(span.transition.type)}`,
    };
    this.current.options.showDiff = true;
    await this.setIndex(span.last);
  }

  // Compares with the previous snapshot again.
  async unpinDiffBase(): Promise<void> {
    this.diffBase = undefined;
    await this.setIndex(this.timelineIndex);
  }

  // Shows the current snapshot's time on the timeline, flagged.
  showInTimeline(): void {
    const ts = this.currentTs;
    if (ts === undefined) return;
    const t = Time.fromRaw(ts);
    this.trace.notes.addNote({
      id: 'com.android.UiHierarchy#snapshot',
      timestamp: t,
      text: 'UI hierarchy snapshot',
    });
    this.trace.navigate('#!/viewer');
    this.trace.scrollTo({
      time: {
        start: Time.sub(t, 50_000_000n),
        end: Time.add(t, 50_000_000n),
        behavior: 'focus',
      },
    });
  }

  async selectNode(nodeId: string): Promise<void> {
    const token = ++this.nodeToken;
    this.win.selectedNodeId = nodeId;
    const win = this.selectedWindow;
    if (!win) return;

    const versions = await queryNodeVersions(
      this.trace.engine,
      win.windowId,
      nodeId,
    );
    if (token !== this.nodeToken) return;

    this.nodeVersions = versions;
    this.nodeVersionChanges = computeVersionDiffs(versions);
    m.redraw();
  }

  // ---------------------------------------------------------------------------
  // Screen level

  // WM displays of the current snapshot.
  get displays(): UiHierarchyNode[] {
    return this.state.displays;
  }

  // The display shown on the Screen level: the picked one, else the one with
  // the most visible windows.
  get screenDisplay(): UiHierarchyNode | undefined {
    return this.state.display(this.selectedDisplayId);
  }

  // All WM windows on the screen display, back to front.
  get displayWindows(): UiHierarchyNode[] {
    return this.state.displayWindows(this.screenDisplay);
  }

  // The windows on the screen display of the picked process (all if none),
  // back to front.
  get screenWindows(): UiHierarchyNode[] {
    return this.state.windows(this.screenDisplay, this.processFilter);
  }

  // Whether the current snapshot has input windows.
  get hasInputData(): boolean {
    return this.state.hasInput;
  }

  // The input windows on the screen display as nodes of the current tree.
  get screenInputCanvas(): InputCanvas {
    return this.state.inputCanvas(
      this.treeMode,
      this.screenDisplay,
      this.processFilter,
    );
  }

  // The processes owning windows on the screen display, topmost first.
  get processGroups(): ProcessGroup[] {
    return this.state.processGroups(this.screenDisplay);
  }

  // The picked process, if it has windows on the screen display (the
  // filter is kept while scrubbing to snapshots without it).
  get filteredProcess(): ProcessGroup | undefined {
    return this.state.process(this.screenDisplay, this.processFilter);
  }

  // The nodes of the Screen level hierarchy tree in the current mode.
  get screenTree(): TreeSource {
    return this.treeSource(
      this.state.tree(this.treeMode, this.screenDisplay, this.processFilter),
    );
  }

  private treeSource(nodes: ReadonlyArray<UiHierarchyNode>): TreeSource {
    if (this.treeSourceCache?.nodes !== nodes) {
      this.treeSourceCache = {
        nodes,
        byNodeId: new Map(nodes.map((n) => [n.nodeId, n])),
      };
    }
    return this.treeSourceCache;
  }

  // Whether the trace has SurfaceFlinger layers (the SurfaceFlinger mode).
  get hasSfData(): boolean {
    return this.sfSnapshots.length > 0;
  }

  // The nodes drawn on the Screen level canvas: windows, or in the
  // SurfaceFlinger mode the layers on the display with screen bounds.
  get screenCanvasNodes(): UiHierarchyNode[] {
    if (this.treeMode !== 'sf') return this.screenWindows;
    const onDisplay = new Set(
      this.state.displayLayers(this.screenDisplay).map((l) => l.id),
    );
    return this.screenTree.nodes.filter((n) => {
      const layer = this.state.layerOf(n);
      return (
        layer !== undefined &&
        onDisplay.has(layer.id) &&
        layer.screenBounds !== undefined
      );
    });
  }

  // The selected node of the Screen level tree, if any.
  get selectedScreenNode(): UiHierarchyNode | undefined {
    const id = this.screen.selectedNodeId;
    return id !== undefined
      ? (this.screenTree.byNodeId.get(id) ?? this.state.byNodeId.get(id))
      : undefined;
  }

  // The selected SF layer (SurfaceFlinger mode), if any.
  get selectedLayer(): SfLayer | undefined {
    const n = this.selectedScreenNode;
    return n !== undefined ? this.state.layerOf(n) : undefined;
  }

  // The selected WM window, or the one drawn by the selected layer.
  get selectedWindowNode(): UiHierarchyNode | undefined {
    const layer = this.selectedLayer;
    const id = this.screen.selectedNodeId;
    const n =
      layer !== undefined
        ? this.state.windowOfLayer(layer.id)
        : id !== undefined
          ? this.state.byNodeId.get(id)
          : undefined;
    return n?.kind === WM_KIND_WINDOW ? n : undefined;
  }

  // The selected process row (Processes mode), if any.
  get selectedProcess(): ProcessGroup | undefined {
    const id = this.screen.selectedNodeId;
    return this.processGroups.find((g) => processNodeId(g.owner) === id);
  }

  // The UI hierarchy window drawn by WM node `nodeId`, if any.
  uiWindowFor(nodeId: string): UiHierarchyWindow | undefined {
    return this.state.uiWindowFor(nodeId);
  }

  // The process owning WM window `nodeId`.
  ownerOf(nodeId: string): WindowOwner | undefined {
    return this.state.ownerOf(nodeId);
  }

  // The properties of Screen level node `n`, with the changes since the
  // previous snapshot when the diff is shown, and who made them.
  screenSections(n: UiHierarchyNode): PropSection[] {
    const cur = this.state.sections(n, this.screenDisplay);
    const prev = this.prevScreen;
    const before = prev?.tree.byNodeId.get(n.nodeId);
    const sections =
      prev !== undefined && before !== undefined
        ? diffSections(prev.state.sections(before, prev.display), cur)
        : cur;
    return [...sections, ...this.transactionSections(n)];
  }

  // The WM and SF properties of WM window `n` (Window level root node).
  windowSections(n: UiHierarchyNode): PropSection[] {
    return [...this.state.windowSections(n), ...this.transactionSections(n)];
  }

  // The previous snapshot's state, display and tree, when the diff is
  // shown.
  private get prevScreen():
    | {
        readonly state: ScreenState;
        readonly display: UiHierarchyNode | undefined;
        readonly tree: TreeSource;
      }
    | undefined {
    const state = this.prevState;
    if (state === undefined || !this.screen.options.showDiff) return undefined;
    const cur = this.screenDisplay;
    const display =
      (cur !== undefined ? state.byNodeId.get(cur.nodeId) : undefined) ??
      state.display(this.selectedDisplayId);
    const nodes = state.tree(this.treeMode, display, this.processFilter);
    return {
      state,
      display,
      tree: {nodes, byNodeId: new Map(nodes.map((n) => [n.nodeId, n]))},
    };
  }

  // What changed in the Screen level tree since the previous snapshot.
  get screenDiff(): TreeDiff {
    const prev = this.prevScreen;
    if (prev === undefined) return NO_DIFF;
    const cur = this.screenTree.nodes;
    if (
      this.screenDiffCache?.cur !== cur ||
      this.screenDiffCache.prev !== prev.tree.nodes
    ) {
      const display = this.screenDisplay;
      const diff = diffTrees(prev.tree.nodes, cur, (n, side) =>
        side === 'prev'
          ? prev.state.sections(n, prev.display)
          : this.state.sections(n, display),
      );
      this.screenDiffCache = {cur, prev: prev.tree.nodes, diff};
    }
    return this.screenDiffCache.diff;
  }

  setTreeMode(mode: ScreenTreeMode): void {
    // Windows and their layers stand for each other; process rows and
    // containers are only in their own tree.
    const window = this.selectedWindowNode;
    this.treeMode = mode;
    if (window !== undefined) {
      this.screen.selectedNodeId = this.state.treeNodeIdOf(mode, window.nodeId);
    }
    this.dropStaleSelection();
    m.redraw();
  }

  // Clears the selection if it is not in the current tree.
  private dropStaleSelection(): void {
    const sel = this.screen.selectedNodeId;
    if (sel !== undefined && !this.screenTree.byNodeId.has(sel)) {
      this.screen.selectedNodeId = undefined;
    }
  }

  // Shows display `displayId` on the Screen level.
  async selectDisplay(displayId: number | undefined): Promise<void> {
    if (this.level === 'window') await this.goToScreen();
    this.selectedDisplayId = displayId;
    this.processFilter = undefined;
    this.dropStaleSelection();
    m.redraw();
  }

  // Shows only the windows of process `key` (all if undefined).
  async setProcessFilter(key: string | undefined): Promise<void> {
    if (this.level === 'window') await this.goToScreen();
    this.processFilter = key;
    this.dropStaleSelection();
    m.redraw();
  }

  // Follows a link of the Properties pane.
  async followLink(target: PropTarget): Promise<void> {
    switch (target.kind) {
      case 'process':
        await this.setProcessFilter(target.key);
        return;
      case 'window':
        if (this.level === 'window') await this.goToScreen();
        this.selectScreenNode(target.nodeId);
        return;
      case 'layer': {
        if (this.level === 'window') await this.goToScreen();
        this.treeMode = 'sf';
        const id = sfNodeId(target.layerId);
        // Layers of other processes are hidden by the process filter.
        if (!this.screenTree.byNodeId.has(id)) this.processFilter = undefined;
        this.screen.selectedNodeId = id;
        m.redraw();
        return;
      }
      case 'transition':
        await this.goToTransition(target.transitionId);
        return;
      case 'query':
        this.trace.plugins
          .getPlugin(QueryPagePlugin)
          .addQueryResultsTab({query: target.sql, title: target.title});
        return;
    }
  }

  // Compares across transition `id` when it spans snapshots on this
  // level, else shows the snapshot it started after.
  private async goToTransition(id: number): Promise<void> {
    const span = this.timelineTransitions.find((s) => s.transition.id === id);
    if (span !== undefined) {
      await this.compareTransition(span);
      return;
    }
    const t = this.transitions.find((x) => x.id === id);
    const start = t !== undefined ? transitionStart(t) : undefined;
    const snaps = this.level === 'screen' ? this.wmSnapshots : this.snapshots;
    if (start === undefined || snaps.length === 0) return;
    this.pause();
    await this.setIndex(indexAtOrBefore(snaps, start));
  }

  // The WM window drawing the currently open UI hierarchy window.
  get wmNodeForSelectedWindow(): UiHierarchyNode | undefined {
    const key = this.selectedWindowKey;
    return this.state.nodes.find(
      (n) =>
        n.kind === WM_KIND_WINDOW && this.uiWindowFor(n.nodeId)?.key === key,
    );
  }

  // The display of the open window (Window level) or the Screen level one.
  get currentDisplay(): UiHierarchyNode | undefined {
    if (this.level === 'window') {
      const n = this.wmNodeForSelectedWindow;
      if (n !== undefined) return displayOf(this.state.byNodeId, n.nodeId);
    }
    return this.screenDisplay;
  }

  // Selects `nodeId` (a node of the current tree, or a WM window, which
  // stands for its layer in the SurfaceFlinger mode).
  selectScreenNode(nodeId: string | undefined): void {
    const display = displayOf(this.state.byNodeId, nodeId);
    if (display !== undefined) this.selectedDisplayId = display.wm?.displayId;
    const n =
      nodeId !== undefined ? this.state.byNodeId.get(nodeId) : undefined;
    this.screen.selectedNodeId =
      n?.kind === WM_KIND_WINDOW
        ? this.state.treeNodeIdOf(this.treeMode, n.nodeId)
        : nodeId;
    m.redraw();
  }

  // Why the selected window's View/Compose tree can't be shown, if so.
  get viewsUnavailableReason(): string | undefined {
    if (this.level === 'window') return undefined;
    const win = this.selectedWindowNode;
    if (win === undefined) {
      return 'Select a window to see its View/Compose tree';
    }
    if (this.uiWindowFor(win.nodeId) === undefined) {
      return `${win.name} has no android.ui.hierarchy data`;
    }
    return undefined;
  }

  // Opens the View/Compose tree of the selected window.
  async openSelected(): Promise<void> {
    const win = this.selectedWindowNode;
    if (win !== undefined) await this.openWindowFor(win.nodeId);
  }

  async openWindowFor(nodeId: string): Promise<void> {
    const uiWin = this.uiWindowFor(nodeId);
    if (uiWin === undefined) return;
    this.selectScreenNode(nodeId);
    await this.setWindow(uiWin.key, this.currentTs);
  }

  async goToScreen(): Promise<void> {
    if (!this.hasScreenLevel) return;
    this.pause();
    const ts = this.currentTs;
    this.level = 'screen';
    await this.loadWmIndex(
      ts !== undefined ? indexAtOrBefore(this.wmSnapshots, ts) : this.wmIndex,
    );
    const wmNode = this.wmNodeForSelectedWindow;
    if (wmNode !== undefined) this.selectScreenNode(wmNode.nodeId);
    m.redraw();
  }

  // Loads the WM state in effect at `ts` so screen context and the display
  // (which can change on fold/unfold or rotation) match the open window.
  private async syncScreenTo(ts: bigint): Promise<void> {
    if (!this.hasScreenLevel) return;
    const i = indexAtOrBefore(this.wmSnapshots, ts);
    if (i !== this.wmIndex || this.state.nodes.length === 0) {
      await this.loadWmIndex(i, /* quiet= */ true);
    }
    const wmNode = this.wmNodeForSelectedWindow;
    if (wmNode !== undefined) this.selectScreenNode(wmNode.nodeId);
  }

  private async loadWmIndex(i: number, quiet = false): Promise<void> {
    if (this.wmSnapshots.length === 0) return;
    const token = ++this.wmToken;
    this.wmIndex = Math.max(0, Math.min(i, this.wmSnapshots.length - 1));
    if (!quiet) this.loading = true;
    try {
      const load = (j: number) =>
        loadScreenState(
          this.trace.engine,
          this.wmSnapshots[j],
          this.sfSnapshots,
          this.windows,
          this.transitions,
        );
      const baseIdx = this.screen.options.showDiff
        ? diffBaseIndex(this.wmIndex, this.pinnedBaseIndex(this.wmSnapshots))
        : undefined;
      const [state, prevState] = await Promise.all([
        load(this.wmIndex),
        baseIdx !== undefined ? load(baseIdx) : undefined,
      ]);
      if (token !== this.wmToken) return;
      this.state = state;
      this.prevState = prevState;
      this.screen.setNodes(state.nodes);
      if (this.level === 'screen') await this.loadTransactionLog();
    } finally {
      if (token === this.wmToken && !quiet) this.loading = false;
    }
    m.redraw();
  }
}

function uiNodeSections(n: UiHierarchyNode): PropSection[] {
  return [{title: n.kindName, rows: uiNodeRows(n)}];
}

function isLogOf(
  log: TransactionLog | undefined,
  range: {readonly after: bigint; readonly upTo: bigint},
): log is TransactionLog {
  return log?.after === range.after && log.upTo === range.upTo;
}

function nearestIndex(items: ReadonlyArray<{ts: bigint}>, ts: bigint): number {
  let best = 0;
  let bestD = -1n;
  for (let i = 0; i < items.length; i++) {
    const t = items[i].ts;
    const d = t > ts ? t - ts : ts - t;
    if (bestD < 0n || d < bestD) {
      bestD = d;
      best = i;
    }
  }
  return best;
}
