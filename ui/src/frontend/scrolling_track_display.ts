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

import {CanvasController} from './canvas_controller';
import {CanvasWrapper} from './canvas_wrapper';
import {globals} from './globals';
import {ScrollableContainer} from './scrollable_container';
import {TimeScale} from './time_scale';
import {TrackComponent} from './track_component';

/**
 * The primary component responsible for showing all the tracks.
 */
export const ScrollingTrackDisplay = {
  oninit() {
    this.width = 0;
    this.height = 0;
    this.canvasController = new CanvasController();
  },
  oncreate(vnode) {
    // TODO: Consider moving this to top level TraceViewer.
    this.onResize = () => {
      const rect = vnode.dom.getBoundingClientRect();
      this.width = rect.width;
      this.height = rect.height;
      this.canvasController.setDimensions(this.width, this.height);
      m.redraw();
    };
    // Have to redraw after initialization to provide dimensions to view().
    setTimeout(() => this.onResize());

    // Once ResizeObservers are out, we can stop accessing the window here.
    window.addEventListener('resize', this.onResize);
  },
  onremove() {
    window.removeEventListener('resize', this.onResize);
  },
  view({attrs}) {
    const canvasTopOffset = this.canvasController.getCanvasYStart();

    this.canvasController.clear();

    let trackYOffset = 0;
    const childTracks: m.Children[] = [];

    for (const id of globals.state.displayedTrackIds) {
      const trackState = globals.state.tracks[id];
      childTracks.push(m(TrackComponent, {
        canvasController: this.canvasController,
        top: trackYOffset,
        width: this.width,
        timeScale: attrs.timeScale,
        trackState,
        visibleWindowMs: attrs.visibleWindowMs
      }));
      trackYOffset += trackState.height;
    }

    return m(
        '.scrolling-track-display',
        {
          style: {
            position: 'relative',
            width: '100%',
            height: 'calc(100% - 66px)',
            overflow: 'hidden'
          }
        },
        m(ScrollableContainer,
          {
            width: this.width,
            height: this.height,
            contentHeight: 1000,
            onPassiveScroll: (scrollTop: number) => {
              this.canvasController.updateScrollOffset(scrollTop);
              m.redraw();
            },
          },
          m(CanvasWrapper, {
            topOffset: canvasTopOffset,
            canvasElement: this.canvasController.getCanvasElement()
          }),
          ...childTracks));
  },
} as m.Component<{
  timeScale: TimeScale,
  visibleWindowMs: {start: number, end: number},
},
                                     {
                                       canvasController: CanvasController,
                                       width: number,
                                       height: number,
                                       onResize: () => void,
                                     }>;
