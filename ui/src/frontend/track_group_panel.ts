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

import {assertExists} from '../base/logging';
import {Actions} from '../common/actions';
import {TrackGroupState, TrackState} from '../common/state';

import {globals} from './globals';
import {drawGridLines} from './gridline_helper';
import {Panel, PanelSize} from './panel';
import {Track} from './track';
import {trackRegistry} from './track_registry';


interface Attrs {
  trackGroupId: string;
}

export class TrackGroupPanel extends Panel<Attrs> {
  private readonly trackGroupId: string;
  private shellWidth = 0;
  private backgroundColor = '#ffffff';  // Updated from CSS later.
  private summaryTrack: Track;

  constructor({attrs}: m.CVnode<Attrs>) {
    super();
    this.trackGroupId = attrs.trackGroupId;
    const trackCreator = trackRegistry.get(this.summaryTrackState.kind);
    this.summaryTrack = trackCreator.create(this.summaryTrackState);
  }

  get trackGroupState(): TrackGroupState {
    return assertExists(globals.state.trackGroups[this.trackGroupId]);
  }

  get summaryTrackState(): TrackState {
    return assertExists(
        globals.state.tracks[this.trackGroupState.summaryTrackId]);
  }

  view({attrs}: m.CVnode<Attrs>) {
    const collapsed = this.trackGroupState.collapsed;
    return m(
        `.track-group-panel[collapsed=${collapsed}]`,
        m('.shell',
          m('h1', `${StripPathFromExecutable(this.trackGroupState.name)}`),
          m('.fold-button',
            {
              onclick: () =>
                  globals.dispatch(Actions.toggleTrackGroupCollapsed({
                    trackGroupId: attrs.trackGroupId,
                  })),
            },
            m('i.material-icons',
              this.trackGroupState.collapsed ? 'expand_more' :
                                               'expand_less'))));
  }

  oncreate(vnode: m.CVnodeDOM<Attrs>) {
    this.onupdate(vnode);
  }

  onupdate({dom}: m.CVnodeDOM<Attrs>) {
    const shell = assertExists(dom.querySelector('.shell'));
    this.shellWidth = shell.getBoundingClientRect().width;
    this.backgroundColor =
        getComputedStyle(dom).getPropertyValue('--collapsed-background');
  }

  renderCanvas(ctx: CanvasRenderingContext2D, size: PanelSize) {
    const collapsed = this.trackGroupState.collapsed;
    if (!collapsed) return;

    ctx.save();
    ctx.translate(this.shellWidth, 0);

    ctx.fillStyle = this.backgroundColor;
    ctx.fillRect(0, 0, size.width, size.height);

    drawGridLines(
        ctx,
        globals.frontendLocalState.timeScale,
        globals.frontendLocalState.visibleWindowTime,
        size.height);

    // Do not show summary view if there are more than 10 track groups.
    // Too slow now.
    // TODO(dproy): Fix this.
    if (Object.keys(globals.state.trackGroups).length < 10) {
      this.summaryTrack.renderCanvas(ctx);
    }
    ctx.restore();
  }
}

function StripPathFromExecutable(path: string) {
  return path.split('/').slice(-1)[0];
}