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
import {Button, ButtonVariant} from '../../../widgets/button';
import {Intent} from '../../../widgets/common';
import type {Cluster, TraceState} from '../state';
import {
  getCrossCompareState,
  closeCrossCompare,
  recordCrossComparison,
  applyCrossCompareResults,
  resetCrossCompare,
  ensureCache,
  updateSlider,
  undoCrossComparison,
  discardCrossCompareTrace,
  skipCrossComparison,
} from '../state';
import {getProgress, getResults} from '../models/cross_compare';
import type {CrossCompareState} from '../models/cross_compare';
import {MiniTimeline} from './mini_timeline';
import {SummaryTables} from './summary_tables';
import {fmtDur} from '../utils/format';
import {buildTraceLink} from '../utils/export';
import type {SortState} from '../models/types';

// -- Module-level state --

let keyHandler: ((e: KeyboardEvent) => void) | null = null;
let ccSliderPct = 100;
let lastPairKey: string | null = null;

// Anchor mode: when set, this trace stays on screen and others rotate against it
let anchorKey: string | null = null;
let anchorSide: 'left' | 'right' | null = null;

// Review screen state
let reviewPairIdx = 0;

// Sort state for summary tables inside panels
const panelSortState: Record<string, SortState> = {};

// -- Slider helpers --

function updateBothSliders(cl: Cluster, pct: number): void {
  ccSliderPct = pct;
  const state = getCrossCompareState();
  if (!state?.currentPair) return;
  const frac = pct / 100;
  for (const key of state.currentPair) {
    const ts = findTrace(cl, key);
    if (!ts) continue;
    ensureCache(ts);
    const target = Math.max(2, Math.round(2 + (ts.origN - 2) * frac));
    updateSlider(ts, target);
  }
}

// -- Lookup maps (rebuilt once per render cycle) --

let traceMap: Map<string, TraceState> | null = null;
let indexMap: Map<string, number> | null = null;
let mapClusterId: string | null = null;

function ensureMaps(cl: Cluster): void {
  if (mapClusterId === cl.id && traceMap) return;
  traceMap = new Map();
  indexMap = new Map();
  cl.traces.forEach((ts, i) => {
    traceMap!.set(ts._key, ts);
    indexMap!.set(ts._key, i);
  });
  mapClusterId = cl.id;
}

function findTrace(cl: Cluster, key: string): TraceState | undefined {
  ensureMaps(cl);
  return traceMap!.get(key);
}

function traceIndex(cl: Cluster, key: string): number {
  ensureMaps(cl);
  return indexMap!.get(key) ?? -1;
}

// -- Anchor helpers --

function toggleAnchor(key: string, side: 'left' | 'right'): void {
  if (anchorKey === key && anchorSide === side) {
    anchorKey = null;
    anchorSide = null;
  } else {
    anchorKey = key;
    anchorSide = side;
  }
  const state = getCrossCompareState();
  if (state) state.selectedSide = null;
  m.redraw();
}

function clearAnchor(): void {
  anchorKey = null;
  anchorSide = null;
}

/** After advancing, ensure anchor stays on the correct side of currentPair. */
function ensureAnchorSide(): void {
  const state = getCrossCompareState();
  if (!anchorKey || !state?.currentPair) return;
  // Clear anchor if anchor trace was discarded
  if (state.discardedKeys.has(anchorKey)) {
    clearAnchor();
    return;
  }
  const [a, b] = state.currentPair;
  // Only swap if anchor is actually in this pair
  if (anchorSide === 'left' && b === anchorKey && a !== anchorKey) {
    state.currentPair = [b, a];
  } else if (anchorSide === 'right' && a === anchorKey && b !== anchorKey) {
    state.currentPair = [b, a];
  }
}

/** Whether the anchor trace is in the current pair. */
function anchorActive(state: CrossCompareState | null): boolean {
  return (
    !!anchorKey && !!state?.currentPair && state.currentPair.includes(anchorKey)
  );
}

