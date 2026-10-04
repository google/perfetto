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

// The Screen level state at one WM snapshot: the WM containers, the SF
// layers composited at that time, which process owns each window, and the
// trees and properties built from them. Kept as one immutable value so the
// current and the previous snapshot can be compared.

import type {Engine} from '../../trace_processor/engine';
import type {UiHierarchyNode, UiHierarchyWindow} from './ui_hierarchy_data';
import {
  type InputCanvas,
  inputCanvas,
  inputRows,
  windowInputLayer,
} from './ui_hierarchy_input';
import type {PropSection} from './ui_hierarchy_props';
import {
  buildProcessTreeNodes,
  displayRows,
  groupWindowsByProcess,
  packageOfTitle,
  type ProcessGroup,
  processNodeId,
  processRows,
  queryProcesses,
  resolveWindowOwner,
  type WindowOwner,
  wmNodeRows,
} from './ui_hierarchy_screen';
import {
  buildSfTreeNodes,
  countCompositions,
  indexAtOrBefore,
  linkWmWindows,
  querySfLayers,
  type SfCompositionCounts,
  type SfLayer,
  SfLayerIndex,
  sfLayerRows,
  sfNodeId,
  type SfSnapshot,
  type SfWindowLink,
  windowLayer,
  windowOfLayer,
} from './ui_hierarchy_sf';
import {
  lastTransitionOf,
  type Transition,
  transitionRows,
  transitionsAt,
} from './ui_hierarchy_transitions';
import {
  defaultDisplay,
  displayOf,
  matchUiWindow,
  queryWmDisplaySummaries,
  queryWmNodes,
  WM_KIND_DISPLAY,
  WM_KIND_WINDOW,
  type WmDisplaySummary,
  type WmSnapshot,
} from './ui_hierarchy_wm';

// The Screen level hierarchy tree: Process > Window, the raw
// WindowManager containers, or the SurfaceFlinger layers.
export type ScreenTreeMode = 'processes' | 'wm' | 'sf';

export interface ScreenStateParts {
  readonly ts: bigint;
  readonly nodes: ReadonlyArray<UiHierarchyNode>;
  readonly sfLayers: SfLayerIndex;
  // Whether the trace has SurfaceFlinger data at all.
  readonly hasSfData: boolean;
  // By display container token.
  readonly displaySummaries: ReadonlyMap<number, WmDisplaySummary>;
  // By WM window node id.
  readonly windowLinks: ReadonlyMap<string, SfWindowLink>;
  readonly windowOwners: ReadonlyMap<string, WindowOwner>;
  // android.ui.hierarchy windows, matched to WM windows.
  readonly uiWindows: ReadonlyArray<UiHierarchyWindow>;
  // All shell transitions of the trace.
  readonly transitions: ReadonlyArray<Transition>;
}

export class ScreenState {
  readonly ts: bigint;
  readonly nodes: ReadonlyArray<UiHierarchyNode>;
  readonly byNodeId: ReadonlyMap<string, UiHierarchyNode>;
  readonly sfLayers: SfLayerIndex;
  readonly hasSfData: boolean;
  private readonly parts: ScreenStateParts;
  private readonly treeCache = new Map<string, UiHierarchyNode[]>();
  private readonly inputCache = new Map<string, InputCanvas>();

  constructor(parts: ScreenStateParts) {
    this.parts = parts;
    this.ts = parts.ts;
    this.nodes = parts.nodes;
    this.byNodeId = new Map(parts.nodes.map((n) => [n.nodeId, n]));
    this.sfLayers = parts.sfLayers;
    this.hasSfData = parts.hasSfData;
  }

  static empty(): ScreenState {
    return new ScreenState({
      ts: 0n,
      nodes: [],
      sfLayers: new SfLayerIndex([]),
      hasSfData: false,
      displaySummaries: new Map(),
      windowLinks: new Map(),
      windowOwners: new Map(),
      uiWindows: [],
      transitions: [],
    });
  }

  // Whether any layer takes input.
  get hasInput(): boolean {
    for (const l of this.sfLayers.byId.values()) {
      if (l.input !== undefined) return true;
    }
    return false;
  }

  // WM displays.
  get displays(): UiHierarchyNode[] {
    return this.nodes.filter((n) => n.kind === WM_KIND_DISPLAY);
  }

  // Display `displayId`, else the one with the most visible windows.
  display(displayId: number | undefined): UiHierarchyNode | undefined {
    return (
      this.displays.find((d) => d.wm?.displayId === displayId) ??
      defaultDisplay(this.nodes)
    );
  }

