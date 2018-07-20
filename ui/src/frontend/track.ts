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

import {GridlineHelper} from './gridline_helper';
import {Milliseconds, TimeScale} from './time_scale';
import {TrackShell} from './track_shell';
import {VirtualCanvasContext} from './virtual_canvas_context';

export const Track = {
  view({attrs}) {
    const sliceStart: Milliseconds = 100000;
    const sliceEnd: Milliseconds = 400000;

    const rectStart = attrs.timeScale.msToPx(sliceStart);
    const rectWidth = attrs.timeScale.msToPx(sliceEnd) - rectStart;
    const shownStart = rectStart > attrs.width ? attrs.width : rectStart;
    const shownWidth = rectWidth + (rectStart as number) > attrs.width ?
        attrs.width :
        rectWidth;

    if (attrs.trackContext.isOnCanvas()) {
      attrs.trackContext.fillStyle = '#ccc';
      attrs.trackContext.fillRect(0, 0, attrs.width, 73);

      GridlineHelper.drawGridLines(
          attrs.trackContext, attrs.timeScale, [0, 1000000], attrs.width, 73);

      attrs.trackContext.fillStyle = '#c00';
      attrs.trackContext.fillRect(shownStart, 40, shownWidth, 30);

      attrs.trackContext.font = '16px Arial';
      attrs.trackContext.fillStyle = '#000';
      attrs.trackContext.fillText(
          attrs.name + ' rendered by canvas', shownStart, 60);
    }

    return m(
        '.track',
        {
          style: {
            position: 'absolute',
            top: attrs.top.toString() + 'px',
            left: 0,
            width: '100%'
          }
        },
        m(TrackShell,
          attrs,
          m('.marker',
            {
              style: {
                'font-size': '1.5em',
                position: 'absolute',
                left: rectStart.toString() + 'px',
                width: rectWidth.toString() + 'px',
                background: '#aca'
              }
            },
            attrs.name + ' DOM Content')));
  }
} as m.Component<{
  name: string,
  trackContext: VirtualCanvasContext,
  top: number,
  width: number,
  timeScale: TimeScale
}>;