function discardSideForAnchor(): 'left' | 'right' | null {
  if (anchorSide === 'left') return 'right';
  if (anchorSide === 'right') return 'left';
  return null;
}

// -- Panel rendering --

function renderPanel(
  cl: Cluster,
  key: string,
  side: 'left' | 'right',
): m.Children {
  const ts = findTrace(cl, key);
  if (!ts) return m('.qs-cc-panel', 'Trace not found');
  ensureCache(ts);
  const isAnchor = anchorKey === key;
  const state = getCrossCompareState();
  const aa = anchorActive(state);
  const isSelected = !aa && state?.selectedSide === side;

  const panelClass =
    '.qs-cc-panel' +
    (isAnchor ? '.qs-cc-anchored' : isSelected ? '.qs-cc-selected' : '');

  return m(
    panelClass,
    {
      onclick: (e: Event) => {
        e.stopPropagation();
        toggleAnchor(key, side);
      },
    },
    [
      m('.qs-cc-panel-header', [
        m('span.qs-cc-panel-idx', `#${traceIndex(cl, key) + 1}`),
        m('span.qs-cc-panel-pkg', ts.trace.package_name),
        ts.trace.startup_dur
          ? m('span.qs-cc-panel-dur', fmtDur(ts.trace.startup_dur))
          : null,
        (() => {
          const href = buildTraceLink(
            ts.trace.trace_uuid,
            ts.trace.package_name,
          );
          return href
            ? m(
                'a.qs-cc-trace-link',
                {
                  href,
                  target: '_blank',
                  rel: 'noopener',
                  onclick: (e: Event) => e.stopPropagation(),
                  title: 'Open in trace viewer',
                },
                '\u2197',
              )
            : null;
        })(),
        isAnchor ? m('span.qs-cc-anchor-badge', 'anchor') : null,
      ]),
      m(MiniTimeline, {ts}),
      m(
        '.qs-cc-panel-detail',
        m(SummaryTables, {
          ts,
          sortState: panelSortState,
        }),
      ),
    ],
  );
}

function renderReviewTraceRow(cl: Cluster, key: string): m.Children {
  const ts = findTrace(cl, key);
  if (!ts) return null;
  ensureCache(ts);
  const href = buildTraceLink(ts.trace.trace_uuid, ts.trace.package_name);

  return m('.qs-cc-review-row', [
    m('.qs-cc-review-row-header', [
      m('span.qs-cc-panel-idx', `#${traceIndex(cl, key) + 1}`),
      m('span.qs-cc-panel-pkg', ts.trace.package_name),
      ts.trace.startup_dur
        ? m('span.qs-cc-panel-dur', fmtDur(ts.trace.startup_dur))
        : null,
      href
        ? m(
            'a.qs-cc-trace-link',
            {
              href,
              target: '_blank',
              rel: 'noopener',
              title: 'Open in trace viewer',
            },
            '\u2197',
          )
        : null,
    ]),
    m(MiniTimeline, {ts}),
  ]);
}

// -- Review screen --

function buildPairings(n: number): Array<[number, number]> {
  if (n < 2) return [[0, -1]];
  const pairs: Array<[number, number]> = [];
  for (let p = 0; p < n; p++) {
    for (let neg = 0; neg < n; neg++) {
      if (neg !== p) pairs.push([p, neg]);
    }
  }
  return pairs;
}

function cycleReview(delta: number, groupCount: number): void {
  const pairings = buildPairings(groupCount);
  if (pairings.length <= 1) return;
  reviewPairIdx =
    (((reviewPairIdx + delta) % pairings.length) + pairings.length) %
    pairings.length;
  m.redraw();
}

