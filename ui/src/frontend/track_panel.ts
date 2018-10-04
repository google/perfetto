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

import {Actions, DeferredAction} from '../common/actions';
import {TrackState} from '../common/state';

import {globals} from './globals';
import {drawGridLines} from './gridline_helper';
import {Panel, PanelSize} from './panel';
import {Track} from './track';
import {trackRegistry} from './track_registry';

// TODO(hjd): We should remove the constant where possible.
// If any uses can't be removed we should read this constant from CSS.
export const TRACK_SHELL_WIDTH = 300;

function isPinned(id: string) {
  return globals.state.pinnedTracks.indexOf(id) !== -1;
}

interface TrackShellAttrs {
  trackState: TrackState;
}
class TrackShell implements m.ClassComponent<TrackShellAttrs> {
  view({attrs}: m.CVnode<TrackShellAttrs>) {
    return m(
        '.track-shell',
        m('h1', attrs.trackState.name),
        m(TrackButton, {
          action: Actions.moveTrack(
              {trackId: attrs.trackState.id, direction: 'up'}),
          i: 'arrow_upward_alt',
        }),
        m(TrackButton, {
          action: Actions.moveTrack(
              {trackId: attrs.trackState.id, direction: 'down'}),
          i: 'arrow_downward_alt',
        }),
        m(TrackButton, {
          action: Actions.toggleTrackPinned({trackId: attrs.trackState.id}),
          i: isPinned(attrs.trackState.id) ? 'star' : 'star_border',
        }));
  }
}

interface TrackContentAttrs {
  track: Track;
}
class TrackContent implements m.ClassComponent<TrackContentAttrs> {
  view({attrs}: m.CVnode<TrackContentAttrs>) {
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
}

interface TrackComponentAttrs {
  trackState: TrackState;
  track: Track;
}
class TrackComponent implements m.ClassComponent<TrackComponentAttrs> {
  view({attrs}: m.CVnode<TrackComponentAttrs>) {
    return m('.track', [
      m(TrackShell, {trackState: attrs.trackState}),
      m(TrackContent, {track: attrs.track})
    ]);
  }
}

interface TrackButtonAttrs {
  action: DeferredAction;
  i: string;
}
class TrackButton implements m.ClassComponent<TrackButtonAttrs> {
  view({attrs}: m.CVnode<TrackButtonAttrs>) {
    return m(
        'i.material-icons.track-button',
        {
          onclick: () => globals.dispatch(attrs.action),
        },
        attrs.i);
  }
}

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
    ctx.save();
    ctx.translate(TRACK_SHELL_WIDTH, 0);
    drawGridLines(
        ctx,
        globals.frontendLocalState.timeScale,
        globals.frontendLocalState.visibleWindowTime,
        size.height);

    this.track.renderCanvas(ctx);
    ctx.restore();
  }
}
