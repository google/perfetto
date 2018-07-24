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

import {TrackState} from '../common/state';

import {Milliseconds, TimeScale} from './time_scale';
import {TrackImpl} from './track_impl';
import {trackRegistry} from './track_registry';
import {TrackShell} from './track_shell';
import {VirtualCanvasContext} from './virtual_canvas_context';

export const Track = {
  oninit({attrs}) {
    // TODO: Since ES6 modules are asynchronous and it is conceivable that we
    // want to load a track implementation on demand, we should not rely here on
    // the fact that the track is already registered. We should show some
    // default content until a track implementation is found.
    const trackCreator = trackRegistry.getCreator(attrs.trackState.type);
    this.trackImpl = trackCreator.create(attrs.trackState);
  },

  view({attrs}) {
    const sliceStart: Milliseconds = 100000;
    const sliceEnd: Milliseconds = 400000;

    const rectStart = attrs.timeScale.msToPx(sliceStart);
    const rectWidth = attrs.timeScale.msToPx(sliceEnd) - rectStart;

    return m(
        '.track',
        {
          style: {
            position: 'absolute',
            top: attrs.top.toString() + 'px',
            left: 0,
            width: '100%',
            height: `${attrs.trackState.height}px`,
          }
        },
        m(TrackShell,
          {name: attrs.trackState.name},
          // TODO(dproy): Move out DOM Content from the track class.
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
            attrs.trackState.name + ' DOM Content')));
  },

  onupdate({attrs}) {
    // TODO(dproy): Figure out how track implementations should render DOM.
    if (attrs.trackContext.isOnCanvas()) {
      this.trackImpl.draw(attrs.trackContext, attrs.width, attrs.timeScale);
    }
  }
} as m.Component<{
  trackContext: VirtualCanvasContext,
  top: number,
  width: number,
  timeScale: TimeScale,
  trackState: TrackState,
},
                     // TODO(dproy): Fix formatter. This is ridiculous.
                     {trackImpl: TrackImpl}>;
