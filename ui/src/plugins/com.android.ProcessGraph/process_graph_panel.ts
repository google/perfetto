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
import {AsyncMemo} from '../../base/async_memo';
import type {HighPrecisionTimeSpan} from '../../base/high_precision_time_span';
import {Icons} from '../../base/semantic_icons';
import {Duration, type duration, Time, type time} from '../../base/time';
import {GraphChart} from '../../components/widgets/charts/graph_chart';
import type {Selection} from '../../public/selection';
import type {Trace} from '../../public/trace';
import {Button, ButtonVariant} from '../../widgets/button';
import {Checkbox} from '../../widgets/checkbox';
import {Intent} from '../../widgets/common';
import {EmptyState} from '../../widgets/empty_state';
import {Section} from '../../widgets/section';
import {Select} from '../../widgets/select';
import {Spinner} from '../../widgets/spinner';
import {SplitPanel} from '../../widgets/split_panel';
import {TextInput} from '../../widgets/text_input';
import {Tree, TreeNode} from '../../widgets/tree';
import {
  buildProcessGraphChartData,
  buildProcessInfoFallback,
  type SelectedItem,
} from './chart_option';
import {
  type CollapsedEdge,
  type GraphSnapshot,
  loadProcessGraphModel,
  type ProcessGraphFilters,
  type ProcessGraphModel,
  snapshotAt,
  type StepMode,
  stepTimestamp,
} from './model';

export class ProcessGraphSession {
  readonly modelMemo = new AsyncMemo<ProcessGraphModel>();
  curTs?: time;
  stepMode: StepMode = 'edge';
  playSpeedMs = 350;
  syncTimeline = false;
  inspectorHeightPx = 210;
  filters: ProcessGraphFilters = {
    showServices: true,
    showProviders: true,
    showSelfLoops: false,
    showIsolated: false,
    searchQuery: '',
  };
  selected?: SelectedItem;

  dispose(): void {
    this.modelMemo.dispose();
  }
}

export interface ProcessGraphPanelAttrs {
  readonly trace: Trace;
  readonly session: ProcessGraphSession;
}

export function formatRelTime(delta: duration): string {
  if (delta <= 0n) return '0s';
  if (delta < 60_000_000_000n) return Duration.humanise(delta);
  return Duration.format((delta / 1_000_000_000n) * 1_000_000_000n);
}

interface TimelineSyncTarget {
  readonly ts?: time;
  readonly upid?: number;
}

function resolveTimelineSyncTarget(
  trace: Trace,
): TimelineSyncTarget | undefined {
  const sel = trace.selection.selection;
  if (sel.kind === 'track_event') {
    const upid = trace.tracks.getTrack(sel.trackUri)?.tags?.upid;
    return {ts: sel.ts, upid};
  }
  if (sel.kind === 'area') {
    const upids = new Set<number>();
    for (const t of sel.tracks) {
      if (t.tags?.upid !== undefined) {
        upids.add(t.tags.upid);
      }
    }
    return {
      ts: sel.start,
      upid: upids.size === 1 ? [...upids][0] : undefined,
    };
  }
  if (sel.kind === 'track') {
    const upid = trace.tracks.getTrack(sel.trackUri)?.tags?.upid;
    return upid !== undefined ? {upid} : undefined;
  }
  if (sel.kind === 'note') {
    const span = trace.selection.getTimeSpanOfSelection();
    return span !== undefined ? {ts: span.start} : undefined;
  }
  return undefined;
}

function jumpToTimeline(trace: Trace, ts: time, upid?: number): void {
  const trackUri =
    upid !== undefined
      ? trace.currentWorkspace.flatTracks.find(
          (t) =>
            t.uri !== undefined &&
            trace.tracks.getTrack(t.uri)?.tags?.upid === upid,
        )?.uri
      : undefined;
  if (trace.getCurrentRoute().page !== '/viewer') {
    trace.navigate('#!/viewer');
  }
  trace.scrollTo({
    time: {start: ts, behavior: 'pan'},
    track:
      trackUri !== undefined ? {uri: trackUri, expandGroup: true} : undefined,
  });
}

