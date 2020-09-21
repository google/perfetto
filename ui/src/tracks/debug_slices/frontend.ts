// Copyright (C) 2020 The Android Open Source Project
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

import {Actions} from '../../common/actions';
import {TrackState} from '../../common/state';
import {globals} from '../../frontend/globals';
import {Track} from '../../frontend/track';
import {TrackButton, TrackButtonAttrs} from '../../frontend/track_panel';
import {trackRegistry} from '../../frontend/track_registry';
import {ChromeSliceTrack} from '../chrome_slices/frontend';

import {DEBUG_SLICE_TRACK_KIND} from './common';

export class DebugSliceTrack extends ChromeSliceTrack {
  static readonly kind = DEBUG_SLICE_TRACK_KIND;
  static create(trackState: TrackState): Track {
    return new DebugSliceTrack(trackState);
  }

  getTrackShellButtons(): Array<m.Vnode<TrackButtonAttrs>> {
    const buttons: Array<m.Vnode<TrackButtonAttrs>> = [];
    buttons.push(m(TrackButton, {
      action: () => {
        globals.dispatch(Actions.requestTrackReload({}));
      },
      i: 'refresh',
      tooltip: 'Refresh tracks',
      showButton: true,
    }));
    buttons.push(m(TrackButton, {
      action: () => {
        globals.dispatch(Actions.removeDebugTrack({}));
      },
      i: 'close',
      tooltip: 'Close',
      showButton: true,
    }));
    return buttons;
  }
}

trackRegistry.register(DebugSliceTrack);
