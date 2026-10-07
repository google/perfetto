// Copyright (C) 2024 The Android Open Source Project
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

import './flamegraph.scss';
import m from 'mithril';
import {ensureExists, assertTrue, assertUnreachable} from '../base/assert';
import {Monitor} from '../base/monitor';
import {Button, ButtonBar} from './button';
import {copyToClipboard} from '../base/clipboard';
import {EmptyState} from './empty_state';
import type {ExportFormat} from './export_button';
import {
  formatAsTSV,
  formatAsJSON,
  formatAsMarkdown,
} from '../base/export_formatters';
import {Popup, PopupPosition} from './popup';
import {Spinner} from './spinner';
import type {Rect2D, Size2D} from '../base/geom';
import {
  VirtualOverlayCanvas,
  type VirtualOverlayCanvasApi,
} from './virtual_overlay_canvas';
import {MenuItem, type MenuItemAttrs, PopupMenu} from './menu';
import {type Color, HSLColor} from '../base/color';
import {hash} from '../base/hash';
import {escapeRegex} from './flamegraph_regex';
import type {MithrilEvent} from '../base/mithril_utils';
import {Icons} from '../base/semantic_icons';
import {
  type ActionCategory,
  FILTER_TYPES,
  type FilterType,
  type TreeExplorerData,
  type TreeExplorerFilter,
  type TreeExplorerMetric,
  type TreeExplorerNode,
  type TreeExplorerOptionalAction,
  type TreeExplorerPropertyDefinition,
  type TreeExplorerState,
  addFilter,
  displayPercentage,
  displaySize,
  getUnitDisplayName,
  metricId,
} from './tree_explorer';

const LABEL_FONT_STYLE = '12px Roboto';
const NODE_HEIGHT = 20;
const MIN_PIXEL_DISPLAYED = 3;
const LABEL_PADDING_PX = 5;
const LABEL_MIN_WIDTH_FOR_TEXT_PX = 5;
const PADDING_NODE_COUNT = 8;

interface BaseSource {
  readonly queryXStart: number;
  readonly queryXEnd: number;
  readonly type: 'ABOVE_ROOT' | 'BELOW_ROOT' | 'ROOT';
}

interface MergedSource extends BaseSource {
  readonly kind: 'MERGED';
}

interface RootSource extends BaseSource {
  readonly kind: 'ROOT';
}

interface NodeSource extends BaseSource {
  readonly kind: 'NODE';
  readonly queryIdx: number;
}

type Source = MergedSource | NodeSource | RootSource;

interface RenderNode {
  readonly x: number;
  readonly y: number;
  readonly width: number;
  readonly source: Source;
  readonly state: 'NORMAL' | 'PARTIAL' | 'SELECTED';
}

interface ZoomRegion {
  readonly queryXStart: number;
  readonly queryXEnd: number;
  readonly type: 'ABOVE_ROOT' | 'BELOW_ROOT' | 'ROOT';
}

const CATEGORY_LABELS: Record<ActionCategory, string> = {
  FOCUS: 'Focus',
  FILTER: 'Filter',
  DRILL: 'Drill down',
  COPY: 'Copy',
};

const CATEGORY_ORDER: ReadonlyArray<ActionCategory> = [
  'FOCUS',
  'FILTER',
  'DRILL',
  'COPY',
];

const CATEGORY_ICONS: Record<ActionCategory, string> = {
  FOCUS: 'center_focus_weak',
  FILTER: 'filter_list',
  DRILL: 'open_in_new',
  COPY: 'content_copy',
};

interface NodeAction {
  readonly label: string;
  readonly icon: string;
  readonly description?: m.Children;
  readonly category: ActionCategory;
  execute(): void;
}

export interface FlamegraphAttrs {
  readonly metrics: ReadonlyArray<TreeExplorerMetric>;
  readonly state: TreeExplorerState;
  readonly data: TreeExplorerData | undefined;

  // Nodes whose name matches are drawn with a highlighted border. Owned by
  // the caller so the (separate) filter bar can drive it.
  readonly highlightRegex?: RegExp;

  readonly onStateChange: (state: TreeExplorerState) => void;
}

/*
 * Widget visualizing "tree-like" data structures using an interactive
 * flamegraph visualization: just the canvas and its tooltip. The filtering
 * bar lives in the separate TreeExplorerFilterBar widget; most callers should
 * use TreeExplorerPanel (in components/) which composes the two and fetches
 * the data.
 *
 * Note that it's valid to pass "undefined" as the data: this will cause a
 * loading container to be shown.
 */
export class Flamegraph implements m.ClassComponent<FlamegraphAttrs> {
  private attrs: FlamegraphAttrs;

  private dataChangeMonitor = new Monitor([() => this.attrs.data]);
  private zoomRegion?: ZoomRegion;
  private canvasApi?: VirtualOverlayCanvasApi;
  private pendingScrollToY?: number;

  private renderNodesMonitor = new Monitor([
    () => this.attrs.data,
    () => this.canvasWidth,
    () => this.zoomRegion,
  ]);
  private renderNodes?: ReadonlyArray<RenderNode>;

  private tooltipPos?: {
    x: number;
    y: number;
    source: Source;
    state: 'HOVER' | 'CLICK' | 'DECLICK';
  };
  private lastClickedNode?: RenderNode;

  // Track hovered node by index to avoid redraws when mouse moves within same node.
  // We also keep hoveredX/Y so we can re-find the node after render nodes change.
  private hoveredNodeIdx?: number;
  private hoveredX?: number;
  private hoveredY?: number;