function renderReview(cl: Cluster): m.Children {
  const state = getCrossCompareState();
  if (!state) return null;
  const {groups, discarded} = getResults(state);

  // Pure anchor: anchor's group vs all others combined. Two groups, no cycling.
  const pureAnchor =
    anchorKey !== null && groups.some((g) => g.includes(anchorKey!));
  let positiveGroup: string[];
  let negativeGroup: string[];
  let pairings: Array<[number, number]>;
  let posIdx: number;
  let negIdx: number;

  if (pureAnchor) {
    const ai = groups.findIndex((g) => g.includes(anchorKey!));
    positiveGroup = groups[ai];
    negativeGroup = groups.flatMap((g, i) => (i === ai ? [] : g));
    posIdx = ai;
    negIdx = -1;
    pairings = [[posIdx, negIdx]];
  } else {
    pairings = buildPairings(groups.length);
    if (reviewPairIdx >= pairings.length) reviewPairIdx = 0;
    [posIdx, negIdx] = pairings[reviewPairIdx];
    positiveGroup = groups[posIdx] ?? [];
    negativeGroup = negIdx >= 0 ? groups[negIdx] ?? [] : [];
  }

  return m('.qs-cc-review', [
    m('.qs-cc-review-split', [
      m('.qs-cc-review-panel', [
        m('.qs-cc-review-panel-header.qs-cc-negative', [
          m('span', 'Negative'),
          m('span.qs-cc-review-count', `${negativeGroup.length}`),
        ]),
        m(
          '.qs-cc-review-panel-body',
          negativeGroup.map((key) => renderReviewTraceRow(cl, key)),
        ),
      ]),
      m('.qs-cc-review-panel', [
        m('.qs-cc-review-panel-header.qs-cc-positive', [
          m('span', 'Positive'),
          m('span.qs-cc-review-count', `${positiveGroup.length}`),
        ]),
        m(
          '.qs-cc-review-panel-body',
          positiveGroup.map((key) => renderReviewTraceRow(cl, key)),
        ),
      ]),
    ]),
    m('.qs-cc-review-nav', [
      pairings.length > 1
        ? m(
            'span.qs-cc-hint',
            `Pairing ${reviewPairIdx + 1} / ${pairings.length}`,
          )
        : null,
      discarded.length > 0
        ? m('span.qs-cc-hint', `${discarded.length} discarded`)
        : null,
    ]),
    pairings.length > 1
      ? m('.qs-cc-hint', '\u2190 \u2192 to cycle pairings')
      : null,
    m('.qs-cc-footer', [
      m(Button, {
        label: 'Apply',
        intent: Intent.Success,
        variant: ButtonVariant.Filled,
        onclick: () => applyCrossCompareResults(cl, posIdx, negIdx),
      }),
      m(Button, {
        label: 'Undo',
        variant: ButtonVariant.Outlined,
        disabled: state.history.length === 0,
        onclick: () => {
          undoCrossComparison(anchorKey ?? undefined);
          ensureAnchorSide();
        },
      }),
      m(Button, {
        label: 'Reset',
        variant: ButtonVariant.Outlined,
        onclick: () => {
          resetCrossCompare(cl);
          clearAnchor();
        },
      }),
      m(Button, {
        label: 'Close',
        variant: ButtonVariant.Outlined,
        onclick: closeCrossCompare,
      }),
    ]),
  ]);
}

// -- Main component --