function renderSection(title: string, body: m.Children): m.Children {
  return m(
    Section,
    {title: m('h3.pf-process-graph__section-title', title)},
    body,
  );
}

export class ProcessGraphPanel implements m.ClassComponent<ProcessGraphPanelAttrs> {
  private isPlaying = false;
  private playTimer?: ReturnType<typeof setTimeout>;
  private resetGeneration = 0;
  private latestModel?: ProcessGraphModel;
  private canvasRedrawDisposer?: Disposable;
  private lastObservedWindow?: HighPrecisionTimeSpan;
  private lastProgrammaticWindow?: HighPrecisionTimeSpan;
  private lastObservedSelection?: Selection;
  private suppressViewportSyncUntilMs = 0;

  oncreate({attrs}: m.CVnodeDOM<ProcessGraphPanelAttrs>) {
    this.lastObservedWindow = attrs.trace.timeline.visibleWindow;
    this.lastObservedSelection = attrs.trace.selection.selection;
    this.canvasRedrawDisposer = attrs.trace.raf.addCanvasRedrawCallback(() =>
      this.onTimelineFrame(attrs.trace, attrs.session),
    );
  }

  onremove({attrs}: m.CVnodeDOM<ProcessGraphPanelAttrs>) {
    this.canvasRedrawDisposer?.[Symbol.dispose]();
    this.canvasRedrawDisposer = undefined;
    this.latestModel = undefined;
    this.stopPlay();
    attrs.trace.timeline.hoverCursorTimestamp = undefined;
  }

  private stopPlay(): void {
    this.isPlaying = false;
    if (this.playTimer !== undefined) {
      clearTimeout(this.playTimer);
      this.playTimer = undefined;
    }
  }

  private advanceTo(
    trace: Trace,
    session: ProcessGraphSession,
    nextTs: time,
    panTimeline = session.syncTimeline,
  ): void {
    session.curTs = nextTs;
    trace.timeline.hoverCursorTimestamp = nextTs;
    if (panTimeline) {
      trace.timeline.panIntoView(nextTs, {align: 'center', animation: 'step'});
      const updatedVis = trace.timeline.visibleWindow;
      this.lastProgrammaticWindow = updatedVis;
      this.lastObservedWindow = updatedVis;
    }
  }

  private onTimelineFrame(trace: Trace, session: ProcessGraphSession): void {
    const vis = trace.timeline.visibleWindow;
    const sel = trace.selection.selection;
    const model = this.latestModel;
    if (!session.syncTimeline || model === undefined) {
      this.lastObservedWindow = vis;
      this.lastObservedSelection = sel;
      return;
    }

    if (sel !== this.lastObservedSelection) {
      this.lastObservedSelection = sel;
      this.lastObservedWindow = vis;
      const target = resolveTimelineSyncTarget(trace);
      if (target?.ts !== undefined || target?.upid !== undefined) {
        this.suppressViewportSyncUntilMs = performance.now() + 400;
        this.stopPlay();
        let changed = false;
        if (target.ts !== undefined) {
          const clamped = Time.clamp(
            target.ts,
            model.traceStart,
            model.traceEnd,
          );
          if (session.curTs !== clamped) {
            this.advanceTo(trace, session, clamped, false);
            changed = true;
          }
        }
        if (
          target.upid !== undefined &&
          model.procsByUpid.has(target.upid) &&
          !(
            session.selected?.type === 'node' &&
            session.selected.upid === target.upid
          )
        ) {
          session.selected = {type: 'node', upid: target.upid};
          changed = true;
        }
        if (changed) {
          m.redraw();
        }
        return;
      }
    }

    if (
      this.lastObservedWindow === undefined ||
      !vis.equals(this.lastObservedWindow)
    ) {
      this.lastObservedWindow = vis;
      if (
        (this.lastProgrammaticWindow !== undefined &&
          vis.equals(this.lastProgrammaticWindow)) ||
        performance.now() < this.suppressViewportSyncUntilMs
      ) {
        return;
      }
      const midTs = Time.clamp(
        vis.midpoint.toTime('round'),
        model.traceStart,
        model.traceEnd,
      );
      if (session.curTs !== midTs) {
        this.stopPlay();
        this.advanceTo(trace, session, midTs, false);
        m.redraw();
      }
    }
  }