  private canvasWidth = 0;
  private labelCharWidth = 0;
  private viewportRect?: Rect2D;
  private lastPopupVisible = false;

  constructor({attrs}: m.Vnode<FlamegraphAttrs, {}>) {
    this.attrs = attrs;
  }

  oncreate() {
    this.flushPendingScroll();
  }

  onupdate() {
    this.flushPendingScroll();
  }

  private flushPendingScroll() {
    if (this.pendingScrollToY === undefined || this.canvasApi === undefined) {
      return;
    }
    const requestedY = this.pendingScrollToY;
    const {y: appliedY} = this.canvasApi.scrollTo({y: requestedY});
    if (requestedY !== Number.MAX_SAFE_INTEGER && appliedY !== requestedY) {
      // Keep shallow graphs aligned to whole rows when the target is unreachable.
      this.canvasApi.scrollTo({y: 0});
    }
    this.pendingScrollToY = undefined;
  }

  view({attrs}: m.Vnode<FlamegraphAttrs, this>): void | m.Children {
    this.attrs = attrs;
    if (this.dataChangeMonitor.ifStateChanged()) {
      this.zoomRegion = undefined;
      this.lastClickedNode = undefined;
      this.tooltipPos = undefined;
      // Auto-scroll so the root (depth 0) is visible. In TOP_DOWN the root
      // sits at the top of the canvas; in BOTTOM_UP it sits near the bottom
      // (only callers/leaves above it); in PIVOT it sits somewhere in the
      // middle with callers above and callees below.
      if (attrs.data !== undefined) {
        if (attrs.state.view.kind === 'BOTTOM_UP') {
          // Large value — the browser clamps to scrollHeight - clientHeight.
          this.pendingScrollToY = Number.MAX_SAFE_INTEGER;
        } else {
          const rootY = -attrs.data.minDepth * NODE_HEIGHT;
          this.pendingScrollToY = Math.max(0, rootY - NODE_HEIGHT);
        }
      }
    }
    if (attrs.data === undefined) {
      this.canvasApi = undefined;
      return m(
        '.pf-flamegraph',
        m(
          '.loading-container',
          m(
            EmptyState,
            {
              icon: 'bar_chart',
              title: 'Computing graph ...',
              className: 'flamegraph-loading',
            },
            m(Spinner, {easing: true}),
          ),
        ),
      );
    }
    const {minDepth, maxDepth} = attrs.data;
    const canvasHeight =
      Math.max(maxDepth - minDepth + PADDING_NODE_COUNT, PADDING_NODE_COUNT) *
      NODE_HEIGHT;
    const hoveredNode =
      this.hoveredNodeIdx !== undefined
        ? this.renderNodes?.[this.hoveredNodeIdx]
        : undefined;
    return m(
      '.pf-flamegraph',
      m(
        VirtualOverlayCanvas,
        {
          className: 'pf-virtual-canvas',
          overflowX: 'hidden',
          overflowY: 'auto',
          onMount: (api) => {
            this.canvasApi = api;
            this.flushPendingScroll();
          },
          onscroll: (e: MithrilEvent<Event>) => {
            // Only redraw if popup visibility would change
            if (!this.tooltipPos) {
              e.redraw = false;
              return;
            }
            const target = e.target as HTMLElement;
            const scrollTop = target.scrollTop;
            const clientHeight = target.clientHeight;
            const tooltipY = this.tooltipPos.y;
            const nowVisible =
              tooltipY >= scrollTop && tooltipY <= scrollTop + clientHeight;
            if (nowVisible === this.lastPopupVisible) {
              e.redraw = false;
            }
          },
          onCanvasRedraw: ({
            ctx,
            virtualCanvasSize,
            canvasRect,
            viewportRect,
          }) => {
            this.drawCanvas(ctx, virtualCanvasSize, canvasRect, viewportRect);
          },
        },
        m(
          'div',
          {
            style: {
              height: `${canvasHeight}px`,
              cursor: hoveredNode === undefined ? 'default' : 'pointer',
            },
            onmousemove: (e: MithrilEvent<MouseEvent>) => {
              const {offsetX, offsetY} = e;
              this.hoveredX = offsetX;
              this.hoveredY = offsetY;

              const nodeIdx = this.renderNodes?.findIndex((n) =>
                isIntersecting(offsetX, offsetY, n),
              );
              const newHoveredIdx =
                nodeIdx !== undefined && nodeIdx !== -1 ? nodeIdx : undefined;

              if (newHoveredIdx === this.hoveredNodeIdx) {
                e.redraw = false;
                return;
              }
              this.hoveredNodeIdx = newHoveredIdx;

              if (this.tooltipPos?.state === 'CLICK') {
                return;
              }
              if (newHoveredIdx === undefined) {
                this.tooltipPos = undefined;
                return;
              }
              const renderNode = this.renderNodes![newHoveredIdx];
              this.tooltipPos = {
                x: offsetX,
                y: renderNode.y,
                source: renderNode.source,
                state: 'HOVER',
              };
            },
            onmouseout: () => {
              this.hoveredNodeIdx = undefined;
              this.hoveredX = undefined;
              this.hoveredY = undefined;
              if (
                this.tooltipPos?.state === 'HOVER' ||
                this.tooltipPos?.state === 'DECLICK'
              ) {
                this.tooltipPos = undefined;
              }
            },
            onclick: ({offsetX, offsetY}: MouseEvent) => {
              const renderNode = this.renderNodes?.find((n) =>
                isIntersecting(offsetX, offsetY, n),
              );
              this.lastClickedNode = renderNode;
              if (renderNode === undefined) {
                this.tooltipPos = undefined;
              } else if (
                isIntersecting(
                  this.tooltipPos?.x,
                  this.tooltipPos?.y,
                  renderNode,
                )
              ) {
                this.tooltipPos!.state =
                  this.tooltipPos?.state === 'CLICK' ? 'DECLICK' : 'CLICK';
              } else {
                this.tooltipPos = {
                  x: offsetX,
                  y: renderNode.y,
                  source: renderNode.source,
                  state: 'CLICK',
                };
              }
            },
            ondblclick: ({offsetX, offsetY}: MouseEvent) => {
              const renderNode = this.renderNodes?.find((n) =>
                isIntersecting(offsetX, offsetY, n),
              );
              if (renderNode?.source.kind === 'MERGED') {
                return;
              }
              this.zoomRegion = renderNode?.source;
            },
          },
          (() => {
            const popupVisible =
              this.isPopupAnchorVisible() &&
              (this.tooltipPos?.state === 'HOVER' ||
                this.tooltipPos?.state === 'CLICK');
            this.lastPopupVisible = popupVisible;
            return m(
              Popup,
              {
                trigger: m('.popup-anchor', {
                  style: {
                    left: this.tooltipPos?.x + 'px',
                    top: this.tooltipPos?.y + 'px',
                  },
                }),
                fitContent: true,
                position: PopupPosition.Right,
                isOpen: popupVisible,
                className: 'pf-flamegraph-tooltip-popup',
                offset: NODE_HEIGHT,
              },
              this.renderTooltip(),
            );
          })(),
        ),
      ),
    );
  }

