// Copyright (C) 2018 The Android Open Source Project
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

import * as m from 'mithril';

import {QueryResponse} from '../common/queries';

import {Animation} from './animation';
import {DragGestureHandler} from './drag_gesture_handler';
import {globals} from './globals';
import {TimeAxis} from './time_axis';
import {TimeScale} from './time_scale';
import {OVERVIEW_QUERY_ID} from './viewer_page';

interface ProcessSummaryData {
  upid: number;
  name: string;
  loadByTime: {[timeMs: number]: number};
  hue: number;
}

/**
 * Overview timeline with a brush for time-based selections.
 */
export const OverviewTimeline = {
  oninit() {
    this.timeScale = new TimeScale([0, 1], [0, 0]);
    this.hoveredLoad = null;
    this.contentRect =
        {top: 0, left: 0, bottom: 0, right: 0, width: 0, height: 0};

    this.onmousemove = e => {
      if (!this.processesById) return;
      const y = e.clientY - this.contentRect.top;
      const processes = Object.values(this.processesById);
      if (processes.length === 0) return;
      const heightPerProcess = this.contentRect.height / processes.length;
      const index = Math.floor(y / heightPerProcess);
      this.hoveredProcess = processes[index];
      const hoveredMs = this.timeScale.pxToMs(e.layerX);

      const loadTimesMs = Object.keys(this.hoveredProcess.loadByTime)
                              .map(stringTime => Number(stringTime));
      this.hoveredLoad = null;
      for (const loadTimeMs of loadTimesMs) {
        if (Math.abs(loadTimeMs - hoveredMs) <= 100) {
          this.hoveredLoad = {
            timeMs: loadTimeMs,
            load: this.hoveredProcess.loadByTime[loadTimeMs]
          };
        }
      }
    };
  },
  oncreate(vnode) {
    this.contentRect = vnode.dom.getElementsByClassName('timeline-content')[0]
                           .getBoundingClientRect();
    this.timeScale.setLimitsPx(0, this.contentRect.width);

    const context =
        vnode.dom.getElementsByTagName('canvas')[0].getContext('2d');
    if (!context) {
      throw Error('Overview canvas context not found.');
    }
    this.context = context;
  },
  onupdate(vnode) {
    this.contentRect = vnode.dom.getElementsByClassName('timeline-content')[0]
                           .getBoundingClientRect();
    this.timeScale.setLimitsPx(0, this.contentRect.width);
  },
  view({attrs}) {
    this.timeScale.setLimitsMs(
        attrs.maxVisibleWindowMs.start, attrs.maxVisibleWindowMs.end);

    const resp = globals.queryResults.get(OVERVIEW_QUERY_ID) as QueryResponse;

    if (this.context && resp) {
      // Update data
      if (!this.processesById) {
        this.processesById = {};
        const data = resp.rows;
        const timesMs = data.map(row => row.rts as number * 1000);
        const minTimeMs = Math.min(...timesMs);

        for (const processLoad of data) {
          const upid = processLoad.upid as number;
          if (!this.processesById[upid]) {
            this.processesById[upid] = {
              upid,
              name: processLoad.name as string,
              loadByTime: {},
              hue: Math.random() * 360,
            };
          }
          const timeMs = ((processLoad.rts as number) * 1000 - minTimeMs);
          this.processesById[upid].loadByTime[timeMs] =
              Number(processLoad.load);
        }
      }

      // Render canvas
      const processes = Object.values(this.processesById);
      const heightPerProcess = this.contentRect.height / processes.length;
      const roundedHeightPerProcess = Math.round(heightPerProcess);

      for (let i = 0; i < processes.length; i++) {
        const process = processes[i];
        const startY = Math.round(i * heightPerProcess);

        // Add a background behind the hovered process
        this.context.fillStyle =
            process === this.hoveredProcess ? '#eee' : '#fff';
        this.context.fillRect(
            0, startY, this.contentRect.width, roundedHeightPerProcess);

        const loadTimes = Object.keys(process.loadByTime)
                              .map(stringTime => Number(stringTime));
        for (const loadTime of loadTimes) {
          const load = process.loadByTime[loadTime] * 100;
          const startPx = this.timeScale.msToPx(loadTime);
          const endPx = this.timeScale.msToPx(loadTime + 100);
          const lightness = Math.round(Math.max(100 - 2 * load, 30));
          this.context.fillStyle = `hsl(${process.hue}, 40%, ${lightness}%)`;
          this.context.fillRect(
              startPx, startY, endPx - startPx, roundedHeightPerProcess);
        }
      }
      this.context.fill();
    }

    const processes =
        !this.processesById ? [] : Object.values(this.processesById);

    return m(
        '.overview-timeline',
        m(TimeAxis, {
          timeScale: this.timeScale,
          contentOffset: 0,
          visibleWindowMs: attrs.maxVisibleWindowMs,
        }),
        m('.timeline-content',
          {
            onmousemove: this.onmousemove,
            onmouseout: () => {
              this.hoveredProcess = null;
              this.hoveredLoad = null;
            }
          },
          m('.tooltip',
            {
              style: {
                display: this.hoveredLoad === null ? 'none' : 'block',
                left:
                    `${
                       this.hoveredLoad === null ?
                           0 :
                           this.timeScale.msToPx(this.hoveredLoad.timeMs) - 100
                     }px`,
                top: `${
                        this.hoveredProcess === null ?
                            0 :
                            processes.indexOf(this.hoveredProcess) *
                                this.contentRect.height / processes.length
                      }px`,
              }
            },
            m('b', `${this.hoveredProcess ? this.hoveredProcess.name : ''}`),
            m('br'),
            m('span', `${this.hoveredLoad ? this.hoveredLoad.load : 0}%`)),
          m('canvas.visualization', {
            width: this.contentRect.width,
            height: this.contentRect.height,
          }),
          m('.brushes',
            {
              style: {
                width: '100%',
                height: '100%',
                position: 'absolute',
                top: '0',
              }
            },
            m(HorizontalBrushSelection, {
              onBrushedPx: (startPx: number, endPx: number) => {
                attrs.onBrushedMs(
                    this.timeScale.pxToMs(startPx),
                    this.timeScale.pxToMs(endPx));
              },
              selectionPx: {
                start: this.timeScale.msToPx(attrs.visibleWindowMs.start),
                end: this.timeScale.msToPx(attrs.visibleWindowMs.end)
              },
            }))));
  },
} as
    m.Component<
        {
          visibleWindowMs: {start: number, end: number},
          maxVisibleWindowMs: {start: number, end: number},
          onBrushedMs: (start: number, end: number) => void,
        },
        {
          timeScale: TimeScale,
          context: CanvasRenderingContext2D | undefined,
          contentRect: ClientRect,
          processesById: {[upid: number]: ProcessSummaryData},
          hoveredProcess: ProcessSummaryData | null,
          hoveredLoad: {timeMs: number, load: number} | null,
          onmousemove: (e: MouseEvent) => void,
        }>;

