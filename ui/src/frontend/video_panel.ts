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

export class VideoPanel implements m.Component {
    view() {
      const offset = globals.state.traceTime.startSec;
      const ts = globals.frontendLocalState.vidTimestamp - offset;
      const vid = m('video#video_pane', {
        controls: true,
        width: 320,
        currentTime: ts,
        onpause: (e: Event) => {
          const elem = e.target as HTMLVideoElement;
          if (globals.state.flagPauseEnabled && !(elem.ended)) {
            const timestamp = elem.currentTime + offset;
            const color = randomColor();
            const isMovie = true;
            globals.dispatch(Actions.addNote({timestamp, color, isMovie}));
            elem.currentTime = timestamp - offset;
          }
        },
      },
      m('source', { src: globals.state.video, type: 'video/mp4' }));
      return vid;
    }
}
