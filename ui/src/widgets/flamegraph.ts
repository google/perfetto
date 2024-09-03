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
import {findRef} from '../base/dom_utils';
import {assertExists, assertTrue} from '../base/logging';
import {Monitor} from '../base/monitor';
import {Button, ButtonBar} from './button';
import {EmptyState} from './empty_state';
import {Popup, PopupPosition} from './popup';
import {scheduleFullRedraw} from './raf';
import {Select} from './select';
import {Spinner} from './spinner';
import {TagInput} from './tag_input';
import {SegmentedButtons} from './segmented_buttons';

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

export interface FlamegraphQueryData {
  readonly nodes: ReadonlyArray<{
    readonly id: number;
    readonly parentId: number;
    readonly depth: number;
    readonly name: string;
    readonly selfValue: number;
    readonly cumulativeValue: number;
    readonly parentCumulativeValue?: number;
    readonly properties: ReadonlyMap<string, string>;
    readonly xStart: number;
    readonly xEnd: number;
  }>;
  readonly unfilteredCumulativeValue: number;
  readonly allRootsCumulativeValue: number;
  readonly minDepth: number;
  readonly maxDepth: number;
}

export interface FlamegraphTopDown {
  readonly kind: 'TOP_DOWN';
}

export interface FlamegraphBottomUp {
  readonly kind: 'BOTTOM_UP';
}

export interface FlamegraphPivot {
  readonly kind: 'PIVOT';
  readonly pivot: string;
}

export type FlamegraphView =
  | FlamegraphTopDown
  | FlamegraphBottomUp
  | FlamegraphPivot;

export interface FlamegraphFilters {
  readonly showStack: ReadonlyArray<string>;
  readonly hideStack: ReadonlyArray<string>;
  readonly showFromFrame: ReadonlyArray<string>;
  readonly hideFrame: ReadonlyArray<string>;
  readonly view: FlamegraphView;
}

export interface FlamegraphAttrs {
  readonly metrics: ReadonlyArray<{
    readonly name: string;
    readonly unit: string;
  }>;
  readonly selectedMetricName: string;
  readonly data: FlamegraphQueryData | undefined;

