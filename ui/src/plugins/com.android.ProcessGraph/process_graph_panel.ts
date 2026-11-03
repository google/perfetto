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
import {GateDetector} from '../../base/mithril_utils';
import {Icons} from '../../base/semantic_icons';
import {Duration, type duration, Time, type time} from '../../base/time';
import {GraphChart} from '../../components/widgets/charts/graph_chart';
import type {Trace} from '../../public/trace';
import {Button, ButtonVariant} from '../../widgets/button';
import {Checkbox} from '../../widgets/checkbox';
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
  buildContextualSubgraph,
  type CollapsedEdge,
  computeDistinctStepTimes,
  type ContextualSubgraph,
  type GraphSnapshot,
  loadProcessGraphModel,
  type ProcessGraphFilters,
  type ProcessGraphModel,
  type ScopeMode,
  snapshotAt,
  type StepMode,
  stepTimestamp,
  upperBoundTime,
} from './model';

export interface ProcessGraphContext {
  readonly selectionKey: string;
  readonly windowStart: time;
  readonly windowEnd: time;
  readonly targetUpids: ReadonlyArray<number>;
  readonly targetStates: ReadonlyArray<string>;
  readonly preselectedEdgeKey?: string;
}

export class ProcessGraphSession {
  readonly modelMemo = new AsyncMemo<ProcessGraphModel>();
  activeSelectionKey?: string;
  curTs?: time;
  diffFromTs?: time;
  scopeMode: ScopeMode = 'neighbors';
  stepMode: StepMode = 'any';
  playSpeedMs = 350;
  inspectorWidthPx = 360;
  filters: ProcessGraphFilters = {
    showServices: true,
    showProviders: true,
    showSelfLoops: false,
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
  readonly context: ProcessGraphContext;
}

export function formatRelTime(delta: duration): string {
  if (delta <= 0n) return '0s';
  if (delta < 60_000_000_000n) return Duration.humanise(delta);
  return Duration.format((delta / 1_000_000_000n) * 1_000_000_000n);
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
  private isVisible = false;
  private playTimer?: ReturnType<typeof setTimeout>;
  private resetGeneration = 0;
  private cachedSubgraphKey?: string;
  private cachedSubgraphModel?: ProcessGraphModel;
  private cachedSubgraph?: ContextualSubgraph;
  private cachedStepsKey?: string;
  private cachedStepsSubgraph?: ContextualSubgraph;
  private cachedSteps?: ReadonlyArray<time>;

  onremove({attrs}: m.CVnodeDOM<ProcessGraphPanelAttrs>) {
    this.stopPlay();
    this.cachedSubgraph = undefined;
    this.cachedSubgraphModel = undefined;
    this.cachedSteps = undefined;
    this.cachedStepsSubgraph = undefined;
    if (this.isVisible) {
      attrs.trace.timeline.hoverCursorTimestamp = undefined;
    }
    this.isVisible = false;
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
  ): void {
    session.curTs = nextTs;
    if (this.isVisible) {
      trace.timeline.hoverCursorTimestamp = nextTs;
    }
  }

  private getSubgraph(
    model: ProcessGraphModel,
    context: ProcessGraphContext,
    session: ProcessGraphSession,
  ): ContextualSubgraph {
    const {filters} = session;
    const key = [
      context.selectionKey,
      session.scopeMode,
      filters.showServices ? 1 : 0,
      filters.showProviders ? 1 : 0,
      filters.showSelfLoops ? 1 : 0,
    ].join('|');
    if (
      this.cachedSubgraph !== undefined &&
      this.cachedSubgraphModel === model &&
      this.cachedSubgraphKey === key
    ) {
      return this.cachedSubgraph;
    }
    const sub = buildContextualSubgraph(model, {
      windowStart: context.windowStart,
      windowEnd: context.windowEnd,
      targetUpids: context.targetUpids,
      targetStates: context.targetStates,
      scopeMode: session.scopeMode,
      filters,
    });
    this.cachedSubgraphModel = model;
    this.cachedSubgraphKey = key;
    this.cachedSubgraph = sub;
    return sub;
  }

  private getDistinctSteps(
    model: ProcessGraphModel,
    subgraph: ContextualSubgraph,
    session: ProcessGraphSession,
  ): ReadonlyArray<time> | undefined {
    const {stepMode, filters} = session;
    if (stepMode !== 'any' && stepMode !== 'edge') {
      return undefined;
    }
    const key = [
      stepMode,
      filters.showServices ? 1 : 0,
      filters.showProviders ? 1 : 0,
      filters.showSelfLoops ? 1 : 0,
      filters.searchQuery.trim().toLowerCase(),
    ].join('|');
    if (
      this.cachedSteps !== undefined &&
      this.cachedStepsSubgraph === subgraph &&
      this.cachedStepsKey === key
    ) {
      return this.cachedSteps;
    }
    const steps = computeDistinctStepTimes(model, subgraph, stepMode, filters);
    this.cachedStepsSubgraph = subgraph;
    this.cachedStepsKey = key;
    this.cachedSteps = steps;
    return steps;
  }

  private step(
    trace: Trace,
    session: ProcessGraphSession,
    model: ProcessGraphModel,
    subgraph: ContextualSubgraph,
    dir: 1 | -1,
  ): boolean {
    const cur = session.curTs ?? subgraph.windowStart;
    const nextTs = stepTimestamp(
      model,
      subgraph,
      cur,
      dir,
      session.stepMode,
      session.filters,
    );
    if (nextTs === undefined) return false;
    session.diffFromTs = cur;
    this.advanceTo(trace, session, nextTs);
    return true;
  }

  private togglePlay(
    trace: Trace,
    session: ProcessGraphSession,
    model: ProcessGraphModel,
    subgraph: ContextualSubgraph,
  ): void {
    if (this.isPlaying) {
      this.stopPlay();
      return;
    }
    if (subgraph.windowEnd <= subgraph.windowStart) {
      return;
    }
    const distinctSteps = this.getDistinctSteps(model, subgraph, session);
    const lastTs =
      distinctSteps !== undefined && distinctSteps.length > 0
        ? (distinctSteps[distinctSteps.length - 1] ?? subgraph.windowEnd)
        : subgraph.windowEnd;
    const cur = session.curTs ?? subgraph.windowStart;
    if (cur >= lastTs) {
      session.diffFromTs = undefined;
      this.advanceTo(trace, session, subgraph.windowStart);
    }
    this.isPlaying = true;
    const tick = () => {
      if (!this.isPlaying) return;
      const liveSub = this.cachedSubgraph ?? subgraph;
      if (!this.step(trace, session, model, liveSub, 1)) {
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
    subgraph: ContextualSubgraph,
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
      this.step(trace, session, model, subgraph, 1);
    } else if (ev.key === 'ArrowLeft') {
      ev.preventDefault();
      this.stopPlay();
      this.step(trace, session, model, subgraph, -1);
    } else if (ev.key === 'Escape' && session.selected !== undefined) {
      session.selected = undefined;
    }
  }

  view({attrs}: m.CVnode<ProcessGraphPanelAttrs>) {
    const {trace, session, context} = attrs;

    const wrapGate = (child: m.Children) =>
      m(
        GateDetector,
        {
          onVisibilityChanged: (visible: boolean) => {
            this.isVisible = visible;
            if (visible) {
              if (session.curTs !== undefined) {
                trace.timeline.hoverCursorTimestamp = session.curTs;
              }
              m.redraw();
            } else {
              this.stopPlay();
              trace.timeline.hoverCursorTimestamp = undefined;
            }
          },
        },
        child,
      );

    const memoRes = session.modelMemo.use({
      key: {traceStart: trace.traceInfo.start},
      compute: (signal) => loadProcessGraphModel(trace, signal),
    });

    if (memoRes.isPending && memoRes.data === undefined) {
      return wrapGate(
        m(
          '.pf-process-graph',
          m(
            '.pf-process-graph__loading',
            m(Spinner),
            'Loading process graph...',
          ),
        ),
      );
    }

    const model = memoRes.data;
    if (model === undefined || model.connections.length === 0) {
      return wrapGate(
        m(
          '.pf-process-graph',
          m(EmptyState, {
            title:
              'No Android service or content provider connections in trace',
          }),
        ),
      );
    }

    const subgraph = this.getSubgraph(model, context, session);
    if (session.activeSelectionKey !== context.selectionKey) {
      this.stopPlay();
      session.activeSelectionKey = context.selectionKey;
      let initTs = subgraph.windowStart;
      if (
        subgraph.windowEnd > subgraph.windowStart &&
        snapshotAt(model, subgraph, initTs, session.filters).nodes.length === 0
      ) {
        const candidates =
          this.getDistinctSteps(model, subgraph, session) ?? subgraph.anyEvents;
        for (const t of candidates) {
          if (
            snapshotAt(model, subgraph, t, session.filters).nodes.length > 0
          ) {
            initTs = t;
            break;
          }
        }
      }
      session.curTs = initTs;
      session.diffFromTs = undefined;
      this.resetGeneration += 1;
      if (context.preselectedEdgeKey !== undefined) {
        session.selected = {type: 'edge', key: context.preselectedEdgeKey};
      } else if (
        context.targetUpids.length === 1 &&
        context.targetUpids[0] !== undefined
      ) {
        session.selected = {type: 'node', upid: context.targetUpids[0]};
      } else {
        session.selected = undefined;
      }
      if (this.isVisible) {
        trace.timeline.hoverCursorTimestamp = initTs;
      }
    }

    const curTs = Time.clamp(
      session.curTs ?? subgraph.windowStart,
      subgraph.windowStart,
      subgraph.windowEnd,
    );
    session.curTs = curTs;

    const snap = snapshotAt(model, subgraph, curTs, session.filters);
    const prevTs =
      session.diffFromTs !== undefined
        ? Time.clamp(
            session.diffFromTs,
            subgraph.windowStart,
            subgraph.windowEnd,
          )
        : undefined;
    const prevSnap =
      prevTs !== undefined && prevTs !== curTs
        ? snapshotAt(model, subgraph, prevTs, session.filters)
        : undefined;
    const distinctSteps = this.getDistinctSteps(model, subgraph, session);
    const activeCols = new Set(snap.nodes.map((n) => n.stateRank)).size;
    const frozenCount = snap.nodes.filter((n) => n.isFrozen).length;

    const isInstant = subgraph.windowEnd <= subgraph.windowStart;
    const firstStepTs =
      distinctSteps !== undefined && distinctSteps.length > 0
        ? (distinctSteps[0] ?? subgraph.windowStart)
        : subgraph.windowStart;
    const lastStepTs =
      distinctSteps !== undefined && distinctSteps.length > 0
        ? (distinctSteps[distinctSteps.length - 1] ?? subgraph.windowEnd)
        : subgraph.windowEnd;
    const canStepPrev = !isInstant && curTs > firstStepTs;
    const canStepNext = !isInstant && curTs < lastStepTs;

    const winDurMs = Math.max(
      0,
      Number(subgraph.windowEnd - subgraph.windowStart) / 1e6,
    );
    const scrubberMax = Math.max(1, winDurMs);
    const scrubberVal = isInstant
      ? 0
      : Math.max(
          0,
          Math.min(winDurMs, Number(curTs - subgraph.windowStart) / 1e6),
        );

    const curFromTraceStart = formatRelTime(curTs - model.traceStart);
    const winStartStr = formatRelTime(subgraph.windowStart - model.traceStart);
    const winEndStr = formatRelTime(subgraph.windowEnd - model.traceStart);
    const stepReadout =
      !isInstant && distinctSteps !== undefined && distinctSteps.length > 0
        ? ` · step ${Math.min(distinctSteps.length, Math.max(1, upperBoundTime(distinctSteps, curTs)))}/${distinctSteps.length}`
        : '';
    const readoutText = isInstant
      ? `+${curFromTraceStart} (instant)`
      : `+${curFromTraceStart}${stepReadout} ([+${winStartStr}, +${winEndStr}])`;

    const chartData = buildProcessGraphChartData(
      model,
      subgraph,
      snap,
      session.selected,
      prevSnap,
    );

    const filterCheckbox = (
      label: string,
      key: 'showServices' | 'showProviders' | 'showSelfLoops',
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

    return wrapGate(
      m(
        '.pf-process-graph',
        {
          tabindex: -1,
          onkeydown: (ev: KeyboardEvent) =>
            this.handleKeyDown(trace, session, model, subgraph, ev),
        },
        m(
          'header.pf-process-graph__toolbar',
          m(
            '.pf-process-graph__toolbar-row',
            m(Button, {
              'icon': 'skip_previous',
              'title': 'Previous step in selection',
              'aria-label': 'Previous step in selection',
              'variant': ButtonVariant.Outlined,
              'compact': true,
              'disabled': !canStepPrev,
              'onclick': () => {
                this.stopPlay();
                this.step(trace, session, model, subgraph, -1);
              },
            }),
            m(Button, {
              'label': this.isPlaying ? 'Pause' : 'Play',
              'icon': this.isPlaying ? 'pause' : Icons.Play,
              'variant': ButtonVariant.Filled,
              'compact': true,
              'active': this.isPlaying,
              'disabled': isInstant,
              'aria-label': this.isPlaying
                ? 'Pause selection playback'
                : 'Play selection window',
              'onclick': () => this.togglePlay(trace, session, model, subgraph),
            }),
            m(Button, {
              'icon': 'skip_next',
              'title': 'Next step in selection',
              'aria-label': 'Next step in selection',
              'variant': ButtonVariant.Outlined,
              'compact': true,
              'disabled': !canStepNext,
              'onclick': () => {
                this.stopPlay();
                this.step(trace, session, model, subgraph, 1);
              },
            }),
            m('input[type=range].pf-process-graph__scrubber', {
              'min': 0,
              'max': scrubberMax,
              'step': 'any',
              'value': scrubberVal,
              'disabled': isInstant,
              'aria-label': 'Selection time scrubber',
              'oninput': (e: InputEvent) => {
                this.stopPlay();
                session.diffFromTs = undefined;
                const target = e.target as HTMLInputElement;
                const relNs = BigInt(Math.round(Number(target.value) * 1e6));
                const nextTs = Time.clamp(
                  Time.fromRaw(subgraph.windowStart + relNs),
                  subgraph.windowStart,
                  subgraph.windowEnd,
                );
                this.advanceTo(trace, session, nextTs);
              },
            }),
            m(
              'span.pf-process-graph__time-readout',
              {'aria-live': 'polite'},
              readoutText,
            ),
            m(
              'label.pf-process-graph__control-label',
              'Scope:',
              m(
                Select,
                {
                  'value': session.scopeMode,
                  'aria-label': 'Subgraph scope',
                  'onchange': (e: Event) => {
                    const val = (e.target as HTMLSelectElement).value;
                    if (
                      val === 'chains' ||
                      val === 'neighbors' ||
                      val === 'component'
                    ) {
                      session.scopeMode = val;
                      session.diffFromTs = undefined;
                      this.resetGeneration += 1;
                    }
                  },
                },
                m('option', {value: 'neighbors'}, 'Direct connections'),
                m('option', {value: 'chains'}, 'Directed chains'),
                m('option', {value: 'component'}, 'Full component'),
              ),
            ),
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
                        parsed === 100 ||
                        parsed === 1000 ||
                        parsed === 5000
                      ) {
                        session.stepMode = parsed;
                      }
                    }
                  },
                },
                m('option', {value: 'any'}, 'Edges + states + freezer'),
                m('option', {value: 'edge'}, 'Edges only'),
                m('option', {value: '100'}, '100 ms'),
                m('option', {value: '1000'}, '1 s'),
                m('option', {value: '5000'}, '5 s'),
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
            '.pf-process-graph__toolbar-row.pf-process-graph__toolbar-row--meta',
            m(
              '.pf-process-graph__filters',
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
              filterCheckbox('Services', 'showServices'),
              filterCheckbox('Providers', 'showProviders'),
              filterCheckbox('Self-loops', 'showSelfLoops'),
            ),
            m(
              'span.pf-process-graph__meta',
              `${snap.nodes.length} procs (${frozenCount} ❄ frozen · ${activeCols}/${chartData.layers?.length ?? subgraph.orderedRanks.length} cols) · ${snap.edges.length} edges · client → server · — service (dim = waived) · -- provider`,
            ),
          ),
        ),
        m(
          '.pf-process-graph__body',
          subgraph.subgraphUpids.size === 0
            ? m(EmptyState, {
                title: 'No processes matched the selected state in this window',
              })
            : m(SplitPanel, {
                direction: 'horizontal',
                controlledPanel: 'second',
                split: {pixels: session.inspectorWidthPx},
                minSize: 220,
                onResize: (px) => {
                  session.inspectorWidthPx = Math.round(px);
                },
                firstPanel:
                  snap.nodes.length === 0
                    ? m(EmptyState, {
                        title:
                          subgraph.windowConnections.length === 0
                            ? 'No active bindings for the selected tracks in this window'
                            : 'No active bindings at this timestamp',
                      })
                    : m(GraphChart, {
                        data: chartData,
                        layout: 'layered',
                        layerOrientation: 'horizontal',
                        fillParent: true,
                        resetToken: this.resetGeneration,
                        ariaLabel:
                          'Contextual process dependency graph ordered horizontally from cached processes on the left to persistent processes on the right. Frozen processes appear as snowflakes. Use W, A, S, D to pan and Shift+W or Shift+S to zoom.',
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
                secondPanel: [
                  this.renderInspector(
                    trace,
                    session,
                    model,
                    subgraph,
                    snap,
                    prevSnap,
                  ),
                ],
              }),
        ),
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
          'onclick': () => jumpToTimeline(trace, ts, upid),
        }),
      ),
      ...body,
    );
  }

  private renderInspector(
    trace: Trace,
    session: ProcessGraphSession,
    model: ProcessGraphModel,
    subgraph: ContextualSubgraph,
    snap: GraphSnapshot,
    prevSnap?: GraphSnapshot,
  ): m.Children {
    const sel = session.selected;
    if (sel?.type === 'node') {
      return this.renderNodeInspector(
        trace,
        session,
        model,
        subgraph,
        snap,
        sel.upid,
      );
    }
    if (sel?.type === 'edge') {
      return this.renderEdgeInspector(
        trace,
        session,
        model,
        subgraph,
        snap,
        prevSnap,
        sel.key,
      );
    }
    return this.renderSummaryInspector(trace, session, model, subgraph, snap);
  }

  private renderNodeInspector(
    trace: Trace,
    session: ProcessGraphSession,
    model: ProcessGraphModel,
    subgraph: ContextualSubgraph,
    snap: GraphSnapshot,
    upid: number,
  ): m.Children {
    const node = snap.allNodesMap.get(upid);
    if (node === undefined) {
      return this.renderSummaryInspector(trace, session, model, subgraph, snap);
    }
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
              const peerNode = snap.allNodesMap.get(peerUpid);
              const peerState = peerNode?.state ?? '—';
              const peerFrozen = peerNode?.isFrozen === true ? ' · FROZEN' : '';
              const peerName = peerProc.name || `pid ${peerProc.pid}`;
              return m(
                'li.pf-process-graph__peer-row',
                {key: e.key},
                m(Button, {
                  className: 'pf-process-graph__peer-main',
                  label: `${peerName} (${peerState}${peerFrozen})`,
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
        m(TreeNode, {left: 'State', right: node.state}),
        m(TreeNode, {
          left: 'Freezer',
          right: node.isFrozen ? 'FROZEN' : 'UNFROZEN',
        }),
        node.isTarget &&
          m(TreeNode, {left: 'Selection', right: 'Selected on timeline'}),
        node.reason !== undefined &&
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
    subgraph: ContextualSubgraph,
    snap: GraphSnapshot,
    prevSnap: GraphSnapshot | undefined,
    edgeKey: string,
  ): m.Children {
    const activeEdge = snap.rawEdges.find((e) => e.key === edgeKey);
    const removedEdge =
      activeEdge === undefined
        ? prevSnap?.rawEdges.find((e) => e.key === edgeKey)
        : undefined;
    const edge = activeEdge ?? removedEdge;
    if (edge === undefined) {
      return this.renderSummaryInspector(trace, session, model, subgraph, snap);
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
        removedEdge !== undefined &&
          m(TreeNode, {left: 'Status', right: 'Removed at this step'}),
        m(TreeNode, {left: 'Kind', right: edge.kind}),
        m(TreeNode, {left: 'Connections', right: String(edge.count)}),
        edge.kind === 'service' &&
          m(TreeNode, {
            left: 'Priority',
            right:
              edge.waivesPriority === true
                ? 'Waived (NOT_FOREGROUND / WAIVE_PRIORITY)'
                : 'Propagates client priority',
          }),
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
    session: ProcessGraphSession,
    model: ProcessGraphModel,
    subgraph: ContextualSubgraph,
    snap: GraphSnapshot,
  ): m.Children {
    const frozenCount = snap.nodes.filter((n) => n.isFrozen).length;
    const targetUpid =
      subgraph.explicitTargetUpids.size === 1
        ? [...subgraph.explicitTargetUpids][0]
        : undefined;
    const scopeLabel =
      session.scopeMode === 'component'
        ? 'Full component'
        : session.scopeMode === 'neighbors'
          ? 'Direct connections'
          : 'Directed chains';
    return this.renderInspectorShell(
      trace,
      'summary',
      'Subgraph summary',
      snap.ts,
      targetUpid,
      m(
        Tree,
        {bordered: true},
        m(TreeNode, {
          left: 'Timestamp',
          right: `+${formatRelTime(snap.ts - model.traceStart)}`,
        }),
        m(TreeNode, {
          left: 'Window',
          right: `+${formatRelTime(subgraph.windowStart - model.traceStart)} → +${formatRelTime(subgraph.windowEnd - model.traceStart)}`,
        }),
        m(TreeNode, {
          left: 'Scope',
          right: scopeLabel,
        }),
        m(TreeNode, {
          left: 'Visible',
          right: `${snap.nodes.length} procs (${frozenCount} frozen) · ${snap.edges.length} edges`,
        }),
      ),
      m(
        'p.pf-process-graph__muted',
        'Click a process or edge to inspect its bindings, or press Play / Next to step through changes in the selected window.',
      ),
    );
  }
}