  private toggleTimelineSync(
    trace: Trace,
    session: ProcessGraphSession,
    model: ProcessGraphModel,
  ): void {
    session.syncTimeline = !session.syncTimeline;
    const vis = trace.timeline.visibleWindow;
    this.lastObservedWindow = vis;
    this.lastObservedSelection = trace.selection.selection;
    if (!session.syncTimeline) return;

    this.stopPlay();
    const selTarget = resolveTimelineSyncTarget(trace);
    const rawTs = selTarget?.ts ?? vis.midpoint.toTime('round');
    this.advanceTo(
      trace,
      session,
      Time.clamp(rawTs, model.traceStart, model.traceEnd),
      false,
    );
    if (
      selTarget?.upid !== undefined &&
      model.procsByUpid.has(selTarget.upid)
    ) {
      session.selected = {type: 'node', upid: selTarget.upid};
    }
  }

  private showOnTimeline(trace: Trace, ts: time, upid?: number): void {
    this.suppressViewportSyncUntilMs = performance.now() + 400;
    jumpToTimeline(trace, ts, upid);
  }

  private step(
    trace: Trace,
    session: ProcessGraphSession,
    model: ProcessGraphModel,
    dir: 1 | -1,
  ): boolean {
    const cur = session.curTs ?? model.traceStart;
    const nextTs = stepTimestamp(
      model,
      cur,
      dir,
      session.stepMode,
      session.filters,
    );
    if (nextTs === undefined) return false;
    this.advanceTo(trace, session, nextTs);
    return true;
  }

  private togglePlay(
    trace: Trace,
    session: ProcessGraphSession,
    model: ProcessGraphModel,
  ): void {
    if (this.isPlaying) {
      this.stopPlay();
      return;
    }
    const cur = session.curTs ?? model.traceStart;
    if (model.traceEnd - cur <= 500_000_000n) {
      session.curTs = model.traceStart;
    }
    this.isPlaying = true;
    const tick = () => {
      if (!this.isPlaying) return;
      if (!this.step(trace, session, model, 1)) {
        this.stopPlay();
        m.redraw();
        return;
      }
      m.redraw();
      this.playTimer = setTimeout(tick, session.playSpeedMs);
    };
    this.playTimer = setTimeout(tick, session.playSpeedMs);
  }

  private handleKeyDown(
    trace: Trace,
    session: ProcessGraphSession,
    model: ProcessGraphModel,
    ev: KeyboardEvent,
  ): void {
    if (ev.ctrlKey || ev.altKey || ev.metaKey) return;
    const target = ev.target as HTMLElement | undefined;
    const tag = target?.tagName ?? '';
    if (
      tag === 'INPUT' ||
      tag === 'SELECT' ||
      tag === 'BUTTON' ||
      tag === 'TEXTAREA' ||
      target?.isContentEditable === true
    ) {
      return;
    }
    if (ev.key === 'ArrowRight') {
      ev.preventDefault();
      this.stopPlay();
      this.step(trace, session, model, 1);
    } else if (ev.key === 'ArrowLeft') {
      ev.preventDefault();
      this.stopPlay();
      this.step(trace, session, model, -1);
    } else if (ev.key === 'Escape' && session.selected !== undefined) {
      session.selected = undefined;
    }
  }