  private drawCanvas(
    ctx: CanvasRenderingContext2D,
    size: Size2D,
    canvasRect: Rect2D,
    viewportRect: Rect2D,
  ) {
    this.viewportRect = viewportRect;
    this.canvasWidth = size.width;

    if (this.renderNodesMonitor.ifStateChanged()) {
      if (this.attrs.data === undefined) {
        this.renderNodes = undefined;
        this.lastClickedNode = undefined;
      } else {
        this.renderNodes = computeRenderNodes(
          this.attrs.data,
          this.zoomRegion ?? {
            queryXStart: 0,
            queryXEnd: this.attrs.data.allRootsCumulativeValue,
            type: 'ROOT',
          },
          size.width,
        );
        this.lastClickedNode = this.renderNodes?.find((n) =>
          isIntersecting(this.lastClickedNode?.x, this.lastClickedNode?.y, n),
        );
      }
      this.tooltipPos = undefined;
      // Re-find hovered node using stored coordinates
      const nodeIdx = this.renderNodes?.findIndex((n) =>
        isIntersecting(this.hoveredX, this.hoveredY, n),
      );
      this.hoveredNodeIdx =
        nodeIdx !== undefined && nodeIdx !== -1 ? nodeIdx : undefined;
    }
    if (this.attrs.data === undefined || this.renderNodes === undefined) {
      return;
    }

    const yStart = canvasRect.top;
    const yEnd = canvasRect.bottom;

    const {allRootsCumulativeValue, unfilteredCumulativeValue, nodes} =
      this.attrs.data;
    const unit = ensureExists(this.selectedMetric).unit;

    ctx.font = LABEL_FONT_STYLE;
    ctx.textBaseline = 'middle';

    ctx.strokeStyle = 'white';
    ctx.lineWidth = 0.5;

    if (this.labelCharWidth === 0) {
      this.labelCharWidth = ctx.measureText('_').width;
    }
    const highlightColor = getComputedStyle(ctx.canvas)
      .getPropertyValue('--pf-color-accent')
      .trim();

    for (let i = 0; i < this.renderNodes.length; i++) {
      const node = this.renderNodes[i];
      const {x, y, width: width, source, state} = node;
      if (y + NODE_HEIGHT <= yStart || y >= yEnd) {
        continue;
      }

      const hover = isIntersecting(this.hoveredX, this.hoveredY, node);
      let name: string;
      let colorScheme;
      if (source.kind === 'ROOT') {
        const val = displaySize(allRootsCumulativeValue, unit);
        const percent = displayPercentage(
          allRootsCumulativeValue,
          unfilteredCumulativeValue,
        );
        name = `root: ${val} (${percent})`;
        colorScheme = getFlamegraphColorScheme('root', state === 'PARTIAL');
      } else if (source.kind === 'MERGED') {
        name = '(merged)';
        colorScheme = getFlamegraphColorScheme(name, state === 'PARTIAL');
      } else {
        name = nodes[source.queryIdx].name;
        colorScheme = getFlamegraphColorScheme(name, state === 'PARTIAL');
      }
      const highlighted =
        source.kind === 'NODE' &&
        this.attrs.highlightRegex?.test(name) === true;
      const bgColor = hover ? colorScheme.variant : colorScheme.base;
      const textColor = hover ? colorScheme.textVariant : colorScheme.textBase;
      ctx.fillStyle = bgColor.cssString;
      ctx.fillRect(x, y, width - 1, NODE_HEIGHT - 1);

      // Render marker
      const MARKER_SIZE = 3;
      const MARKER_LEFT_MARGIN = 2;
      const MIN_WIDTH_FOR_MARKER = 15; // Don't show marker on very small nodes
      const hasMarker =
        source.kind === 'NODE' &&
        nodes[source.queryIdx].marker !== undefined &&
        width >= MIN_WIDTH_FOR_MARKER;
      if (hasMarker) {
        ctx.fillStyle = textColor.cssString;
        const markerX = x + MARKER_LEFT_MARGIN;
        const markerY = y + 2; // Position at top of node with small margin
        ctx.fillRect(markerX, markerY, MARKER_SIZE, MARKER_SIZE);
      }

      // Text positioning - no need to reserve space since marker is in top corner
      const widthNoPadding = width - LABEL_PADDING_PX * 2;
      if (widthNoPadding >= LABEL_MIN_WIDTH_FOR_TEXT_PX) {
        ctx.fillStyle = textColor.cssString;
        ctx.fillText(
          name.substring(0, widthNoPadding / this.labelCharWidth),
          x + LABEL_PADDING_PX,
          y + (NODE_HEIGHT - 1) / 2,
          widthNoPadding,
        );
      }
      if (highlighted) {
        const highlightWidth = Math.max(0, width - 3);
        const highlightHeight = NODE_HEIGHT - 3;
        ctx.fillStyle = highlightColor || textColor.cssString;
        ctx.fillRect(x + 1, y + 1, highlightWidth, 2);
        ctx.fillRect(x + 1, y + NODE_HEIGHT - 3, highlightWidth, 2);
        ctx.fillRect(x + 1, y + 1, 2, highlightHeight);
        ctx.fillRect(x + width - 3, y + 1, 2, highlightHeight);
      }
      if (this.lastClickedNode?.x === x && this.lastClickedNode?.y === y) {
        ctx.strokeStyle = 'blue';
        ctx.lineWidth = 2;
        ctx.beginPath();
        ctx.moveTo(x, y);
        ctx.lineTo(x + width, y);
        ctx.lineTo(x + width, y + NODE_HEIGHT - 1);
        ctx.lineTo(x, y + NODE_HEIGHT - 1);
        ctx.lineTo(x, y);
        ctx.stroke();
        ctx.strokeStyle = 'white';
        ctx.lineWidth = 0.5;
      }
    }
  }

