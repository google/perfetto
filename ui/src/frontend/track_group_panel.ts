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

import m from 'mithril';

import {Icons} from '../base/semantic_icons';
import {Actions} from '../common/actions';
import {getContainingGroupKey} from '../common/state';
import {TrackCacheEntry} from '../common/track_cache';
import {TrackTags} from '../public';

import {TRACK_SHELL_WIDTH} from './css_constants';
import {globals} from './globals';
import {drawGridLines} from './gridline_helper';
import {PanelSize} from './panel';
import {Panel} from './panel_container';
import {
  CrashButton,
  renderChips,
  renderHoveredCursorVertical,
  renderHoveredNoteVertical,
  renderNoteVerticals,
  renderWakeupVertical,
  TrackContent,
} from './track_panel';
import {canvasClip} from '../common/canvas_utils';
import {Button} from '../widgets/button';

interface Attrs {
  groupKey: string;
  title: string;
  collapsed: boolean;
  trackFSM?: TrackCacheEntry;
  tags?: TrackTags;
  labels?: string[];
}

export class TrackGroupPanel implements Panel {
  readonly kind = 'panel';
  readonly selectable = true;
  readonly groupKey: string;

  constructor(private attrs: Attrs) {
    this.groupKey = attrs.groupKey;
  }

  render(): m.Children {
    const {groupKey, title, labels, tags, collapsed, trackFSM} = this.attrs;

    let name = title;
    if (name[0] === '/') {
      name = StripPathFromExecutable(name);
    }

    // The shell should be highlighted if the current search result is inside
    // this track group.
    let highlightClass = '';
    const searchIndex = globals.state.searchIndex;
    if (searchIndex !== -1) {
      const trackKey = globals.currentSearchResults.trackKeys[searchIndex];
      const containingGroupKey = getContainingGroupKey(globals.state, trackKey);
      if (containingGroupKey === groupKey) {
        highlightClass = 'flash';
      }
    }

    const selection = globals.state.selection;

    const trackGroup = globals.state.trackGroups[groupKey];
    let checkBox = Icons.BlankCheckbox;
    if (selection.kind === 'area') {
      if (
        selection.tracks.includes(groupKey) &&
        trackGroup.tracks.every((id) => selection.tracks.includes(id))
      ) {
        checkBox = Icons.Checkbox;
      } else if (
        selection.tracks.includes(groupKey) ||
        trackGroup.tracks.some((id) => selection.tracks.includes(id))
      ) {
        checkBox = Icons.IndeterminateCheckbox;
      }
    }

    let child = null;
    if (labels && labels.length > 0) {
      child = labels.join(', ');
    }

    const error = trackFSM?.getError();

    return m(
      `.track-group-panel[collapsed=${collapsed}]`,
      {
        id: 'track_' + groupKey,
        oncreate: () => this.onupdate(),
        onupdate: () => this.onupdate(),
      },
      m(
        `.shell`,
        {
          onclick: (e: MouseEvent) => {
            if (e.defaultPrevented) return;
            globals.dispatch(
              Actions.toggleTrackGroupCollapsed({
                groupKey,
              }),
            ),
              e.stopPropagation();
          },
          class: `${highlightClass}`,
        },
        m(
          '.fold-button',
          m('i.material-icons', collapsed ? Icons.ExpandDown : Icons.ExpandUp),
        ),
        m(
          '.title-wrapper',
          m('h1.track-title', {title: name}, name, renderChips(tags)),
          collapsed && child !== null ? m('h2.track-subtitle', child) : null,
        ),
        m(
          '.track-buttons',
          error && m(CrashButton, {error}),
          selection.kind === 'area' &&
            m(Button, {
              onclick: (e: MouseEvent) => {
                globals.dispatch(
                  Actions.toggleTrackSelection({
                    key: groupKey,
                    isTrackGroup: true,
                  }),
                );
                e.stopPropagation();
              },
              icon: checkBox,
              compact: true,
            }),
        ),
      ),
      trackFSM
        ? m(
            TrackContent,
            {
              track: trackFSM.track,
              hasError: Boolean(trackFSM.getError()),
              height: this.attrs.trackFSM?.track.getHeight(),
            },
            !collapsed && child !== null ? m('span', child) : null,
          )
        : null,
    );
  }

  private onupdate() {
    if (this.attrs.trackFSM !== undefined) {
      this.attrs.trackFSM.track.onFullRedraw?.();
    }
  }

  highlightIfTrackSelected(ctx: CanvasRenderingContext2D, size: PanelSize) {
    const {visibleTimeScale} = globals.timeline;
    const selection = globals.state.selection;
    if (selection.kind !== 'area') return;
    const selectedAreaDuration = selection.end - selection.start;
    if (selection.tracks.includes(this.groupKey)) {
      ctx.fillStyle = 'rgba(131, 152, 230, 0.3)';
      ctx.fillRect(
        visibleTimeScale.timeToPx(selection.start) + TRACK_SHELL_WIDTH,
        0,
        visibleTimeScale.durationToPx(selectedAreaDuration),
        size.height,
      );
    }
  }

  renderCanvas(ctx: CanvasRenderingContext2D, size: PanelSize) {
    const {collapsed, trackFSM: track} = this.attrs;

    if (!collapsed) return;

    ctx.save();
    canvasClip(
      ctx,
      TRACK_SHELL_WIDTH,
      0,
      size.width - TRACK_SHELL_WIDTH,
      size.height,
    );
    drawGridLines(ctx, size.width, size.height);

    if (track) {
      ctx.save();
      ctx.translate(TRACK_SHELL_WIDTH, 0);
      const trackSize = {...size, width: size.width - TRACK_SHELL_WIDTH};
      if (!track.getError()) {
        track.update();
        track.track.render(ctx, trackSize);
      }
      ctx.restore();
    }

    this.highlightIfTrackSelected(ctx, size);

    const {visibleTimeScale} = globals.timeline;
    // Draw vertical line when hovering on the notes panel.
    renderHoveredNoteVertical(ctx, visibleTimeScale, size);
    renderHoveredCursorVertical(ctx, visibleTimeScale, size);
    renderWakeupVertical(ctx, visibleTimeScale, size);
    renderNoteVerticals(ctx, visibleTimeScale, size);

    ctx.restore();
  }
}

function StripPathFromExecutable(path: string) {
  return path.split('/').slice(-1)[0];
}
