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

import m from 'mithril';
import {assertExists, assertTrue} from '../base/logging';
import {Monitor} from '../base/monitor';
import {Button, ButtonBar} from './button';
import {EmptyState} from './empty_state';
import {Popup, PopupPosition} from './popup';
import {Select} from './select';
import {Spinner} from './spinner';
import {TagInput} from './tag_input';
import {SegmentedButtons} from './segmented_buttons';
import {z} from 'zod';
import {Rect2D, Size2D} from '../base/geom';
import {VirtualOverlayCanvas} from './virtual_overlay_canvas';
import {MenuItem, MenuItemAttrs, PopupMenu} from './menu';
import {Color, HSLColor} from '../base/color';
import {hash} from '../base/hash';

const LABEL_FONT_STYLE = '12px Roboto';
const NODE_HEIGHT = 20;
const MIN_PIXEL_DISPLAYED = 3;
const FILTER_COMMON_TEXT = `
- "Show Stack: foo" or "SS: foo" or "foo" to show only stacks containing "foo"
- "Hide Stack: foo" or "HS: foo" to hide all stacks containing "foo"
- "Show From Frame: foo" or "SFF: foo" to show frames containing "foo" and all descendants
- "Hide Frame: foo" or "HF: foo" to hide all frames containing "foo"
- "Pivot: foo" or "P: foo" to pivot on frames containing "foo".
Note: Pivot applies after all other filters and only one pivot can be active at a time.
`;
const FILTER_EMPTY_TEXT = `
Available filters:${FILTER_COMMON_TEXT}
`;
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

export interface FlamegraphOptionalAction {
  readonly name: string;
  execute?: (kv: ReadonlyMap<string, string>) => void;
  readonly subActions?: FlamegraphOptionalAction[];
}

export interface FlamegraphOptionalMarker {
  readonly name: string;
  isVisible: (properties: ReadonlyMap<string, string>) => boolean;
}

export type FlamegraphPropertyDefinition = {
  displayName: string;
  value: string;
  isVisible: boolean;
};

export interface FlamegraphQueryData {
  readonly nodes: ReadonlyArray<{
    readonly id: number;
    readonly parentId: number;
    readonly depth: number;
    readonly name: string;
    readonly selfValue: number;
    readonly cumulativeValue: number;
    readonly parentCumulativeValue?: number;
    readonly properties: ReadonlyMap<string, FlamegraphPropertyDefinition>;
    readonly marker?: string;
    readonly xStart: number;
    readonly xEnd: number;
  }>;
  readonly unfilteredCumulativeValue: number;
  readonly allRootsCumulativeValue: number;
  readonly minDepth: number;
  readonly maxDepth: number;
  readonly nodeActions: ReadonlyArray<FlamegraphOptionalAction>;
  readonly rootActions: ReadonlyArray<FlamegraphOptionalAction>;
}

const FLAMEGRAPH_FILTER_SCHEMA = z
  .object({
    kind: z
      .union([
        z.literal('SHOW_STACK').readonly(),
        z.literal('HIDE_STACK').readonly(),
        z.literal('SHOW_FROM_FRAME').readonly(),
        z.literal('HIDE_FRAME').readonly(),
        z.literal('OPTIONS').readonly(),
      ])
      .readonly(),
    filter: z.string().readonly(),
  })
  .readonly();

type FlamegraphFilter = z.infer<typeof FLAMEGRAPH_FILTER_SCHEMA>;

const FLAMEGRAPH_VIEW_SCHEMA = z
  .discriminatedUnion('kind', [
    z.object({kind: z.literal('TOP_DOWN').readonly()}),
    z.object({kind: z.literal('BOTTOM_UP').readonly()}),
    z.object({
      kind: z.literal('PIVOT').readonly(),
      pivot: z.string().readonly(),
    }),
  ])
  .readonly();

export type FlamegraphView = z.infer<typeof FLAMEGRAPH_VIEW_SCHEMA>;

export const FLAMEGRAPH_STATE_SCHEMA = z
  .object({
    selectedMetricName: z.string().readonly(),
    filters: z.array(FLAMEGRAPH_FILTER_SCHEMA),
    view: FLAMEGRAPH_VIEW_SCHEMA,
  })
  .readonly();

export type FlamegraphState = z.infer<typeof FLAMEGRAPH_STATE_SCHEMA>;

interface FlamegraphMetric {
  readonly name: string;
  readonly unit: string;
}

