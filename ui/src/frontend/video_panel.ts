// Copyright (C) 2019 The Android Open Source Project
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

import {globals} from './globals';
import {Actions} from '../common/actions';
import {randomColor} from './colorizer';

// TODO: kodiobika - Capture onpause event within VideoPanel and handle sync
// by creating a new animation rather than via raf_scheduler

export function syncVideo() {
    const ts = globals.frontendLocalState.hoveredTimestamp -
               globals.state.traceTime.startSec;
    const elem = document.getElementById('video_pane') as HTMLVideoElement;
    if (elem != null) {
        elem.currentTime = ts;
        elem.onpause = _event => {
          if (globals.state.flagPauseEnabled && !(elem.ended)) {
            globals.dispatch(Actions.updateOnPauseTime({ts: elem.currentTime}));
            const timestamp =
                elem.currentTime + globals.state.traceTime.startSec;
            const color = randomColor();
            globals.dispatch(
                Actions.addNote({timestamp, color, isMovie: true}));
          }
        };
        elem.currentTime = globals.state.onPauseTime;
    }
}

export class VideoPanel implements m.Component {
    view() {
        const ts = globals.frontendLocalState.hoveredTimestamp -
                   globals.state.traceTime.startSec;
        const vid = m('video#video_pane', {
                controls: true,
                width: 320,
                currentTime: ts
            },
            m('source', { src: globals.state.video, type: 'video/mp4' }));
        return vid;
    }
}