  private isPopupAnchorVisible(): boolean {
    if (!this.tooltipPos || !this.viewportRect) {
      return false;
    }
    const {y} = this.tooltipPos;
    return y >= this.viewportRect.top && y <= this.viewportRect.bottom;
  }

  private renderTooltip() {
    if (this.tooltipPos === undefined) {
      return undefined;
    }
    const {source} = this.tooltipPos;
    if (source.kind === 'MERGED') {
      return m(
        'div',
        m('.tooltip-bold-text', '(merged)'),
        m('.tooltip-text', 'Nodes too small to show, please use filters'),
      );
    }
    const {
      nodes,
      allRootsCumulativeValue,
      unfilteredCumulativeValue,
      nodeActions,
      rootActions,
    } = ensureExists(this.attrs.data);
    const {unit, nameColumnLabel} = ensureExists(this.selectedMetric);
    if (source.kind === 'ROOT') {
      const val = displaySize(allRootsCumulativeValue, unit);
      const percent = displayPercentage(
        allRootsCumulativeValue,
        unfilteredCumulativeValue,
      );
      return m(
        'div',
        m('.tooltip-bold-text', 'root'),
        m(
          '.tooltip-text-line',
          m('.tooltip-bold-text', 'Cumulative:'),
          m('.tooltip-text', `${val}, ${percent}`),
          this.renderActionsMenu(rootActions, new Map()),
        ),
      );
    }
    const {queryIdx} = source;
    const {
      name,
      cumulativeValue,
      selfValue,
      parentCumulativeValue,
      properties,
      marker,
    } = nodes[queryIdx];

    const percent = displayPercentage(
      cumulativeValue,
      unfilteredCumulativeValue,
    );
    const selfPercent = displayPercentage(selfValue, unfilteredCumulativeValue);

    let percentText = `all: ${percent}`;
    let selfPercentText = `all: ${selfPercent}`;
    if (parentCumulativeValue !== undefined) {
      const parentPercent = displayPercentage(
        cumulativeValue,
        parentCumulativeValue,
      );
      percentText += `, parent: ${parentPercent}`;
      const parentSelfPercent = displayPercentage(
        selfValue,
        parentCumulativeValue,
      );
      selfPercentText += `, parent: ${parentSelfPercent}`;
    }
    const nameLabel = nameColumnLabel ?? 'Name';
    return m(
      'div',
      // Show marker at the top of the tooltip
      marker &&
        m('.tooltip-text-line', m('.tooltip-marker-text', `■ ${marker}`)),
      m(
        '.tooltip-text-line',
        m('.tooltip-bold-text', `${nameLabel}:`),
        m('.tooltip-text', name),
      ),
      m(
        '.tooltip-text-line',
        m('.tooltip-bold-text', 'Cumulative:'),
        m(
          '.tooltip-text',
          `${displaySize(cumulativeValue, unit)} (${percentText})`,
        ),
      ),
      m(
        '.tooltip-text-line',
        m('.tooltip-bold-text', 'Self:'),
        m(
          '.tooltip-text',
          `${displaySize(selfValue, unit)} (${selfPercentText})`,
        ),
      ),
      Array.from(properties, ([_, value]) => {
        if (value.isVisible) {
          return m(
            '.tooltip-text-line',
            m('.tooltip-bold-text', value.displayName + ':'),
            m('.tooltip-text', value.value),
          );
        }
        return null;
      }),
      this.renderNodeActionBar(source, name, nodes[queryIdx], nodeActions),
    );
  }