export const CrossCompareModal: m.Component<{cl: Cluster}> = {
  oncreate(vnode: m.VnodeDOM<{cl: Cluster}>) {
    const getCl = () => vnode.attrs.cl;
    keyHandler = (e: KeyboardEvent) => {
      const target = e.target;
      if (target instanceof HTMLElement) {
        const tag = target.tagName;
        if (tag === 'INPUT' || tag === 'TEXTAREA') return;
      }
      const state = getCrossCompareState();
      if (!state) return;

      if (e.key === 'Escape') {
        closeCrossCompare();
        return;
      }
      if (e.key === 'z' && (e.ctrlKey || e.metaKey)) {
        e.preventDefault();
        undoCrossComparison(anchorKey ?? undefined);
        ensureAnchorSide();
        return;
      }

      if (state.isComplete) {
        const {groups} = getResults(state);
        if (e.key === 'ArrowLeft') {
          cycleReview(-1, groups.length);
          return;
        }
        if (e.key === 'ArrowRight') {
          cycleReview(1, groups.length);
          return;
        }
        return;
      }

      // Comparison keys -- pass anchorKey so advancePair tries anchor-first
      const ak = anchorKey ?? undefined;
      const aa = anchorActive(state);

      if (e.key === 'p' || e.key === 'P') {
        recordCrossComparison('positive', ak);
        ensureAnchorSide();
        return;
      }
      if (e.key === 'n' || e.key === 'N') {
        recordCrossComparison('negative', ak);
        ensureAnchorSide();
        return;
      }
      if (e.key === 's' || e.key === 'S') {
        skipCrossComparison(ak);
        ensureAnchorSide();
        return;
      }
      if (e.key === 'd' || e.key === 'D') {
        const side = aa ? discardSideForAnchor() : state.selectedSide;
        if (side) {
          discardCrossCompareTrace(getCl(), side, ak);
          ensureAnchorSide();
        }
        return;
      }

      // Arrow keys: select side when anchor not active in current pair
      if (!aa) {
        if (e.key === 'ArrowLeft') {
          state.selectedSide = 'left';
          m.redraw();
          return;
        }
        if (e.key === 'ArrowRight') {
          state.selectedSide = 'right';
          m.redraw();
          return;
        }
      }
    };
    document.addEventListener('keydown', keyHandler);
  },

  onremove() {
    if (keyHandler) {
      document.removeEventListener('keydown', keyHandler);
      keyHandler = null;
    }
  },

  onupdate(vnode: m.VnodeDOM<{cl: Cluster}>) {
    const {cl} = vnode.attrs;
    const state = getCrossCompareState();
    if (!state) return;

    // Apply slider to new pairs when they change.
    const pairKey = state.currentPair
      ? state.currentPair[0] + '|' + state.currentPair[1]
      : null;
    if (pairKey && pairKey !== lastPairKey) {
      lastPairKey = pairKey;
      if (ccSliderPct < 100) updateBothSliders(cl, ccSliderPct);
    }

    // Clear anchor if discarded.
    if (anchorKey && state.discardedKeys.has(anchorKey)) clearAnchor();
  },

  view(vnode: m.Vnode<{cl: Cluster}>) {
    const {cl} = vnode.attrs;
    const state = getCrossCompareState();
    if (!state) return null;

    const progress = getProgress(state);

    const active = anchorActive(state);
    const canDiscard = active || !!state.selectedSide;

    return m(
      '.qs-cc-overlay',
      {
        onclick: () => {
          if (anchorKey) {
            clearAnchor();
            m.redraw();
          } else if (state.selectedSide) {
            state.selectedSide = null;
            m.redraw();
          } else {
            closeCrossCompare();
          }
        },
      },
      [
        m(
          '.qs-cc-modal',
          {
            onclick: (e: Event) => {
              e.stopPropagation();
            },
          },
          [
            // Header
            m('.qs-cc-header', [
              m('span.qs-cc-title', 'Compare'),
              m(Button, {
                label: '\u00d7',
                variant: ButtonVariant.Minimal,
                compact: true,
                onclick: closeCrossCompare,
                title: 'Close (Esc)',
                className: 'qs-cc-close',
              }),
            ]),

            // Progress bar
            m('.qs-cc-progress', [
              m(
                '.qs-cc-progress-text',
                `${progress.completed} / ${progress.total} pairs resolved (${progress.pct}%)`,
              ),
              m('.qs-cc-progress-bar', [
                m('.qs-cc-progress-fill', {
                  style: {width: progress.pct + '%'},
                }),
              ]),
            ]),

            // Body
            state.isComplete
              ? renderReview(cl)
              : state.currentPair
                ? m('.qs-cc-body', [
                    m('.qs-cc-pair', [
                      renderPanel(cl, state.currentPair[0], 'left'),
                      m('.qs-cc-pair-divider', 'vs'),
                      renderPanel(cl, state.currentPair[1], 'right'),
                    ]),
                    m('.qs-cc-slider', [
                      m('span.qs-cc-slider-label', 'Detail'),
                      m('span.qs-cc-slider-num', ccSliderPct + '%'),
                      m('input[type=range]', {
                        min: 1,
                        max: 100,
                        value: ccSliderPct,
                        step: 1,
                        oninput: (e: Event) => {
                          const el = e.target as HTMLInputElement;
                          updateBothSliders(cl, +el.value);
                        },
                        onchange: (e: Event) => {
                          (e.target as HTMLElement).blur();
                        },
                      }),
                    ]),
                    m('.qs-cc-actions', [
                      m(Button, {
                        label: 'Positive (P)',
                        intent: Intent.Success,
                        variant: ButtonVariant.Filled,
                        onclick: () => {
                          recordCrossComparison(
                            'positive',
                            anchorKey ?? undefined,
                          );
                          ensureAnchorSide();
                        },
                      }),
                      m(Button, {
                        label: 'Negative (N)',
                        intent: Intent.Danger,
                        variant: ButtonVariant.Filled,
                        onclick: () => {
                          recordCrossComparison(
                            'negative',
                            anchorKey ?? undefined,
                          );
                          ensureAnchorSide();
                        },
                      }),
                      m(Button, {
                        label: 'Skip (S)',
                        variant: ButtonVariant.Outlined,
                        onclick: () => {
                          skipCrossComparison(anchorKey ?? undefined);
                          ensureAnchorSide();
                        },
                      }),
                      m(Button, {
                        label: 'Discard (D)',
                        intent: Intent.Warning,
                        variant: ButtonVariant.Outlined,
                        disabled: !canDiscard,
                        title: active
                          ? 'Discard the non-anchor trace'
                          : state.selectedSide
                            ? 'Discard selected trace'
                            : 'Click a panel to anchor, or \u2190\u2192 to select',
                        onclick: () => {
                          const side = active
                            ? discardSideForAnchor()
                            : state.selectedSide;
                          if (side) {
                            discardCrossCompareTrace(
                              cl,
                              side,
                              anchorKey ?? undefined,
                            );
                            ensureAnchorSide();
                          }
                        },
                      }),
                      m(Button, {
                        label: 'Undo (\u2318Z)',
                        variant: ButtonVariant.Outlined,
                        disabled: state.history.length === 0,
                        title: 'Undo (Ctrl+Z)',
                        onclick: () => {
                          undoCrossComparison(anchorKey ?? undefined);
                          ensureAnchorSide();
                        },
                      }),
                    ]),
                    m(
                      '.qs-cc-hint',
                      active
                        ? 'Anchored \u00b7 P/N/S/D apply to other trace \u00b7 click anchor to deselect'
                        : anchorKey
                          ? 'Anchor set (not in this pair) \u00b7 \u2190\u2192 to select \u00b7 click panel to re-anchor'
                          : 'Click a panel to anchor \u00b7 \u2190\u2192 to select for discard \u00b7 Esc close',
                    ),
                    m('.qs-cc-footer', [
                      m(Button, {
                        label: 'Apply Current Results',
                        variant: ButtonVariant.Outlined,
                        onclick: () => applyCrossCompareResults(cl),
                      }),
                      m(Button, {
                        label: 'Reset',
                        variant: ButtonVariant.Outlined,
                        onclick: () => {
                          resetCrossCompare(cl);
                          clearAnchor();
                        },
                      }),
                    ]),
                  ])
                : null,
          ],
        ),
      ],
    );
  },
};