  // All WM windows on `display`, back to front.
  displayWindows(display: UiHierarchyNode | undefined): UiHierarchyNode[] {
    return this.nodes.filter(
      (n) =>
        n.kind === WM_KIND_WINDOW &&
        (display === undefined ||
          displayOf(this.byNodeId, n.nodeId) === display),
    );
  }

  // The processes owning windows on `display`, topmost first.
  processGroups(display: UiHierarchyNode | undefined): ProcessGroup[] {
    return groupWindowsByProcess(this.displayWindows(display), (w) =>
      this.ownerOfWindow(w),
    );
  }

  // Process `key`, if it has windows on `display`.
  process(
    display: UiHierarchyNode | undefined,
    key: string | undefined,
  ): ProcessGroup | undefined {
    return key !== undefined
      ? this.processGroups(display).find((g) => g.owner.key === key)
      : undefined;
  }

  // The windows on `display` of process `key` (all if none), back to front.
  windows(
    display: UiHierarchyNode | undefined,
    key: string | undefined,
  ): UiHierarchyNode[] {
    const picked = this.process(display, key);
    return picked !== undefined
      ? [...picked.windows].reverse()
      : this.displayWindows(display);
  }

  // The process owning WM window `nodeId`.
  ownerOf(nodeId: string): WindowOwner | undefined {
    return this.parts.windowOwners.get(nodeId);
  }

  // Every window gets an owner when the state loads; the fallback keeps
  // this total.
  private ownerOfWindow(w: UiHierarchyNode): WindowOwner {
    return this.ownerOf(w.nodeId) ?? resolveWindowOwner({title: w.name}, []);
  }

  // The SF layers drawing WM window `nodeId`, if any.
  windowLinkFor(nodeId: string): SfWindowLink | undefined {
    return this.parts.windowLinks.get(nodeId);
  }

  // The UI hierarchy window drawn by WM node `nodeId`, if any.
  uiWindowFor(nodeId: string): UiHierarchyWindow | undefined {
    const n = this.byNodeId.get(nodeId);
    if (n === undefined) return undefined;
    const displayId = displayOf(this.byNodeId, nodeId)?.wm?.displayId;
    return matchUiWindow(n, this.parts.uiWindows, displayId);
  }

  // The WM window drawn by layer `layerId`, if any.
  windowOfLayer(layerId: number): UiHierarchyNode | undefined {
    const id = windowOfLayer(this.sfLayers, this.parts.windowLinks, layerId);
    return id !== undefined ? this.byNodeId.get(id) : undefined;
  }

  // The node standing for WM window `nodeId` in tree `mode`.
  treeNodeIdOf(mode: ScreenTreeMode, nodeId: string): string | undefined {
    if (mode !== 'sf') return nodeId;
    const link = this.windowLinkFor(nodeId);
    const layer = link !== undefined ? windowLayer(link.layers) : undefined;
    return layer !== undefined ? sfNodeId(layer.id) : undefined;
  }

  // The nodes of the hierarchy tree in `mode` for `display`, with only the
  // windows of process `key` if set.
  tree(
    mode: ScreenTreeMode,
    display: UiHierarchyNode | undefined,
    key: string | undefined,
  ): UiHierarchyNode[] {
    const picked = this.process(display, key);
    const cacheKey = [mode, display?.nodeId, picked?.owner.key].join('|');
    let nodes = this.treeCache.get(cacheKey);
    if (nodes === undefined) {
      nodes = this.buildTree(mode, display, picked);
      this.treeCache.set(cacheKey, nodes);
    }
    return nodes;
  }

  // The input windows on `display` as nodes of tree `mode` (see tree()).
  inputCanvas(
    mode: ScreenTreeMode,
    display: UiHierarchyNode | undefined,
    key: string | undefined,
  ): InputCanvas {
    const cacheKey = [mode, display?.nodeId, key].join('|');
    let canvas = this.inputCache.get(cacheKey);
    if (canvas === undefined) {
      const tree = new Map(
        this.tree(mode, display, key).map((n) => [n.nodeId, n]),
      );
      canvas = inputCanvas(this.displayLayers(display), (layer) => {
        if (mode === 'sf') return tree.get(sfNodeId(layer.id));
        const w = this.windowOfLayer(layer.id);
        return w !== undefined ? tree.get(w.nodeId) : undefined;
      });
      this.inputCache.set(cacheKey, canvas);
    }
    return canvas;
  }

  private buildTree(
    mode: ScreenTreeMode,
    display: UiHierarchyNode | undefined,
    picked: ProcessGroup | undefined,
  ): UiHierarchyNode[] {
    switch (mode) {
      case 'processes':
        return buildProcessTreeNodes(
          picked !== undefined ? [picked] : this.processGroups(display),
          this.ts,
        );
      case 'sf':
        return this.buildSfTree(display, picked);
      case 'wm':
        return this.buildWmTree(display, picked);
    }
  }