  // One dropdown per category. Built-ins come from buildNodeActions; flat
  // embedder actions slot into their declared category, nested/disabled ones
  // keep the renderMenuItem path under Drill down.
  private renderNodeActionBar(
    source: NodeSource,
    name: string,
    node: TreeExplorerNode,
    nodeActions: ReadonlyArray<TreeExplorerOptionalAction>,
  ) {
    const {properties} = node;
    const builtIn = this.buildNodeActions(source, name, node);

    const isFlat = (a: TreeExplorerOptionalAction) =>
      a.execute !== undefined &&
      (a.subActions === undefined || a.subActions.length === 0);
    const embedderFlat: NodeAction[] = nodeActions.filter(isFlat).map((a) => ({
      label: a.name,
      icon: a.icon ?? 'open_in_new',
      description: a.description,
      category: a.category ?? 'DRILL',
      execute: () => {
        a.execute!({
          properties: this.createReducedProperties(properties),
          node,
        });
        this.tooltipPos = undefined;
      },
    }));
    const embedderComplex = nodeActions
      .filter((a) => !isFlat(a))
      .map((a) => this.renderMenuItem(a, properties, node));

    const actions = [...builtIn, ...embedderFlat];
    return m(
      ButtonBar,
      {className: 'pf-flamegraph-action-bar'},
      CATEGORY_ORDER.map((cat) => {
        const items = actions.filter((a) => a.category === cat);
        const extra = cat === 'DRILL' ? embedderComplex : [];
        if (items.length === 0 && extra.length === 0) return null;
        return m(
          PopupMenu,
          {
            trigger: m(Button, {
              label: CATEGORY_LABELS[cat],
              icon: CATEGORY_ICONS[cat],
              rightIcon: 'arrow_drop_down',
              compact: true,
            }),
            position: PopupPosition.Bottom,
            className: 'pf-popup-menu pf-flamegraph-action-menu',
          },
          items.map((a) => this.renderNodeActionItem(a)),
          extra,
        );
      }),
    );
  }

  private renderNodeActionItem(a: NodeAction): m.Children {
    return m(MenuItem, {
      icon: a.icon,
      label: this.actionItemLabel(a.label, a.description),
      onclick: a.execute,
    });
  }

  // Two-line menu label: name on top, muted description beneath. A name with no
  // description renders as a plain single line.
  private actionItemLabel(
    label: m.Children,
    description?: m.Children,
  ): m.Children {
    if (description == null) return label;
    return m(
      '.pf-flamegraph-action',
      m('.pf-flamegraph-action__title', label),
      m('.pf-flamegraph-action__desc', description),
    );
  }

  private buildNodeActions(
    source: NodeSource,
    name: string,
    node: TreeExplorerNode,
  ): NodeAction[] {
    const applyState = (state: TreeExplorerState) => {
      this.attrs.onStateChange(state);
      this.tooltipPos = undefined;
    };
    const addF = (kind: TreeExplorerFilter['kind'], filter: string) =>
      applyState(addFilter(this.attrs.state, {kind, filter}));
    // Match this exact name: an anchored regex over the escaped literal name,
    // wrapped in `/…/` so the filter bar reads it as a regex.
    const exactNameRegex = `/^${escapeRegex(name)}$/`;
    const ft = (v: FilterType) =>
      ensureExists(FILTER_TYPES.find((o) => o.value === v));
    const filterAction = (v: FilterType, execute: () => void): NodeAction => {
      const o = ft(v);
      return {
        label: o.friendlyLabel,
        icon: o.icon,
        category: o.category,
        description: [
          o.description,
          o.aka !== undefined && [
            ' ',
            m(
              'span.pf-flamegraph-action__aka',
              `Also called "${o.aka}" in other profilers.`,
            ),
          ],
        ],
        execute,
      };
    };

    return [
      {
        label: 'Zoom in',
        icon: 'zoom_in',
        category: 'FOCUS',
        description:
          'Enlarge this branch. Nothing is removed, so you can zoom out anytime.',
        execute: () => {
          this.zoomRegion = source;
        },
      },
      {
        label: 'Show from this frame',
        icon: 'center_focus_strong',
        category: 'FOCUS',
        description:
          'Re-root at this frame and show its descendants, dropping ancestors.',
        execute: () =>
          applyState({
            ...this.attrs.state,
            view: {
              kind: 'FROM_FRAME',
              pattern: exactNameRegex,
              displayLabel: name,
            },
          }),
      },
      {
        label: 'Pivot on this frame',
        icon: 'account_tree',
        category: 'FOCUS',
        description:
          'Re-root at this frame with callers above and callees below.',
        execute: () =>
          applyState({
            ...this.attrs.state,
            view: {
              kind: 'PIVOT',
              pivot: exactNameRegex,
              displayLabel: name,
            },
          }),
      },
      filterAction('SHOW_STACK', () => addF('SHOW_STACK', exactNameRegex)),
      filterAction('HIDE_STACK', () => addF('HIDE_STACK', exactNameRegex)),
      filterAction('HIDE_FRAME', () => addF('HIDE_FRAME', exactNameRegex)),
      {
        label: 'Copy stack',
        icon: Icons.Copy,
        category: 'COPY',
        description: 'Copy this stack as text.',
        execute: () => copyToClipboard(this.buildStackString(node, false)),
      },
      {
        label: 'Copy stack with details',
        icon: Icons.Copy,
        category: 'COPY',
        description: 'Copy this stack with per-frame metrics and columns.',
        execute: () => copyToClipboard(this.buildStackString(node, true)),
      },
    ];
  }

  private get selectedMetric() {
    return this.attrs.metrics.find(
      (x) => metricId(x) === this.attrs.state.selectedMetricId,
    );
  }