  readonly onMetricChange: (metricName: string) => void;
  readonly onFiltersChanged: (filters: FlamegraphFilters) => void;
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
 * const selectedMetricName = ...;
 * const filters = ...;
 * const data = ...;
 *
 * m(Flamegraph, {
 *   metrics,
 *   selectedMetricName,
 *   onMetricChange: (metricName) => {
 *     selectedMetricName = metricName;
 *     data = undefined;
 *     fetchData();
 *   },
 *   data,
 *   onFiltersChanged: (showStack, hideStack, hideFrame) => {
 *     updateFilters(showStack, hideStack, hideFrame);
 *     data = undefined;
 *     fetchData();
 *   },
 * });
 * ```
 */
export class Flamegraph implements m.ClassComponent<FlamegraphAttrs> {
  private attrs: FlamegraphAttrs;

  private rawFilterText: string = '';
  private rawFilters: ReadonlyArray<string> = [];
  private filterFocus: boolean = false;
  private switchState: 'TOP_DOWN' | 'BOTTOM_UP' = 'TOP_DOWN';

  private dataChangeMonitor = new Monitor([() => this.attrs.data]);
  private zoomRegion?: ZoomRegion;

  private renderNodesMonitor = new Monitor([
    () => this.attrs.data,
    () => this.canvasWidth,
    () => this.zoomRegion,
  ]);
  private renderNodes?: ReadonlyArray<RenderNode>;

  private tooltipPos?: {
    node: RenderNode;
    x: number;
    state: 'HOVER' | 'CLICK' | 'DECLICK';
  };
  private lastClickedNode?: RenderNode;

  private hoveredX?: number;
  private hoveredY?: number;

  private canvasWidth = 0;
  private labelCharWidth = 0;

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
    return m(
      '.pf-flamegraph',
      this.renderFilterBar(attrs),
      m(
        '.canvas-container[ref=canvas-container]',
        {
          onscroll: () => scheduleFullRedraw(),
        },
        m(
          Popup,
          {
            trigger: m('.popup-anchor', {
              style: {
                left: this.tooltipPos?.x + 'px',
                top: this.tooltipPos?.node.y + 'px',
              },
            }),
            position: PopupPosition.Bottom,
            isOpen:
              this.tooltipPos?.state === 'HOVER' ||
              this.tooltipPos?.state === 'CLICK',
            className: 'pf-flamegraph-tooltip-popup',
            offset: NODE_HEIGHT,
          },
          this.renderTooltip(),
        ),
        m(`canvas[ref=canvas]`, {
          style: `height:${canvasHeight}px; width:100%`,
          onmousemove: ({offsetX, offsetY}: MouseEvent) => {
            scheduleFullRedraw();
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
                this.tooltipPos?.node.y,
                renderNode,
              )
            ) {
              return;
            }
            this.tooltipPos = {
              x: offsetX,
              node: renderNode,
              state: 'HOVER',
            };
          },
          onmouseout: () => {
            this.hoveredX = undefined;
            this.hoveredY = undefined;
            document.body.style.cursor = 'default';
            if (
              this.tooltipPos?.state === 'HOVER' ||
              this.tooltipPos?.state === 'DECLICK'
            ) {
              this.tooltipPos = undefined;
            }
            scheduleFullRedraw();
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
                this.tooltipPos?.node.y,
                renderNode,
              )
            ) {
              this.tooltipPos!.state =
                this.tooltipPos?.state === 'CLICK' ? 'DECLICK' : 'CLICK';
            } else {
              this.tooltipPos = {
                x: offsetX,
                node: renderNode,
                state: 'CLICK',
              };
            }
            scheduleFullRedraw();
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
            scheduleFullRedraw();
          },
        }),
      ),
    );
  }

  oncreate({dom}: m.VnodeDOM<FlamegraphAttrs, this>) {
    this.drawCanvas(dom);
  }

  onupdate({dom}: m.VnodeDOM<FlamegraphAttrs, this>) {
    this.drawCanvas(dom);
  }

  private drawCanvas(dom: Element) {
    // TODO(lalitm): consider migrating to VirtualCanvas to improve performance here.
    const canvasContainer = findRef(dom, 'canvas-container');
    if (canvasContainer === null) {
      return;
    }
    const canvas = findRef(dom, 'canvas');
    if (canvas === null || !(canvas instanceof HTMLCanvasElement)) {
      return;
    }
    const ctx = canvas.getContext('2d');
    if (ctx === null) {
      return;
    }
    canvas.width = canvas.offsetWidth * devicePixelRatio;
    canvas.height = canvas.offsetHeight * devicePixelRatio;
    this.canvasWidth = canvas.offsetWidth;

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
          canvas.offsetWidth,
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

    const containerRect = canvasContainer.getBoundingClientRect();
    const canvasRect = canvas.getBoundingClientRect();

    const yStart = containerRect.top - canvasRect.top;
    const yEnd = containerRect.bottom - canvasRect.top;

    const {allRootsCumulativeValue, unfilteredCumulativeValue, nodes} =
      this.attrs.data;
    const unit = assertExists(this.selectedMetric).unit;

    ctx.clearRect(0, 0, canvas.offsetWidth, canvas.offsetHeight);
    ctx.save();
    ctx.scale(devicePixelRatio, devicePixelRatio);

    ctx.font = LABEL_FONT_STYLE;
    ctx.textBaseline = 'middle';

    ctx.strokeStyle = 'white';
    ctx.lineWidth = 0.5;

    if (this.labelCharWidth === 0) {
      this.labelCharWidth = ctx.measureText('_').width;
    }

    let hoveredNode: RenderNode | undefined = undefined;
    for (let i = 0; i < this.renderNodes.length; i++) {
      const node = this.renderNodes[i];
      const {x, y, width: width, source, state} = node;
      if (y + NODE_HEIGHT <= yStart || y >= yEnd) {
        continue;
      }

      const hover = isIntersecting(this.hoveredX, this.hoveredY, node);
      if (hover) {
        hoveredNode = node;
      }
      let name: string;
      if (source.kind === 'ROOT') {
        const val = displaySize(allRootsCumulativeValue, unit);
        const percent = displayPercentage(
          allRootsCumulativeValue,
          unfilteredCumulativeValue,
        );
        name = `root: ${val} (${percent})`;
        ctx.fillStyle = generateColor('root', state === 'PARTIAL', hover);
      } else if (source.kind === 'MERGED') {
        name = '(merged)';
        ctx.fillStyle = generateColor(name, state === 'PARTIAL', false);
      } else {
        name = nodes[source.queryIdx].name;
        ctx.fillStyle = generateColor(name, state === 'PARTIAL', hover);
      }
      ctx.fillRect(x, y, width - 1, NODE_HEIGHT - 1);

      const widthNoPadding = width - LABEL_PADDING_PX * 2;
      if (widthNoPadding >= LABEL_MIN_WIDTH_FOR_TEXT_PX) {
        ctx.fillStyle = 'black';
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
    if (hoveredNode === undefined) {
      canvas.style.cursor = 'default';
    } else {
      canvas.style.cursor = 'pointer';
    }
    ctx.restore();
  }

  private renderFilterBar(attrs: FlamegraphAttrs) {
    const self = this;
    return m(
      '.filter-bar',
      m(
        Select,
        {
          value: attrs.selectedMetricName,
          onchange: (e: Event) => {
            const el = e.target as HTMLSelectElement;
            attrs.onMetricChange(el.value);
            scheduleFullRedraw();
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
            tags: this.rawFilters,
            value: this.rawFilterText,
            onChange: (value: string) => {
              self.rawFilterText = value;
              scheduleFullRedraw();
            },
            onTagAdd: (tag: string) => {
              self.rawFilters = addFilter(
                self.rawFilters,
                normalizeFilter(tag),
              );
              self.rawFilterText = '';
              self.attrs.onFiltersChanged(
                computeFilters(self.switchState, self.rawFilters),
              );
              scheduleFullRedraw();
            },
            onTagRemove(index: number) {
              const filters = Array.from(self.rawFilters);
              filters.splice(index, 1);
              self.rawFilters = filters;
              self.attrs.onFiltersChanged(
                computeFilters(self.switchState, self.rawFilters),
              );
              scheduleFullRedraw();
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
        selectedOption: this.switchState === 'TOP_DOWN' ? 0 : 1,
        onOptionSelected: (num) => {
          this.switchState = num === 0 ? 'TOP_DOWN' : 'BOTTOM_UP';
          self.attrs.onFiltersChanged(
            computeFilters(self.switchState, self.rawFilters),
          );
          scheduleFullRedraw();
        },
        disabled: hasPivot(this.rawFilters),
      }),
    );
  }

  private renderTooltip() {
    if (this.tooltipPos === undefined) {
      return undefined;
    }
    const {node} = this.tooltipPos;
    if (node.source.kind === 'MERGED') {
      return m(
        'div',
        m('.tooltip-bold-text', '(merged)'),
        m('.tooltip-text', 'Nodes too small to show, please use filters'),
      );
    }
    const {nodes, allRootsCumulativeValue, unfilteredCumulativeValue} =
      assertExists(this.attrs.data);
    const {unit} = assertExists(this.selectedMetric);
    if (node.source.kind === 'ROOT') {
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
        ),
      );
    }
    const {queryIdx} = node.source;
    const {
      name,
      cumulativeValue,
      selfValue,
      parentCumulativeValue,
      properties,
    } = nodes[queryIdx];
    const filterButtonClick = (filter: string) => {
      this.rawFilters = addFilter(this.rawFilters, filter);
      this.attrs.onFiltersChanged(
        computeFilters(this.switchState, this.rawFilters),
      );
      this.tooltipPos = undefined;
      scheduleFullRedraw();
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
      Array.from(properties, ([key, value]) => {
        return m(
          '.tooltip-text-line',
          m('.tooltip-bold-text', key + ':'),
          m('.tooltip-text', value),
        );
      }),
      m(
        ButtonBar,
        {},
        m(Button, {
          label: 'Zoom',
          onclick: () => {
            this.zoomRegion = node.source;
            scheduleFullRedraw();
          },
        }),
        m(Button, {
          label: 'Show Stack',
          onclick: () => {
            filterButtonClick(`Show Stack: ^${name}$`);
          },
        }),
        m(Button, {
          label: 'Hide Stack',
          onclick: () => {
            filterButtonClick(`Hide Stack: ^${name}$`);
          },
        }),
        m(Button, {
          label: 'Hide Frame',
          onclick: () => {
            filterButtonClick(`Hide Frame: ^${name}$`);
          },
        }),
        m(Button, {
          label: 'Show From Frame',
          onclick: () => {
            filterButtonClick(`Show From Frame: ^${name}$`);
          },
        }),
        m(Button, {
          label: 'Pivot',
          onclick: () => {
            filterButtonClick(`Pivot: ^${name}$`);
          },
        }),
      ),
    );
  }

  private get selectedMetric() {
    return this.attrs.metrics.find(
      (x) => x.name === this.attrs.selectedMetricName,
    );
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
  if (unit === '') return totalSize.toLocaleString();
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

function normalizeFilter(filter: string): string {
  const lwr = filter.toLowerCase();
  if (lwr.startsWith('ss: ') || lwr.startsWith('show stack: ')) {
    return 'Show Stack: ' + filter.split(': ', 2)[1];
  } else if (lwr.startsWith('hs: ') || lwr.startsWith('hide stack: ')) {
    return 'Hide Stack: ' + filter.split(': ', 2)[1];
  } else if (lwr.startsWith('sff: ') || lwr.startsWith('show from frame: ')) {
    return 'Show From Frame: ' + filter.split(': ', 2)[1];
  } else if (lwr.startsWith('hf: ') || lwr.startsWith('hide frame: ')) {
    return 'Hide Frame: ' + filter.split(': ', 2)[1];
  } else if (lwr.startsWith('p:') || lwr.startsWith('pivot: ')) {
    return 'Pivot: ' + filter.split(': ', 2)[1];
  }
  return 'Show Stack: ' + filter;
}

function addFilter(filters: ReadonlyArray<string>, filter: string): string[] {
  if (filter.startsWith('Pivot: ')) {
    return [...filters.filter((x) => !x.startsWith('Pivot: ')), filter];
  }
  return [...filters, filter];
}

function computeFilters(
  switchState: 'TOP_DOWN' | 'BOTTOM_UP',
  rawFilters: readonly string[],
): FlamegraphFilters {
  const showStack = rawFilters
    .filter((x) => x.startsWith('Show Stack: '))
    .map((x) => x.split(': ', 2)[1]);
  assertTrue(
    showStack.length < 32,
    'More than 32 show stack filters is not supported',
  );

  const showFromFrame = rawFilters
    .filter((x) => x.startsWith('Show From Frame: '))
    .map((x) => x.split(': ', 2)[1]);
  assertTrue(
    showFromFrame.length < 32,
    'More than 32 show from frame filters is not supported',
  );

  const pivot = rawFilters.filter((x) => x.startsWith('Pivot: '));
  assertTrue(pivot.length <= 1, 'Only one pivot can be active');

  const view: FlamegraphView =
    pivot.length === 0
      ? {kind: switchState}
      : {kind: 'PIVOT', pivot: pivot[0].split(': ', 2)[1]};
  return {
    showStack,
    hideStack: rawFilters
      .filter((x) => x.startsWith('Hide Stack: '))
      .map((x) => x.split(': ', 2)[1]),
    showFromFrame,
    hideFrame: rawFilters
      .filter((x) => x.startsWith('Hide Frame: '))
      .map((x) => x.split(': ', 2)[1]),
    view,
  };
}

function hasPivot(rawFilters: readonly string[]) {
  const pivot = rawFilters.filter((x) => x.startsWith('Pivot: '));
  assertTrue(pivot.length <= 1, 'Only one pivot can be active');
  return pivot.length === 1;
}

function generateColor(name: string, greyed: boolean, hovered: boolean) {
  if (greyed) {
    return `hsl(0deg, 0%, ${hovered ? 85 : 80}%)`;
  }
  if (name === 'unknown' || name === 'root') {
    return `hsl(0deg, 0%, ${hovered ? 78 : 73}%)`;
  }
  let x = 0;
  for (let i = 0; i < name.length; ++i) {
    x += name.charCodeAt(i) % 64;
  }
  return `hsl(${x % 360}deg, 45%, ${hovered ? 78 : 73}%)`;
}