  // The display's containers, and with a process picked, only that
  // process' windows and their ancestors.
  private buildWmTree(
    display: UiHierarchyNode | undefined,
    picked: ProcessGroup | undefined,
  ): UiHierarchyNode[] {
    const keep = new Set<string>();
    for (const w of picked?.windows ?? []) {
      for (
        let cur: UiHierarchyNode | undefined = w;
        cur !== undefined && !keep.has(cur.nodeId);
        cur =
          cur.parentNodeId !== undefined
            ? this.byNodeId.get(cur.parentNodeId)
            : undefined
      ) {
        keep.add(cur.nodeId);
      }
    }
    return this.nodes.filter(
      (n) =>
        (display === undefined ||
          displayOf(this.byNodeId, n.nodeId) === display) &&
        (picked === undefined ||
          n.kind === WM_KIND_DISPLAY ||
          keep.has(n.nodeId)),
    );
  }

  // The display's layers then the offscreen ones, and with a process
  // picked, only its windows' layers with their ancestors and descendants.
  private buildSfTree(
    display: UiHierarchyNode | undefined,
    picked: ProcessGroup | undefined,
  ): UiHierarchyNode[] {
    const index = this.sfLayers;
    let keep: Set<number> | undefined;
    if (picked !== undefined) {
      const ids: number[] = [];
      for (const w of picked.windows) {
        const layers = this.windowLinkFor(w.nodeId)?.layers;
        if (layers?.container !== undefined) ids.push(layers.container.id);
        if (layers?.buffer !== undefined) ids.push(layers.buffer.id);
      }
      keep = index.withContext(ids);
    }
    const roots = [
      ...index.displayRoots(display?.wm?.layerId),
      ...index.offscreenRoots,
    ];
    return buildSfTreeNodes(index, roots, this.ts, keep);
  }

  // The layers drawn on `display`, in paint order.
  displayLayers(display: UiHierarchyNode | undefined): SfLayer[] {
    const index = this.sfLayers;
    return index
      .displayRoots(display?.wm?.layerId)
      .flatMap((r) => index.subtree(r));
  }

  // The WM summary of `display`, if known.
  displaySummary(display: UiHierarchyNode): WmDisplaySummary | undefined {
    return display.wm !== undefined
      ? this.parts.displaySummaries.get(display.wm.token)
      : undefined;
  }

  // How the layers on `display` were composed, with SurfaceFlinger data.
  displayLayerCounts(
    display: UiHierarchyNode,
  ): SfCompositionCounts | undefined {
    return this.hasSfData
      ? countCompositions(this.displayLayers(display))
      : undefined;
  }

  // The SF layer behind tree node `n`, if it is one.
  layerOf(n: UiHierarchyNode): SfLayer | undefined {
    return n.sfLayerId !== undefined
      ? this.sfLayers.byId.get(n.sfLayerId)
      : undefined;
  }

  // The layers whose transactions describe tree node `n`: a layer itself,
  // or a WM window's leash, container and buffer layers. Empty for others.
  transactionLayerIds(n: UiHierarchyNode): Set<number> {
    const layer = this.layerOf(n);
    if (layer !== undefined) return new Set([layer.id]);
    const ids = new Set<number>();
    if (n.kind === WM_KIND_WINDOW) {
      const layers = this.windowLinkFor(n.nodeId)?.layers;
      for (const l of [layers?.leash, layers?.container, layers?.buffer]) {
        if (l !== undefined) ids.add(l.id);
      }
    }
    return ids;
  }

  // The WindowManager and SurfaceFlinger properties of WM window `node`.
  windowSections(node: UiHierarchyNode): PropSection[] {
    const sections: PropSection[] = [
      {title: 'Window', rows: wmNodeRows(node, this.ownerOf(node.nodeId))},
    ];
    const layers = this.windowLinkFor(node.nodeId)?.layers;
    const layer = layers !== undefined ? windowLayer(layers) : undefined;
    if (layers !== undefined && layer !== undefined) {
      sections.push({
        title: 'SurfaceFlinger',
        rows: sfLayerRows(this.sfLayers, layer, layers),
      });
    }
    const input =
      layers !== undefined ? windowInputLayer(layers)?.input : undefined;
    if (input !== undefined) {
      sections.push({title: 'Input', rows: inputRows(input, false)});
    }
    sections.push(...this.transitionSections(layers?.container?.id));
    return sections;
  }