  view({attrs}: m.CVnode<ProcessGraphPanelAttrs>) {
    const {trace, session} = attrs;
    const memoRes = session.modelMemo.use({
      key: {traceStart: trace.traceInfo.start},
      compute: (signal) => loadProcessGraphModel(trace, signal),
    });

    if (memoRes.isPending && memoRes.data === undefined) {
      return m(
        '.pf-process-graph',
        m('.pf-process-graph__loading', m(Spinner), 'Loading process graph...'),
      );
    }

    const model = memoRes.data;
    this.latestModel = model;
    if (model === undefined || model.connections.length === 0) {
      return m(
        '.pf-process-graph',
        m(EmptyState, {
          title: 'No Android service or content provider connections in trace',
        }),
      );
    }

    if (session.curTs === undefined) {
      session.curTs = model.edgeEvents[0] ?? model.traceStart;
    }
    const curTs = session.curTs;
    const snap = snapshotAt(model, curTs, session.filters);
    const activeRows = new Set(snap.nodes.map((n) => n.stateRank)).size;
    const durMs = Math.max(1, Number(model.traceEnd - model.traceStart) / 1e6);
    const curRelMs = Math.max(
      0,
      Math.min(durMs, Number(curTs - model.traceStart) / 1e6),
    );
    const chartData = buildProcessGraphChartData(model, snap, session.selected);

    const filterCheckbox = (
      label: string,
      key: 'showServices' | 'showProviders' | 'showSelfLoops' | 'showIsolated',
    ) =>
      m(Checkbox, {
        label,
        checked: session.filters[key],
        onchange: () => {
          session.filters = {
            ...session.filters,
            [key]: !session.filters[key],
          };
        },
      });

    return m(
      '.pf-process-graph',
      {
        tabindex: -1,
        onkeydown: (ev: KeyboardEvent) =>
          this.handleKeyDown(trace, session, model, ev),
      },
      m(
        'header.pf-process-graph__toolbar',
        m(
          '.pf-process-graph__toolbar-row',
          m(Button, {
            'icon': 'skip_previous',
            'title': 'Previous step',
            'aria-label': 'Previous step',
            'variant': ButtonVariant.Outlined,
            'compact': true,
            'onclick': () => {
              this.stopPlay();
              this.step(trace, session, model, -1);
            },
          }),
          m(Button, {
            'label': this.isPlaying ? 'Pause' : 'Play',
            'icon': this.isPlaying ? 'pause' : Icons.Play,
            'variant': ButtonVariant.Filled,
            'compact': true,
            'active': this.isPlaying,
            'aria-label': this.isPlaying ? 'Pause timeline' : 'Play timeline',
            'onclick': () => this.togglePlay(trace, session, model),
          }),
          m(Button, {
            'icon': 'skip_next',
            'title': 'Next step',
            'aria-label': 'Next step',
            'variant': ButtonVariant.Outlined,
            'compact': true,
            'onclick': () => {
              this.stopPlay();
              this.step(trace, session, model, 1);
            },
          }),
          m('input[type=range].pf-process-graph__scrubber', {
            'min': 0,
            'max': durMs,
            'step': 'any',
            'value': curRelMs,
            'aria-label': 'Trace timestamp',
            'oninput': (e: InputEvent) => {
              this.stopPlay();
              const target = e.target as HTMLInputElement;
              const relNs = BigInt(Math.round(Number(target.value) * 1e6));
              const nextTs = Time.clamp(
                Time.fromRaw(model.traceStart + relNs),
                model.traceStart,
                model.traceEnd,
              );
              this.advanceTo(trace, session, nextTs);
            },
          }),
          m(
            'span.pf-process-graph__time-readout',
            {'aria-live': 'polite'},
            `+${formatRelTime(curTs - model.traceStart)} / ${formatRelTime(model.traceEnd - model.traceStart)}`,
          ),
          m(Button, {
            'icon': 'sync',
            'title': session.syncTimeline
              ? 'Keep timeline synced (on) — click to turn off'
              : 'Keep timeline synced with graph',
            'aria-label': 'Keep timeline synced with graph',
            'aria-pressed': String(session.syncTimeline),
            'active': session.syncTimeline,
            'intent': session.syncTimeline ? Intent.Primary : Intent.None,
            'variant': session.syncTimeline
              ? ButtonVariant.Filled
              : ButtonVariant.Outlined,
            'compact': true,
            'onclick': () => this.toggleTimelineSync(trace, session, model),
          }),
          m(Button, {
            'icon': 'fit_screen',
            'title': 'Reset zoom and pan',
            'aria-label': 'Reset zoom and pan',
            'variant': ButtonVariant.Outlined,
            'compact': true,
            'onclick': () => {
              this.resetGeneration += 1;
            },
          }),
        ),
        m(
          '.pf-process-graph__toolbar-row',
          m(
            'label.pf-process-graph__control-label',
            'Step:',
            m(
              Select,
              {
                'value': String(session.stepMode),
                'aria-label': 'Step mode',
                'onchange': (e: Event) => {
                  const val = (e.target as HTMLSelectElement).value;
                  if (val === 'edge' || val === 'any') {
                    session.stepMode = val;
                  } else {
                    const parsed = Number(val);
                    if (
                      parsed === 1000 ||
                      parsed === 5000 ||
                      parsed === 10000 ||
                      parsed === 60000
                    ) {
                      session.stepMode = parsed;
                    }
                  }
                },
              },
              m('option', {value: 'edge'}, 'Edges'),
              m('option', {value: 'any'}, 'Edges + states'),
              m('option', {value: '1000'}, '1 s'),
              m('option', {value: '5000'}, '5 s'),
              m('option', {value: '10000'}, '10 s'),
              m('option', {value: '60000'}, '60 s'),
            ),
          ),
          m(
            'label.pf-process-graph__control-label',
            'Speed:',
            m(
              Select,
              {
                'value': String(session.playSpeedMs),
                'aria-label': 'Playback speed',
                'onchange': (e: Event) => {
                  session.playSpeedMs = Number(
                    (e.target as HTMLSelectElement).value,
                  );
                },
              },
              m('option', {value: '1000'}, '1×/s'),
              m('option', {value: '350'}, '3×/s'),
              m('option', {value: '100'}, '10×/s'),
            ),
          ),
          m(TextInput, {
            'className': 'pf-process-graph__search',
            'leftIcon': Icons.Search,
            'placeholder': 'Filter...',
            'value': session.filters.searchQuery,
            'aria-label': 'Filter processes or components',
            'onInput': (val: string) => {
              session.filters = {...session.filters, searchQuery: val};
            },
          }),
        ),
        m(
          '.pf-process-graph__toolbar-row',
          filterCheckbox('Services', 'showServices'),
          filterCheckbox('Providers', 'showProviders'),
          filterCheckbox('Self-loops', 'showSelfLoops'),
          filterCheckbox('Isolated', 'showIsolated'),
        ),
        m(
          '.pf-process-graph__toolbar-row.pf-process-graph__toolbar-row--meta',
          m(
            'span.pf-process-graph__meta',
            `${snap.nodes.length} procs (${activeRows}/${model.globalRanks.length} rows) · ${snap.edges.length} edges`,
          ),
          m('span.pf-process-graph__meta', '— service · -- provider'),
        ),
      ),
      m(
        '.pf-process-graph__body',
        m(SplitPanel, {
          direction: 'vertical',
          controlledPanel: 'second',
          split: {pixels: session.inspectorHeightPx},
          minSize: 96,
          onResize: (px) => {
            session.inspectorHeightPx = Math.round(px);
          },
          firstPanel: m(GraphChart, {
            data: chartData,
            layout: 'layered',
            fillParent: true,
            resetToken: this.resetGeneration,
            ariaLabel:
              'Layered process dependency graph ordered vertically from persistent processes down to cached empty processes. Use W, A, S, D to pan and Shift+W or Shift+S to zoom.',
            onNodeClick: (nodeId) => {
              const upid = Number(nodeId);
              if (Number.isFinite(upid)) {
                session.selected = {type: 'node', upid};
              }
            },
            onEdgeClick: (edgeId) => {
              session.selected = {type: 'edge', key: edgeId};
            },
            onCanvasEmptyClick: () => {
              session.selected = undefined;
            },
          }),
          secondPanel: [this.renderInspector(trace, session, model, snap)],
        }),
      ),
    );
  }