export interface FlamegraphAttrs {
  readonly metrics: ReadonlyArray<FlamegraphMetric>;
  readonly state: FlamegraphState;
  readonly data: FlamegraphQueryData | undefined;

  readonly onStateChange: (filters: FlamegraphState) => void;
}

/*
 * Widget for visualizing "tree-like" data structures using an interactive
 * flamegraph visualization.
 *
 * To use this widget, provide an array of "metrics", which correspond to
 * different properties of the tree to switch between (e.g. object size
 * and object count) and the data which should be displayed.
 *
 * Note that it's valid to pass "undefined" as the data: this will cause a
 * loading container to be shown.
 *
 * Example:
 *
 * ```
 * const metrics = [...];
 * let state = ...;
 * let data = ...;
 *
 * m(Flamegraph, {
 *   metrics,
 *   state,
 *   data,
 *   onStateChange: (newState) => {
 *     state = newState,
 *     data = undefined;
 *     fetchData();
 *   },
 * });
 * ```
 */
export class Flamegraph implements m.ClassComponent<FlamegraphAttrs> {
  private attrs: FlamegraphAttrs;

  private rawFilterText: string = '';
  private filterFocus: boolean = false;

  private dataChangeMonitor = new Monitor([() => this.attrs.data]);
  private zoomRegion?: ZoomRegion;

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

  private hoveredX?: number;
  private hoveredY?: number;

  private canvasWidth = 0;
  private labelCharWidth = 0;
  private canvasRect?: Rect2D;

  constructor({attrs}: m.Vnode<FlamegraphAttrs, {}>) {
    this.attrs = attrs;
  }