  // Root-only actions menu; node actions go through renderNodeActionBar.
  private renderActionsMenu(
    actions: ReadonlyArray<TreeExplorerOptionalAction>,
    properties: ReadonlyMap<string, TreeExplorerPropertyDefinition>,
  ) {
    if (actions.length === 0) {
      return null;
    }

    return m(
      PopupMenu,
      {
        trigger: m(Button, {
          icon: 'menu',
          compact: true,
        }),
        position: PopupPosition.Bottom,
      },
      actions.map((action) => this.renderMenuItem(action, properties)),
    );
  }

  private renderMenuItem(
    action: TreeExplorerOptionalAction,
    properties: ReadonlyMap<string, TreeExplorerPropertyDefinition>,
    node?: TreeExplorerNode,
  ): m.Vnode<MenuItemAttrs> {
    if (action.subActions !== undefined && action.subActions.length > 0) {
      return this.renderParentMenuItem(
        action,
        action.subActions,
        properties,
        node,
      );
    } else if (action.execute) {
      return this.renderExecutableMenuItem(action, properties, node);
    } else {
      return this.renderDisabledMenuItem(action);
    }
  }

  private renderParentMenuItem(
    action: TreeExplorerOptionalAction,
    subActions: TreeExplorerOptionalAction[],
    properties: ReadonlyMap<string, TreeExplorerPropertyDefinition>,
    node?: TreeExplorerNode,
  ): m.Vnode<MenuItemAttrs> {
    return m(
      MenuItem,
      {
        label: this.actionItemLabel(action.name, action.description),
        icon: action.icon,
        // No onclick handler for parent menu items
      },
      // Directly render sub-actions as children of the MenuItem
      subActions.map((subAction) =>
        this.renderMenuItem(subAction, properties, node),
      ),
    );
  }

  private renderExecutableMenuItem(
    action: TreeExplorerOptionalAction,
    properties: ReadonlyMap<string, TreeExplorerPropertyDefinition>,
    node?: TreeExplorerNode,
  ): m.Vnode<MenuItemAttrs> {
    return m(MenuItem, {
      label: this.actionItemLabel(action.name, action.description),
      icon: action.icon,
      onclick: () => {
        action.execute!({
          properties: this.createReducedProperties(properties),
          node,
        });
        this.tooltipPos = undefined; // Close tooltip after action
      },
    });
  }

  private renderDisabledMenuItem(
    action: TreeExplorerOptionalAction,
  ): m.Vnode<MenuItemAttrs> {
    return m(MenuItem, {
      label: this.actionItemLabel(action.name, action.description),
      icon: action.icon,
      disabled: true,
    });
  }

  private buildStackString(
    node: TreeExplorerNode,
    withDetails: boolean,
  ): string {
    const {nodes, unfilteredCumulativeValue} = ensureExists(this.attrs.data);
    const metric = ensureExists(this.selectedMetric);
    const view = this.attrs.state.view;

    // Walk via parentId for all modes. Reverse for TOP_DOWN and PIVOT below.
    const stack: TreeExplorerNode[] = [];
    let currentId = node.id;
    while (currentId !== -1) {
      const current = ensureExists(nodes.find((n) => n.id === currentId));
      stack.push(current);
      currentId = current.parentId;
    }

    const shouldReverse =
      view.kind === 'TOP_DOWN' ||
      view.kind === 'FROM_FRAME' ||
      (view.kind === 'PIVOT' && node.depth > 0);
    if (shouldReverse) {
      stack.reverse();
    }

    if (!withDetails) {
      return stack.map((n) => n.name).join('\n');
    }

    // Collect all unique property keys, separated by aggregatable status
    const unaggKeys: string[] = [];
    const aggKeys: string[] = [];
    for (const entry of stack) {
      for (const [key, prop] of entry.properties) {
        if (prop.isAggregatable) {
          if (!aggKeys.includes(key)) {
            aggKeys.push(key);
          }
        } else {
          if (!unaggKeys.includes(key)) {
            unaggKeys.push(key);
          }
        }
      }
    }

    // Helper to get display name for a property key
    const getDisplayName = (key: string): string => {
      for (const entry of stack) {
        const prop = entry.properties.get(key);
        if (prop !== undefined) {
          return prop.displayName;
        }
      }
      return key;
    };

    // Build header: Name | Non-agg props | Cumulative | Self | Agg props
    const nameLabel = metric.nameColumnLabel ?? 'Name';
    const unitDisplay = getUnitDisplayName(metric.unit);
    const headers = [nameLabel];
    for (const key of unaggKeys) {
      headers.push(getDisplayName(key));
    }
    headers.push(
      `Cumulative ${metric.name} (${unitDisplay})`,
      `Self ${metric.name} (${unitDisplay})`,
    );
    for (const key of aggKeys) {
      headers.push(getDisplayName(key));
    }

    // Format as markdown table
    const lines: string[] = [];
    lines.push('| ' + headers.join(' | ') + ' |');
    lines.push('|' + headers.map(() => '------').join('|') + '|');

    for (const entry of stack) {
      const cumulative = displaySize(entry.cumulativeValue, metric.unit);
      const cumulativePercent = displayPercentage(
        entry.cumulativeValue,
        unfilteredCumulativeValue,
      );
      const self = displaySize(entry.selfValue, metric.unit);
      const selfPercent = displayPercentage(
        entry.selfValue,
        unfilteredCumulativeValue,
      );

      const cols = [entry.name];
      for (const key of unaggKeys) {
        cols.push(entry.properties.get(key)?.value ?? '');
      }
      cols.push(
        `${cumulative} (${cumulativePercent})`,
        `${self} (${selfPercent})`,
      );
      for (const key of aggKeys) {
        cols.push(entry.properties.get(key)?.value ?? '');
      }
      lines.push('| ' + cols.join(' | ') + ' |');
    }
    return lines.join('\n');
  }