  private renderInspectorShell(
    trace: Trace,
    key: string,
    title: string,
    ts: time,
    upid: number | undefined,
    ...body: m.Children[]
  ): m.Children {
    return m(
      'aside.pf-process-graph__inspector',
      {
        key,
        'role': 'region',
        'aria-labelledby': 'pf-process-graph-inspector-title',
        'tabindex': 0,
      },
      m(
        '.pf-process-graph__inspector-header',
        m(
          'h2#pf-process-graph-inspector-title.pf-process-graph__inspector-title',
          title,
        ),
        m(Button, {
          'label': 'Show on timeline',
          'aria-label': 'Show on timeline',
          'rightIcon': Icons.GoTo,
          'variant': ButtonVariant.Outlined,
          'compact': true,
          'onclick': () => this.showOnTimeline(trace, ts, upid),
        }),
      ),
      ...body,
    );
  }

  private renderInspector(
    trace: Trace,
    session: ProcessGraphSession,
    model: ProcessGraphModel,
    snap: GraphSnapshot,
  ): m.Children {
    const sel = session.selected;
    if (sel?.type === 'node') {
      return this.renderNodeInspector(trace, session, model, snap, sel.upid);
    }
    if (sel?.type === 'edge') {
      return this.renderEdgeInspector(trace, session, model, snap, sel.key);
    }
    return this.renderSummaryInspector(trace, model, snap);
  }