const ZOOM_IN_PERCENTAGE_PER_MS = 0.998;
const ZOOM_OUT_PERCENTAGE_PER_MS = 1 / ZOOM_IN_PERCENTAGE_PER_MS;
const WHEEL_ZOOM_DURATION = 200;

/**
 * Interactive horizontal brush for pixel-based selections.
 */
const HorizontalBrushSelection = {
  oninit() {
    this.limitThenBrush = (startPx: number, endPx: number) => {
      startPx = Math.min(Math.max(0, startPx), this.width);
      endPx = Math.min(Math.max(0, endPx), this.width);
      this.onBrushedPx(startPx, endPx);
    };
  },
  oncreate(vnode) {
    const el = vnode.dom as HTMLElement;
    const bcr = el.getBoundingClientRect() as DOMRect;
    this.offsetLeft = bcr.x;
    this.width = bcr.width;

    const startHandle =
        el.getElementsByClassName('brush-handle-start')[0] as HTMLElement;
    const endHandle =
        el.getElementsByClassName('brush-handle-end')[0] as HTMLElement;

    let dragState: 'draggingStartHandle'|'draggingEndHandle'|'notDragging' =
        'notDragging';

    const dragged = (posX: number) => {
      if ((dragState === 'draggingEndHandle' &&
           posX < this.selectionPx.start) ||
          (dragState === 'draggingStartHandle' &&
           posX > this.selectionPx.end)) {
        // Flip start and end if handle has been dragged past the other limit.
        dragState = dragState === 'draggingStartHandle' ? 'draggingEndHandle' :
                                                          'draggingStartHandle';
      }
      if (dragState === 'draggingStartHandle') {
        this.limitThenBrush(posX, this.selectionPx.end);
      } else {
        this.limitThenBrush(this.selectionPx.start, posX);
      }
    };

    new DragGestureHandler(
        startHandle,
        x => dragged(x - this.offsetLeft),
        () => dragState = 'draggingStartHandle',
        () => dragState = 'notDragging');
    new DragGestureHandler(
        endHandle,
        x => dragged(x - this.offsetLeft),
        () => dragState = 'draggingEndHandle',
        () => dragState = 'notDragging');

    new DragGestureHandler(el, x => dragged(x - this.offsetLeft), x => {
      this.selectionPx.start = this.selectionPx.end = x - this.offsetLeft;
      dragState = 'draggingEndHandle';
      this.limitThenBrush(this.selectionPx.start, this.selectionPx.end);
    }, () => dragState = 'notDragging');

    this.onMouseMove = e => {
      this.mousePositionX = e.clientX - this.offsetLeft;
    };

    let zoomingIn = true;
    const zoomAnimation = new Animation((timeSinceLastMs: number) => {
      const percentagePerMs =
          zoomingIn ? ZOOM_IN_PERCENTAGE_PER_MS : ZOOM_OUT_PERCENTAGE_PER_MS;
      const percentage = Math.pow(percentagePerMs, timeSinceLastMs);

      const selectionLength = this.selectionPx.end - this.selectionPx.start;
      const newSelectionLength = selectionLength * percentage;

      // Brush toward the mouse, like zooming.
      const zoomPositionPercentage =
          (this.mousePositionX - this.selectionPx.start) / selectionLength;

      const brushStart =
          this.mousePositionX - zoomPositionPercentage * newSelectionLength;
      const brushEnd = this.mousePositionX +
          (1 - zoomPositionPercentage) * newSelectionLength;

      this.limitThenBrush(brushStart, brushEnd);
    });

    this.onWheel = e => {
      if (Math.abs(e.deltaX) > Math.abs(e.deltaY)) {
        // Horizontal motion: panning.
        this.limitThenBrush(
            this.selectionPx.start + e.deltaX, this.selectionPx.end + e.deltaX);
      } else {
        // Vertical motion: zooming.
        zoomingIn = e.deltaY < 0;
        zoomAnimation.start(WHEEL_ZOOM_DURATION);
      }
    };
  },
  onupdate(vnode) {
    const el = vnode.dom as HTMLElement;
    const bcr = el.getBoundingClientRect() as DOMRect;
    this.offsetLeft = bcr.x;
    this.width = bcr.width;
  },
  view({attrs}) {
    this.onBrushedPx = attrs.onBrushedPx;
    this.selectionPx = attrs.selectionPx;

    return m(
        '.brushes',
        {
          onwheel: this.onWheel,
          onmousemove: this.onMouseMove,
          style: {
            width: '100%',
            height: '100%',
          }
        },
        m('.brush-left.brush-rect', {
          style: {
            'border-right': '1px solid #aaa',
            left: '0',
            width: `${attrs.selectionPx.start}px`,
          }
        }),
        m('.brush-right.brush-rect', {
          style: {
            'border-left': '1px solid #aaa',
            left: `${attrs.selectionPx.end}px`,
            width: `calc(100% - ${attrs.selectionPx.end}px)`,
          }
        }),
        m(BrushHandle, {
          left: attrs.selectionPx.start,
          className: 'brush-handle-start',
        }),
        m(BrushHandle, {
          left: attrs.selectionPx.end,
          className: 'brush-handle-end',
        }));
  }
} as
    m.Component<
        {
          onBrushedPx: (start: number, end: number) => void,
          selectionPx: {start: number, end: number},
        },
        {
          selectionPx: {start: number, end: number},
          onBrushedPx: (start: number, end: number) => void,
          offsetLeft: number,
          onWheel: (e: WheelEvent) => void,
          onMouseMove: (e: MouseEvent) => void,
          mousePositionX: number,
          width: number,
          limitThenBrush: (startPx: number, endPx: number) => void,
        }>;

/**
 * Creates a visual handle with three horizontal bars.
 */
const BrushHandle = {
  view({attrs}) {
    const handleBar = m('.handle-bar', {
      style: {
        height: '5px',
        width: '8px',
        'margin-left': '2px',
        'border-top': '1px solid #888',
      }
    });

    return m(
        `.brush-handle.${attrs.className}`,
        {
          onmousemove: (e: MouseEvent) => e.stopPropagation(),
          style: {
            left: `${attrs.left - 6}px`,
          }
        },
        m('.handle-bars',
          {
            style: {
              position: 'relative',
              top: '9px',
            }
          },
          handleBar,
          handleBar,
          handleBar));
  }
} as m.Component<{
  left: number,
  className: string,
},
                    {}>;