  view({attrs}: m.Vnode<FlamegraphAttrs, this>): void | m.Children {
    this.attrs = attrs;
    if (this.dataChangeMonitor.ifStateChanged()) {
      this.zoomRegion = undefined;
      this.lastClickedNode = undefined;
      this.tooltipPos = undefined;
    }
    if (attrs.data === undefined) {
      return m(
        '.pf-flamegraph',
        this.renderFilterBar(attrs),
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
    const hoveredNode = this.renderNodes?.find((n) =>
      isIntersecting(this.hoveredX, this.hoveredY, n),
    );
    return m(
      '.pf-flamegraph',
      this.renderFilterBar(attrs),
      m(
        VirtualOverlayCanvas,
        {
          className: 'pf-virtual-canvas',
          overflowX: 'hidden',
          overflowY: 'auto',
          onCanvasRedraw: ({ctx, virtualCanvasSize, canvasRect}) => {
            this.drawCanvas(ctx, virtualCanvasSize, canvasRect);
          },
        },
        m(
          'div',
          {
            style: {
              height: `${canvasHeight}px`,
              cursor: hoveredNode === undefined ? 'default' : 'pointer',
            },
            onmousemove: ({offsetX, offsetY}: MouseEvent) => {
              this.hoveredX = offsetX;
              this.hoveredY = offsetY;
              if (this.tooltipPos?.state === 'CLICK') {
                return;
              }
              const renderNode = this.renderNodes?.find((n) =>
                isIntersecting(offsetX, offsetY, n),
              );
              if (renderNode === undefined) {
                this.tooltipPos = undefined;
                return;
              }
              if (
                isIntersecting(
                  this.tooltipPos?.x,
                  this.tooltipPos?.y,
                  renderNode,
                )
              ) {
                return;
              }
              this.tooltipPos = {
                x: offsetX,
                y: renderNode.y,
                source: renderNode.source,
                state: 'HOVER',
              };
            },
            onmouseout: () => {
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
              // TODO(lalitm): ignore merged nodes for now as we haven't quite
              // figured out the UX for this.
              if (renderNode?.source.kind === 'MERGED') {
                return;
              }
              this.zoomRegion = renderNode?.source;
            },
          },
          m(
            Popup,
            {
              trigger: m('.popup-anchor', {
                style: {
                  left: this.tooltipPos?.x + 'px',
                  top: this.tooltipPos?.y + 'px',
                },
              }),
              // We have a wide set of buttons that would overflow given the
              // normal width constraints of the popup.
              fitContent: true,
              position: PopupPosition.Right,
              isOpen:
                this.isPopupAnchorVisible() &&
                (this.tooltipPos?.state === 'HOVER' ||
                  this.tooltipPos?.state === 'CLICK'),
              className: 'pf-flamegraph-tooltip-popup',
              offset: NODE_HEIGHT,
            },
            this.renderTooltip(),
          ),
        ),
      ),
    );
  }

  static createDefaultState(
    metrics: ReadonlyArray<FlamegraphMetric>,
  ): FlamegraphState {
    return {
      selectedMetricName: metrics[0].name,
      filters: [],
      view: {kind: 'TOP_DOWN'},
    };
  }

  /**
   * Updates a FlamegraphState with new metrics, preserving filters where possible.
   *
   * If the current state has no metric selected (empty string), this will
   * initialize it with the first metric. Otherwise, it preserves the selected
   * metric if it still exists in the new metrics array, or falls back to the
   * first metric if it doesn't.
   */
  static updateState(
    state: FlamegraphState | undefined,
    metrics: ReadonlyArray<FlamegraphMetric>,
  ): FlamegraphState {
    if (state === undefined) {
      return Flamegraph.createDefaultState(metrics);
    }
    const metricStillExists = metrics.some(
      (m) => m.name === state.selectedMetricName,
    );
    return {
      filters: state.filters,
      view: state.view,
      selectedMetricName: metricStillExists
        ? state.selectedMetricName
        : metrics[0].name,
    };
  }

  private drawCanvas(
    ctx: CanvasRenderingContext2D,
    size: Size2D,
    rect: Rect2D,
  ) {
    this.canvasRect = rect;
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
    }
    if (this.attrs.data === undefined || this.renderNodes === undefined) {
      return;
    }

    const yStart = rect.top;
    const yEnd = rect.bottom;

    const {allRootsCumulativeValue, unfilteredCumulativeValue, nodes} =
      this.attrs.data;
    const unit = assertExists(this.selectedMetric).unit;

    ctx.font = LABEL_FONT_STYLE;
    ctx.textBaseline = 'middle';

    ctx.strokeStyle = 'white';
    ctx.lineWidth = 0.5;

    if (this.labelCharWidth === 0) {
      this.labelCharWidth = ctx.measureText('_').width;
    }

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
    if (!this.tooltipPos || !this.canvasRect) {
      return false;
    }
    const {y} = this.tooltipPos;
    return y >= this.canvasRect.top && y <= this.canvasRect.bottom;
  }

  private renderFilterBar(attrs: FlamegraphAttrs) {
    const self = this;
    return m(
      '.filter-bar',
      m(
        Select,
        {
          value: attrs.state.selectedMetricName,
          onchange: (e: Event) => {
            const el = e.target as HTMLSelectElement;
            attrs.onStateChange({
              ...self.attrs.state,
              selectedMetricName: el.value,
            });
          },
        },
        attrs.metrics.map((x) => {
          return m('option', {value: x.name}, x.name);
        }),
      ),
      m(
        Popup,
        {
          trigger: m(TagInput, {
            tags: toTags(self.attrs.state),
            value: this.rawFilterText,
            onChange: (value: string) => {
              self.rawFilterText = value;
            },
            onTagAdd: (tag: string) => {
              self.rawFilterText = '';
              self.attrs.onStateChange(updateState(self.attrs.state, tag));
            },
            onTagRemove(index: number) {
              if (index === self.attrs.state.filters.length) {
                self.attrs.onStateChange({
                  ...self.attrs.state,
                  view: {kind: 'TOP_DOWN'},
                });
              } else {
                const filters = Array.from(self.attrs.state.filters);
                filters.splice(index, 1);
                self.attrs.onStateChange({
                  ...self.attrs.state,
                  filters,
                });
              }
            },
            onfocus() {
              self.filterFocus = true;
            },
            onblur() {
              self.filterFocus = false;
            },
            placeholder: 'Add filter...',
          }),
          isOpen: self.filterFocus && this.rawFilterText.length === 0,
          position: PopupPosition.Bottom,
        },
        m('.pf-flamegraph-filter-bar-popup-content', FILTER_EMPTY_TEXT.trim()),
      ),
      m(SegmentedButtons, {
        options: [{label: 'Top Down'}, {label: 'Bottom Up'}],
        selectedOption: this.attrs.state.view.kind === 'TOP_DOWN' ? 0 : 1,
        onOptionSelected: (num) => {
          self.attrs.onStateChange({
            ...this.attrs.state,
            view: {kind: num === 0 ? 'TOP_DOWN' : 'BOTTOM_UP'},
          });
        },
        disabled: this.attrs.state.view.kind === 'PIVOT',
      }),
    );
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
    } = assertExists(this.attrs.data);
    const {unit} = assertExists(this.selectedMetric);
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
    const filterButtonClick = (state: FlamegraphState) => {
      this.attrs.onStateChange(state);
      this.tooltipPos = undefined;
    };

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
    return m(
      'div',
      // Show marker at the top of the tooltip
      marker &&
        m('.tooltip-text-line', m('.tooltip-marker-text', `■ ${marker}`)),
      m('.tooltip-bold-text', name),
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
      m(
        ButtonBar,
        {},
        m(Button, {
          label: 'Zoom',
          onclick: () => {
            this.zoomRegion = source;
          },
        }),
        m(Button, {
          label: 'Show Stack',
          onclick: () => {
            filterButtonClick(
              addFilter(this.attrs.state, {
                kind: 'SHOW_STACK',
                filter: `^${name}$`,
              }),
            );
          },
        }),
        m(Button, {
          label: 'Hide Stack',
          onclick: () => {
            filterButtonClick(
              addFilter(this.attrs.state, {
                kind: 'HIDE_STACK',
                filter: `^${name}$`,
              }),
            );
          },
        }),
        m(Button, {
          label: 'Hide Frame',
          onclick: () => {
            filterButtonClick(
              addFilter(this.attrs.state, {
                kind: 'HIDE_FRAME',
                filter: `^${name}$`,
              }),
            );
          },
        }),
        m(Button, {
          label: 'Show From Frame',
          onclick: () => {
            filterButtonClick(
              addFilter(this.attrs.state, {
                kind: 'SHOW_FROM_FRAME',
                filter: `^${name}$`,
              }),
            );
          },
        }),
        m(Button, {
          label: 'Pivot',
          onclick: () => {
            filterButtonClick({
              ...this.attrs.state,
              view: {kind: 'PIVOT', pivot: `^${name}$`},
            });
          },
        }),
        this.renderActionsMenu(nodeActions, properties),
      ),
    );
  }