  private renderNodeInspector(
    trace: Trace,
    session: ProcessGraphSession,
    model: ProcessGraphModel,
    snap: GraphSnapshot,
    upid: number,
  ): m.Children {
    const node = snap.allNodesMap.get(upid);
    const proc = buildProcessInfoFallback(model, upid);
    const selfEdges = snap.rawEdges.filter(
      (e) => e.client === upid && e.server === upid,
    );
    const byPeerRank =
      (peerOf: (e: CollapsedEdge) => number) =>
      (a: CollapsedEdge, b: CollapsedEdge) =>
        (snap.allNodesMap.get(peerOf(a))?.stateRank ?? 99) -
          (snap.allNodesMap.get(peerOf(b))?.stateRank ?? 99) ||
        b.count - a.count;
    const inEdges = snap.rawEdges
      .filter((e) => e.server === upid && e.client !== upid)
      .sort(byPeerRank((e) => e.client));
    const outEdges = snap.rawEdges
      .filter((e) => e.client === upid && e.server !== upid)
      .sort(byPeerRank((e) => e.server));

    const renderPeerList = (
      edges: ReadonlyArray<CollapsedEdge>,
      peerOf: (e: CollapsedEdge) => number,
    ): m.Children =>
      edges.length === 0
        ? m('span.pf-process-graph__muted', 'None')
        : m(
            'ul.pf-process-graph__list',
            edges.map((e) => {
              const peerUpid = peerOf(e);
              const peerProc = buildProcessInfoFallback(model, peerUpid);
              const peerState = snap.allNodesMap.get(peerUpid)?.state ?? '—';
              const peerName = peerProc.name || `pid ${peerProc.pid}`;
              return m(
                'li.pf-process-graph__peer-row',
                {key: e.key},
                m(Button, {
                  className: 'pf-process-graph__peer-main',
                  label: `${peerName} (${peerState})`,
                  title: `Inspect process ${peerProc.name}`,
                  variant: ButtonVariant.Outlined,
                  compact: true,
                  shrink: true,
                  onclick: () => {
                    session.selected = {type: 'node', upid: peerUpid};
                  },
                }),
                m(Button, {
                  'label': `${e.kind} ×${e.count} →`,
                  'aria-label': `${e.kind} ×${e.count} → with ${peerName}`,
                  'title': `Inspect ${e.kind} edge (${e.count} connections)`,
                  'variant': ButtonVariant.Outlined,
                  'compact': true,
                  'onclick': () => {
                    session.selected = {type: 'edge', key: e.key};
                  },
                }),
              );
            }),
          );

    return this.renderInspectorShell(
      trace,
      `node:${upid}`,
      'Process inspector',
      snap.ts,
      upid,
      m(
        Tree,
        {bordered: true},
        m(TreeNode, {left: 'Process', right: proc.name}),
        m(TreeNode, {
          left: 'PID / UID',
          right: `pid ${proc.pid} · uid ${proc.uid ?? '—'}`,
        }),
        proc.packageName !== undefined &&
          m(TreeNode, {left: 'Package', right: proc.packageName}),
        m(TreeNode, {left: 'State', right: node?.state ?? 'EXITED'}),
        node?.reason !== undefined &&
          node.reason !== '' &&
          m(TreeNode, {left: 'Reason', right: node.reason}),
      ),
      selfEdges.length > 0 &&
        renderSection(
          `Self-loops (${selfEdges.length})`,
          m(
            'ul.pf-process-graph__list',
            selfEdges.map((e) =>
              m(
                'li',
                {key: e.key},
                m(Button, {
                  className: 'pf-process-graph__full-btn',
                  label: `↻ self (${e.kind} ×${e.count}) — ${e.components[0] ?? 'inspect components'}`,
                  variant: ButtonVariant.Outlined,
                  compact: true,
                  shrink: true,
                  onclick: () => {
                    session.selected = {type: 'edge', key: e.key};
                  },
                }),
              ),
            ),
          ),
        ),
      renderSection(
        `Incoming — bound by (${inEdges.length})`,
        renderPeerList(inEdges, (e) => e.client),
      ),
      renderSection(
        `Outgoing — binds to (${outEdges.length})`,
        renderPeerList(outEdges, (e) => e.server),
      ),
    );
  }