  private createReducedProperties(
    properties: ReadonlyMap<string, TreeExplorerPropertyDefinition>,
  ): ReadonlyMap<string, string> {
    return new Map([...properties].map(([key, {value}]) => [key, value]));
  }
}

// Builds a table of all currently displayed nodes (i.e. with filters
// applied) in the given format. Metric values are raw numbers in the
// metric's unit so they can be aggregated in spreadsheets; the tree
// structure is preserved via the id/parentId columns.
export function buildFlamegraphExportString(
  data: TreeExplorerData,
  metric: TreeExplorerMetric,
  format: ExportFormat,
): string {
  const {nodes} = data;
  const unitDisplay = getUnitDisplayName(metric.unit);

  const unaggKeys: string[] = [];
  const aggKeys: string[] = [];
  const propDisplayNames = new Map<string, string>();
  for (const node of nodes) {
    for (const [key, prop] of node.properties) {
      const keys = prop.isAggregatable ? aggKeys : unaggKeys;
      if (!keys.includes(key)) {
        keys.push(key);
        propDisplayNames.set(key, prop.displayName);
      }
    }
  }

  const columns = [
    'id',
    'parentId',
    'depth',
    'name',
    ...unaggKeys,
    'cumulativeValue',
    'selfValue',
    ...aggKeys,
  ];
  const columnNames: Record<string, string> = {
    ...Object.fromEntries(propDisplayNames),
    id: 'Id',
    parentId: 'Parent Id',
    depth: 'Depth',
    name: metric.nameColumnLabel ?? 'Name',
    cumulativeValue: `Cumulative ${metric.name} (${unitDisplay})`,
    selfValue: `Self ${metric.name} (${unitDisplay})`,
  };
  const rows = nodes.map((n) => {
    const row: Record<string, string> = {
      id: n.id.toString(),
      parentId: n.parentId.toString(),
      depth: n.depth.toString(),
      name: n.name,
      cumulativeValue: n.cumulativeValue.toString(),
      selfValue: n.selfValue.toString(),
    };
    for (const key of [...unaggKeys, ...aggKeys]) {
      row[key] = n.properties.get(key)?.value ?? '';
    }
    return row;
  });

  switch (format) {
    case 'tsv':
      return formatAsTSV(columns, columnNames, rows);
    case 'json':
      return formatAsJSON(columns, columnNames, rows);
    case 'markdown':
      return formatAsMarkdown(columns, columnNames, rows);
    default:
      assertUnreachable(format);
  }
}

function computeRenderNodes(
  {nodes, allRootsCumulativeValue, minDepth}: TreeExplorerData,
  zoomRegion: ZoomRegion,
  canvasWidth: number,
): ReadonlyArray<RenderNode> {
  const renderNodes: RenderNode[] = [];

  const mergedKeyToX = new Map<string, number>();
  const keyToChildMergedIdx = new Map<string, number>();
  renderNodes.push({
    x: 0,
    y: -minDepth * NODE_HEIGHT,
    width: canvasWidth,
    source: {
      kind: 'ROOT',
      queryXStart: 0,
      queryXEnd: allRootsCumulativeValue,
      type: 'ROOT',
    },
    state:
      zoomRegion.queryXStart === 0 &&
      zoomRegion.queryXEnd === allRootsCumulativeValue
        ? 'NORMAL'
        : 'PARTIAL',
  });

  const zoomQueryWidth = zoomRegion.queryXEnd - zoomRegion.queryXStart;
  for (let i = 0; i < nodes.length; i++) {
    const {id, parentId, depth, xStart: qXStart, xEnd: qXEnd} = nodes[i];
    assertTrue(depth !== 0);

    const depthMatchingZoom = isDepthMatchingZoom(depth, zoomRegion);
    if (
      depthMatchingZoom &&
      (qXEnd <= zoomRegion.queryXStart || qXStart >= zoomRegion.queryXEnd)
    ) {
      continue;
    }
    const queryXPerPx = depthMatchingZoom
      ? zoomQueryWidth / canvasWidth
      : allRootsCumulativeValue / canvasWidth;
    const relativeXStart = depthMatchingZoom
      ? qXStart - zoomRegion.queryXStart
      : qXStart;
    const relativeXEnd = depthMatchingZoom
      ? qXEnd - zoomRegion.queryXStart
      : qXEnd;
    const relativeWidth = relativeXEnd - relativeXStart;

    const x = Math.max(0, relativeXStart) / queryXPerPx;
    const y = NODE_HEIGHT * (depth - minDepth);
    const width = depthMatchingZoom
      ? Math.min(relativeWidth, zoomQueryWidth) / queryXPerPx
      : relativeWidth / queryXPerPx;
    const state = computeState(qXStart, qXEnd, zoomRegion, depthMatchingZoom);

    if (width < MIN_PIXEL_DISPLAYED) {
      // Check if parent was merged - if so, use x-position-based key so that
      // children of different parents that were merged together also merge.
      // This enables recursive merging: if parents A and B merged into the
      // same visual node, their children should also merge together.
      const parentMergedX = mergedKeyToX.get(`${parentId}_${depth}`);
      const parentChildMergeKey =
        parentMergedX !== undefined
          ? `x_${Math.round(parentMergedX)}_${depth}`
          : `p_${parentId}_${depth}`;

      const mergedXKey = `${id}_${depth > 0 ? depth + 1 : depth - 1}`;
      const childMergedIdx = keyToChildMergedIdx.get(parentChildMergeKey);
      if (childMergedIdx !== undefined) {
        const r = renderNodes[childMergedIdx];
        const mergedWidth = isDepthMatchingZoom(depth, zoomRegion)
          ? Math.min(qXEnd - r.source.queryXStart, zoomQueryWidth) / queryXPerPx
          : (qXEnd - r.source.queryXStart) / queryXPerPx;
        renderNodes[childMergedIdx] = {
          ...r,
          width: Math.max(mergedWidth, MIN_PIXEL_DISPLAYED),
          source: {
            ...(r.source as MergedSource),
            queryXEnd: qXEnd,
          },
        };
        mergedKeyToX.set(mergedXKey, r.x);
        continue;
      }
      const mergedX = parentMergedX ?? x;
      renderNodes.push({
        x: mergedX,
        y,
        width: Math.max(width, MIN_PIXEL_DISPLAYED),
        source: {
          kind: 'MERGED',
          queryXStart: qXStart,
          queryXEnd: qXEnd,
          type: depth > 0 ? 'BELOW_ROOT' : 'ABOVE_ROOT',
        },
        state,
      });
      keyToChildMergedIdx.set(parentChildMergeKey, renderNodes.length - 1);
      mergedKeyToX.set(mergedXKey, mergedX);
      continue;
    }
    renderNodes.push({
      x,
      y,
      width,
      source: {
        kind: 'NODE',
        queryXStart: qXStart,
        queryXEnd: qXEnd,
        queryIdx: i,
        type: depth > 0 ? 'BELOW_ROOT' : 'ABOVE_ROOT',
      },
      state,
    });
  }
  return renderNodes;
}

