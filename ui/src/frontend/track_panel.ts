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

import {moveTrack} from '../common/actions';
import {TrackState} from '../common/state';

import {globals} from './globals';
import {drawGridLines} from './gridline_helper';
import {Panel, PanelSize} from './panel';
import {Track} from './track';
import {trackRegistry} from './track_registry';

// TODO(hjd): We should remove the constant where possible.
// If any uses can't be removed we should read this constant from CSS.
export const TRACK_SHELL_WIDTH = 300;

const TrackShell = {
  view({attrs}) {
    return m(
        '.track-shell',
        m('h1', attrs.trackState.name),
        m(TrackMoveButton, {
          direction: 'up',
          trackId: attrs.trackState.id,
        }),
        m(TrackMoveButton, {
          direction: 'down',
          trackId: attrs.trackState.id,
        }));
  },
} as m.Component<{trackState: TrackState}>;

const TrackContent = {
  view({attrs}) {
    return m('.track-content', {
      onmousemove: (e: MouseEvent) => {
        attrs.track.onMouseMove({x: e.layerX, y: e.layerY});
        globals.rafScheduler.scheduleRedraw();
      },
      onmouseout: () => {
        attrs.track.onMouseOut();
        globals.rafScheduler.scheduleRedraw();
      },
    }, );
  }
} as m.Component<{track: Track}>;

const TrackComponent = {
  view({attrs}) {
    return m('.track', [
      m(TrackShell, {trackState: attrs.trackState}),
      m(TrackContent, {track: attrs.track})
    ]);
  }
} as m.Component<{trackState: TrackState, track: Track}>;

const TrackMoveButton = {
  view({attrs}) {
    return m(
        'i.material-icons.track-move-icons',
        {
          onclick: () =>
              globals.dispatch(moveTrack(attrs.trackId, attrs.direction)),
        },
        attrs.direction === 'up' ? 'arrow_upward_alt' : 'arrow_downward_alt');
  }
} as m.Component<{
  direction: 'up' | 'down',
  trackId: string,
},
                        {}>;

interface TrackPanelAttrs {
  id: string;
}

export class TrackPanel extends Panel<TrackPanelAttrs> {
  private track: Track;
  private trackState: TrackState;
  constructor(vnode: m.CVnode<TrackPanelAttrs>) {
    super();
    this.trackState = globals.state.tracks[vnode.attrs.id];
    const trackCreator = trackRegistry.get(this.trackState.kind);
    this.track = trackCreator.create(this.trackState);
  }

  view() {
    return m(
        '.track',
        {
          style: {
            height: `${this.track.getHeight()}px`,
          }
        },
        [
          m(TrackShell, {trackState: this.trackState}),
          m(TrackContent, {track: this.track})
        ]);
    return m(TrackComponent, {trackState: this.trackState, track: this.track});
  }

  renderCanvas(ctx: CanvasRenderingContext2D, size: PanelSize) {
    ctx.translate(TRACK_SHELL_WIDTH, 0);
    drawGridLines(
        ctx,
        globals.frontendLocalState.timeScale,
        globals.frontendLocalState.visibleWindowTime,
        size.height);

    this.track.renderCanvas(ctx);
  }
}