  private renderEdgeInspector(
    trace: Trace,
    session: ProcessGraphSession,
    model: ProcessGraphModel,
    snap: GraphSnapshot,
    edgeKey: string,
  ): m.Children {
    const edge = snap.rawEdges.find((e) => e.key === edgeKey);
    if (edge === undefined) {
      return this.renderSummaryInspector(trace, model, snap);
    }

    const clientProc = buildProcessInfoFallback(model, edge.client);
    const serverProc = buildProcessInfoFallback(model, edge.server);
    const renderMonoList = (items: ReadonlyArray<string>): m.Children =>
      items.length === 0
        ? m('span.pf-process-graph__muted', 'None recorded')
        : m(
            'ul.pf-process-graph__list',
            items.map((item) =>
              m('li.pf-process-graph__mono-item', {key: item}, item),
            ),
          );

    return this.renderInspectorShell(
      trace,
      `edge:${edgeKey}`,
      'Edge inspector',
      snap.ts,
      edge.client,
      renderSection(
        'Client → Server',
        m(
          '.pf-process-graph__list',
          m(Button, {
            className: 'pf-process-graph__full-btn',
            label: `Client: ${clientProc.name} (pid ${clientProc.pid})`,
            variant: ButtonVariant.Outlined,
            compact: true,
            shrink: true,
            onclick: () => {
              session.selected = {type: 'node', upid: edge.client};
            },
          }),
          m(Button, {
            className: 'pf-process-graph__full-btn',
            label: `Server: ${serverProc.name} (pid ${serverProc.pid})`,
            variant: ButtonVariant.Outlined,
            compact: true,
            shrink: true,
            onclick: () => {
              session.selected = {type: 'node', upid: edge.server};
            },
          }),
        ),
      ),
      m(
        Tree,
        {bordered: true},
        m(TreeNode, {left: 'Kind', right: edge.kind}),
        m(TreeNode, {left: 'Connections', right: String(edge.count)}),
      ),
      renderSection(
        `Components / authorities (${edge.components.length})`,
        renderMonoList(edge.components),
      ),
      edge.details.length > 0 &&
        renderSection('Flags / metadata', renderMonoList(edge.details)),
    );
  }

  private renderSummaryInspector(
    trace: Trace,
    model: ProcessGraphModel,
    snap: GraphSnapshot,
  ): m.Children {
    return this.renderInspectorShell(
      trace,
      'summary',
      'Graph summary',
      snap.ts,
      undefined,
      m(
        Tree,
        {bordered: true},
        m(TreeNode, {
          left: 'Timestamp',
          right: `+${formatRelTime(snap.ts - model.traceStart)}`,
        }),
        m(TreeNode, {
          left: 'Visible',
          right: `${snap.nodes.length} nodes · ${snap.edges.length} edges`,
        }),
      ),
      m(
        'p.pf-process-graph__muted',
        'Click a process or edge to inspect its bindings, or press Play / Next to step through graph changes.',
      ),
    );
  }
}