function isDepthMatchingZoom(depth: number, zoomRegion: ZoomRegion): boolean {
  assertTrue(
    depth !== 0,
    'Handling zooming root not possible in this function',
  );
  return (
    (depth > 0 && zoomRegion.type === 'BELOW_ROOT') ||
    (depth < 0 && zoomRegion.type === 'ABOVE_ROOT')
  );
}

function computeState(
  qXStart: number,
  qXEnd: number,
  zoomRegion: ZoomRegion,
  isDepthMatchingZoom: boolean,
) {
  if (!isDepthMatchingZoom) {
    return 'NORMAL';
  }
  if (qXStart === zoomRegion.queryXStart && qXEnd === zoomRegion.queryXEnd) {
    return 'SELECTED';
  }
  if (qXStart < zoomRegion.queryXStart || qXEnd > zoomRegion.queryXEnd) {
    return 'PARTIAL';
  }
  return 'NORMAL';
}

function isIntersecting(
  needleX: number | undefined,
  needleY: number | undefined,
  {x, y, width}: RenderNode,
) {
  if (needleX === undefined || needleY === undefined) {
    return false;
  }
  return (
    needleX >= x &&
    needleX < x + width &&
    needleY >= y &&
    needleY < y + NODE_HEIGHT
  );
}

// Unfortunately, widgets *cannot* depend on components so we cannot use the
// colorizer code. Since we need very little of that code anyway, just inline
// what we need here.
const PERCEIVED_BRIGHTNESS_LIMIT = 180;
const WHITE_COLOR = new HSLColor([0, 0, 100]);
const BLACK_COLOR = new HSLColor([0, 0, 0]);
// Lightness 85 ensures even darken(10) stays above brightness threshold for black text
const GRAY_VARIANT_COLOR = new HSLColor([0, 0, 85]);

interface ColorScheme {
  readonly base: Color;
  readonly variant: Color;
  readonly textBase: Color;
  readonly textVariant: Color;
}

function makeColorScheme(base: Color, variant: Color): ColorScheme {
  // Use the same text color for both base and variant to prevent text color
  // switching on hover. The text color is determined by the base color only.
  const textColor =
    base.perceivedBrightness >= PERCEIVED_BRIGHTNESS_LIMIT
      ? BLACK_COLOR
      : WHITE_COLOR;
  return {
    base,
    variant,
    textBase: textColor,
    textVariant: textColor,
  };
}

// Pre-computed color schemes for special cases
const GREYED_COLOR_SCHEME = makeColorScheme(
  GRAY_VARIANT_COLOR,
  GRAY_VARIANT_COLOR.darken(5),
);
const ROOT_COLOR_SCHEME = makeColorScheme(
  GRAY_VARIANT_COLOR.darken(10),
  GRAY_VARIANT_COLOR.darken(15),
);

// Cache for computed color schemes by name
const colorSchemeCache = new Map<string, ColorScheme>();

function getFlamegraphColorScheme(name: string, greyed: boolean): ColorScheme {
  if (greyed) {
    return GREYED_COLOR_SCHEME;
  }
  if (name === 'unknown' || name === 'root') {
    return ROOT_COLOR_SCHEME;
  }

  // Check cache first
  let scheme = colorSchemeCache.get(name);
  if (scheme !== undefined) {
    return scheme;
  }

  // Hash the name to get a predictable hue, then create color with fixed
  // saturation and lightness values to match what pprof web UI does.
  const hue = hash(name, 360);
  const base = new HSLColor({h: hue, s: 46, l: 80});
  scheme = makeColorScheme(base, base.darken(15).saturate(15));
  colorSchemeCache.set(name, scheme);
  return scheme;
}