  // The last transition at or before this snapshot that moved layer
  // `layerId` or one of its ancestors (participants are usually tasks).
  private transitionSections(layerId: number | undefined): PropSection[] {
    if (layerId === undefined) return [];
    const ids = new Set<number>();
    for (
      let l = this.sfLayers.byId.get(layerId);
      l !== undefined && !ids.has(l.id);
      l =
        l.parentId !== undefined
          ? this.sfLayers.byId.get(l.parentId)
          : undefined
    ) {
      ids.add(l.id);
    }
    const hit = lastTransitionOf(this.parts.transitions, ids, this.ts);
    if (hit === undefined) return [];
    const rows = transitionRows(
      hit.transition,
      hit.participant,
      this.ts,
      (id) => this.sfLayers.byId.get(id)?.name,
    );
    return [{title: 'Transition', rows}];
  }

  // The transitions running at this snapshot.
  private runningTransitionSections(): PropSection[] {
    return transitionsAt(this.parts.transitions, this.ts).map((t) => ({
      title: 'Transition',
      rows: transitionRows(
        t,
        undefined,
        this.ts,
        (id) => this.sfLayers.byId.get(id)?.name,
      ),
    }));
  }

  // The properties of tree node `n` on `display`: a process, a layer, a WM
  // window or container, or the display.
  sections(
    n: UiHierarchyNode,
    display: UiHierarchyNode | undefined,
  ): PropSection[] {
    const process = this.processGroups(display).find(
      (g) => processNodeId(g.owner) === n.nodeId,
    );
    if (process !== undefined) {
      return [{title: 'Process', rows: processRows(process)}];
    }
    const layer = this.layerOf(n);
    if (layer !== undefined) {
      const sections: PropSection[] = [
        {title: 'SurfaceFlinger', rows: sfLayerRows(this.sfLayers, layer)},
      ];
      if (layer.input !== undefined) {
        sections.push({title: 'Input', rows: inputRows(layer.input, true)});
      }
      sections.push(...this.transitionSections(layer.id));
      return sections;
    }
    switch (n.kind) {
      case WM_KIND_WINDOW:
        return this.windowSections(n);
      case WM_KIND_DISPLAY:
        return [
          {
            title: 'Display',
            rows: displayRows(
              this.displaySummary(n),
              this.displayLayerCounts(n),
            ),
          },
          ...this.runningTransitionSections(),
        ];
      default:
        return [{title: n.kindName, rows: wmNodeRows(n, undefined)}];
    }
  }
}

// Loads the Screen level state at WM snapshot `snap`, with the SF snapshot
// composited at that time (the last one at or before it).
export async function loadScreenState(
  engine: Engine,
  snap: WmSnapshot,
  sfSnapshots: ReadonlyArray<SfSnapshot>,
  uiWindows: ReadonlyArray<UiHierarchyWindow>,
  transitions: ReadonlyArray<Transition>,
): Promise<ScreenState> {
  const sfSnap =
    sfSnapshots.length > 0
      ? sfSnapshots[indexAtOrBefore(sfSnapshots, snap.ts)]
      : undefined;
  const [nodes, sfLayers, displaySummaries] = await Promise.all([
    queryWmNodes(engine, snap),
    sfSnap !== undefined
      ? querySfLayers(engine, sfSnap).then((l) => new SfLayerIndex(l))
      : new SfLayerIndex([]),
    queryWmDisplaySummaries(engine, snap),
  ]);
  const byNodeId = new Map(nodes.map((n) => [n.nodeId, n]));
  const windows = nodes.filter((n) => n.kind === WM_KIND_WINDOW);
  const windowLinks = linkWmWindows(windows, sfLayers);
  const signals = windows.map((w) => ({
    nodeId: w.nodeId,
    title: w.name,
    uiWindow: matchUiWindow(
      w,
      uiWindows,
      displayOf(byNodeId, w.nodeId)?.wm?.displayId,
    ),
    appUid: windowLinks.get(w.nodeId)?.appUid,
  }));
  const uids = new Set<number>();
  const packages = new Set<string>();
  for (const sig of signals) {
    if (sig.appUid !== undefined) uids.add(sig.appUid);
    const pkg = packageOfTitle(sig.title);
    if (pkg !== undefined) packages.add(pkg);
  }
  const processes = await queryProcesses(engine, [...uids], [...packages]);
  return new ScreenState({
    ts: snap.ts,
    nodes,
    sfLayers,
    hasSfData: sfSnapshots.length > 0,
    displaySummaries,
    windowLinks,
    windowOwners: new Map(
      signals.map((sig) => [sig.nodeId, resolveWindowOwner(sig, processes)]),
    ),
    uiWindows,
    transitions,
  });
}
