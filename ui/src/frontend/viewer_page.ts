
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

import {OverviewTimeline} from './overview_timeline';
import {createPage} from './pages';
import {PanAndZoomHandler} from './pan_and_zoom_handler';
import {ScrollingTrackDisplay} from './scrolling_track_display';
import {TimeAxis} from './time_axis';
import {TimeScale} from './time_scale';

/**
 * Top-most level component for the viewer page. Holds tracks, brush timeline,
 * panels, and everything else that's part of the main trace viewer page.
 */
const TraceViewer = {
  oninit() {
    this.width = 0;
    this.contentOffsetX = 200;
    this.visibleWindowMs = {start: 1000000, end: 2000000};
    this.maxVisibleWindowMs = {start: 0, end: 10000000};
    this.timeScale = new TimeScale(
        [this.visibleWindowMs.start, this.visibleWindowMs.end],
        [0, this.width - this.contentOffsetX]);
  },
  oncreate(vnode) {
    this.onResize = () => {
      const rect = vnode.dom.getBoundingClientRect();
      this.width = rect.width;
      this.timeScale.setLimitsPx(0, this.width - this.contentOffsetX);
      m.redraw();
    };

    // Have to redraw after initialization to provide dimensions to view().
    setTimeout(() => this.onResize());

    // Once ResizeObservers are out, we can stop accessing the window here.
    window.addEventListener('resize', this.onResize);

    const panZoomEl =
        vnode.dom.getElementsByClassName('page-content')[0] as HTMLElement;

    // TODO: ContentOffsetX should be defined somewhere central.
    // Currently it lives here, in canvas wrapper, and in track shell.
    this.zoomContent = new PanAndZoomHandler({
      element: panZoomEl,
      contentOffsetX: this.contentOffsetX,
      onPanned: (pannedPx: number) => {
        const deltaMs = this.timeScale.deltaPxToDurationMs(pannedPx);
        this.visibleWindowMs.start += deltaMs;
        this.visibleWindowMs.end += deltaMs;
        this.timeScale.setLimitsMs(
            this.visibleWindowMs.start, this.visibleWindowMs.end);
        m.redraw();
      },
      onZoomed: (zoomedPositionPx: number, zoomPercentage: number) => {
        const totalTimespanMs =
            this.visibleWindowMs.end - this.visibleWindowMs.start;
        const newTotalTimespanMs = totalTimespanMs * zoomPercentage;

        const zoomedPositionMs =
            this.timeScale.pxToMs(zoomedPositionPx) as number;
        const positionPercentage =
            (zoomedPositionMs - this.visibleWindowMs.start) / totalTimespanMs;

        this.visibleWindowMs.start =
            zoomedPositionMs - newTotalTimespanMs * positionPercentage;
        this.visibleWindowMs.end =
            zoomedPositionMs + newTotalTimespanMs * (1 - positionPercentage);
        this.timeScale.setLimitsMs(
            this.visibleWindowMs.start, this.visibleWindowMs.end);
        m.redraw();
      }
    });
  },
  onremove() {
    window.removeEventListener('resize', this.onResize);
    this.zoomContent.shutdown();
  },
  view() {
    const onBrushedMs = (start: number, end: number) => {
      this.visibleWindowMs.start = start;
      this.visibleWindowMs.end = end;
      this.timeScale.setLimitsMs(
          this.visibleWindowMs.start, this.visibleWindowMs.end);
      m.redraw();
    };

    return m(
        '#page',
        {
          style: {
            width: '100%',
            height: '100%',
          },
        },
        m(OverviewTimeline, {
          visibleWindowMs: this.visibleWindowMs,
          maxVisibleWindowMs: this.maxVisibleWindowMs,
          width: this.width,
          onBrushedMs
        }),
        m('.page-content',
          {
            style: {
              width: '100%',
              height: '100%',
            }
          },
          m(TimeAxis, {
            timeScale: this.timeScale,
            contentOffset: 200,
            visibleWindowMs: this.visibleWindowMs,
            width: this.width,
          }),
          m(ScrollingTrackDisplay, {
            timeScale: this.timeScale,
            visibleWindowMs: this.visibleWindowMs,
          })));
  },
} as m.Component<{}, {
  visibleWindowMs: {start: number, end: number},
  maxVisibleWindowMs: {start: number, end: number},
  onResize: () => void,
  timeScale: TimeScale,
  width: number,
  zoomContent: PanAndZoomHandler,
  contentOffsetX: number,
}>;

export const ViewerPage = createPage({
  view() {
    return m(TraceViewer);
  }
});