  private get selectedMetric() {
    return this.attrs.metrics.find(
      (x) => x.name === this.attrs.state.selectedMetricName,
    );
  }

  private renderActionsMenu(
    actions: ReadonlyArray<FlamegraphOptionalAction>,
    properties: ReadonlyMap<string, FlamegraphPropertyDefinition>,
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
    action: FlamegraphOptionalAction,
    properties: ReadonlyMap<string, FlamegraphPropertyDefinition>,
  ): m.Vnode<MenuItemAttrs> {
    if (action.subActions !== undefined && action.subActions.length > 0) {
      return this.renderParentMenuItem(action, action.subActions, properties);
    } else if (action.execute) {
      return this.renderExecutableMenuItem(action, properties);
    } else {
      return this.renderDisabledMenuItem(action);
    }
  }

  private renderParentMenuItem(
    action: FlamegraphOptionalAction,
    subActions: FlamegraphOptionalAction[],
    properties: ReadonlyMap<string, FlamegraphPropertyDefinition>,
  ): m.Vnode<MenuItemAttrs> {
    return m(
      MenuItem,
      {
        label: action.name,
        // No onclick handler for parent menu items
      },
      // Directly render sub-actions as children of the MenuItem
      subActions.map((subAction) => this.renderMenuItem(subAction, properties)),
    );
  }

  private renderExecutableMenuItem(
    action: FlamegraphOptionalAction,
    properties: ReadonlyMap<string, FlamegraphPropertyDefinition>,
  ): m.Vnode<MenuItemAttrs> {
    return m(MenuItem, {
      label: action.name,
      onclick: () => {
        const reducedProperties = this.createReducedProperties(properties);
        action.execute!(reducedProperties);
        this.tooltipPos = undefined; // Close tooltip after action
      },
    });
  }

  private renderDisabledMenuItem(
    action: FlamegraphOptionalAction,
  ): m.Vnode<MenuItemAttrs> {
    return m(MenuItem, {
      label: action.name,
      disabled: true,
    });
  }

  private createReducedProperties(
    properties: ReadonlyMap<string, FlamegraphPropertyDefinition>,
  ): ReadonlyMap<string, string> {
    return new Map([...properties].map(([key, {value}]) => [key, value]));
  }
}

