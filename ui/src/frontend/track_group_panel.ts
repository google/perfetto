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
import {TrackContent} from './track_panel';
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
    const name = StripPathFromExecutable(this.trackGroupState.name);
    return m(
        `.track-group-panel[collapsed=${collapsed}]`,
        m('.shell',
          m('h1',
            {
              title: name,
            },
            name,
            m.trust('&#x200E;')),
          m('.fold-button',
            {
              onclick: (e:MouseEvent) => {
                globals.dispatch(Actions.toggleTrackGroupCollapsed({
                  trackGroupId: attrs.trackGroupId,
                })),
                e.stopPropagation();
              }
            },
            m('i.material-icons',
              this.trackGroupState.collapsed ? 'expand_more' : 'expand_less'))),
        m(TrackContent, {track: this.summaryTrack}), );
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

    ctx.fillStyle = this.backgroundColor;
    ctx.fillRect(0, 0, size.width, size.height);

    drawGridLines(
        ctx,
        globals.frontendLocalState.timeScale,
        globals.frontendLocalState.visibleWindowTime,
        size.width,
        size.height);

    ctx.translate(this.shellWidth, 0);
    this.summaryTrack.renderCanvas(ctx);
    ctx.restore();
  }
}

function StripPathFromExecutable(path: string) {
  return path.split('/').slice(-1)[0];
}