function computeRenderNodes(
  {nodes, allRootsCumulativeValue, minDepth}: FlamegraphQueryData,
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
      const parentChildMergeKey = `${parentId}_${depth}`;
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
      const mergedX = mergedKeyToX.get(`${parentId}_${depth}`) ?? x;
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

function displaySize(totalSize: number, unit: string): string {
  if (unit === '' || unit === 'count') return totalSize.toLocaleString();
  if (totalSize === 0) return `0 ${unit}`;
  let step: number;
  let units: string[];
  switch (unit) {
    case 'B':
      step = 1024;
      units = ['B', 'KiB', 'MiB', 'GiB'];
      break;
    case 'ns':
      step = 1000;
      units = ['ns', 'us', 'ms', 's'];
      break;
    default:
      step = 1000;
      units = [unit, `K${unit}`, `M${unit}`, `G${unit}`];
      break;
  }
  const unitsIndex = Math.min(
    Math.trunc(Math.log(totalSize) / Math.log(step)),
    units.length - 1,
  );
  const pow = Math.pow(step, unitsIndex);
  const result = totalSize / pow;
  const resultString =
    totalSize % pow === 0 ? result.toString() : result.toFixed(2);
  return `${resultString} ${units[unitsIndex]}`;
}

function displayPercentage(size: number, totalSize: number): string {
  if (totalSize === 0) {
    return `[NULL]%`;
  }
  return `${((size / totalSize) * 100.0).toFixed(2)}%`;
}

function updateState(state: FlamegraphState, filter: string): FlamegraphState {
  const lwr = filter.toLowerCase();
  const splitFilterFn = (f: string) => f.substring(f.indexOf(':') + 1).trim();
  if (lwr.startsWith('ss:') || lwr.startsWith('show stack:')) {
    return addFilter(state, {
      kind: 'SHOW_STACK',
      filter: splitFilterFn(filter),
    });
  } else if (lwr.startsWith('hs:') || lwr.startsWith('hide stack:')) {
    return addFilter(state, {
      kind: 'HIDE_STACK',
      filter: splitFilterFn(filter),
    });
  } else if (lwr.startsWith('sff:') || lwr.startsWith('show from frame:')) {
    return addFilter(state, {
      kind: 'SHOW_FROM_FRAME',
      filter: splitFilterFn(filter),
    });
  } else if (lwr.startsWith('hf:') || lwr.startsWith('hide frame:')) {
    return addFilter(state, {
      kind: 'HIDE_FRAME',
      filter: splitFilterFn(filter),
    });
  } else if (lwr.startsWith('p:') || lwr.startsWith('pivot:')) {
    return {
      ...state,
      view: {kind: 'PIVOT', pivot: splitFilterFn(filter)},
    };
  }
  return addFilter(state, {
    kind: 'SHOW_STACK',
    filter: filter.trim(),
  });
}

function toTags(state: FlamegraphState): ReadonlyArray<string> {
  const toString = (x: FlamegraphFilter) => {
    switch (x.kind) {
      case 'HIDE_FRAME':
        return 'Hide Frame: ' + x.filter;
      case 'HIDE_STACK':
        return 'Hide Stack: ' + x.filter;
      case 'SHOW_FROM_FRAME':
        return 'Show From Frame: ' + x.filter;
      case 'SHOW_STACK':
        return 'Show Stack: ' + x.filter;
      case 'OPTIONS':
        return 'Options';
    }
  };
  const filters = state.filters.map((x) => toString(x));
  return filters.concat(
    state.view.kind === 'PIVOT' ? ['Pivot: ' + state.view.pivot] : [],
  );
}

function addFilter(
  state: FlamegraphState,
  filter: FlamegraphFilter,
): FlamegraphState {
  return {
    ...state,
    filters: state.filters.concat([filter]),
  };
}

// Unfortunately, widgets *cannot* depend on components so we cannot use the
// colorizer code. Since we need very little of that code anyway, just inline
// what we need here.
const PERCEIVED_BRIGHTNESS_LIMIT = 180;
const WHITE_COLOR = new HSLColor([0, 0, 100]);
const BLACK_COLOR = new HSLColor([0, 0, 0]);
const GRAY_VARIANT_COLOR = new HSLColor([0, 0, 62]);

function makeColorScheme(base: Color, variant: Color) {
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

function getFlamegraphColorScheme(name: string, greyed: boolean) {
  if (greyed) {
    return makeColorScheme(GRAY_VARIANT_COLOR, GRAY_VARIANT_COLOR.darken(5));
  }
  if (name === 'unknown' || name === 'root') {
    return makeColorScheme(
      GRAY_VARIANT_COLOR.darken(10),
      GRAY_VARIANT_COLOR.darken(15),
    );
  }
  // Hash the name to get a predictable hue, then create color with fixed
  // saturation and lightness values to match what pprof web UI does.
  const hue = hash(name, 360);
  const base = new HSLColor({h: hue, s: 46, l: 80});
  return makeColorScheme(base, base.darken(15).saturate(15));
}
