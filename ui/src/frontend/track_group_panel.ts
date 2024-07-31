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
import {Size} from '../base/geom';
import {Panel} from './panel_container';
import {
  CrashButton,
  drawGridLines,
  renderChips,
  renderHoveredCursorVertical,
  renderHoveredNoteVertical,
  renderNoteVerticals,
  renderWakeupVertical,
  TrackContent,
} from './track_panel';
import {canvasClip} from '../common/canvas_utils';
import {Button} from '../widgets/button';
import {TrackRenderContext} from '../public/tracks';
import {calculateResolution} from '../common/resolution';
import {PxSpan, TimeScale} from './time_scale';
import {exists} from '../base/utils';
import {classNames} from '../base/classnames';

interface Attrs {
  readonly groupKey: string;
  readonly title: m.Children;
  readonly tooltip: string;
  readonly collapsed: boolean;
  readonly collapsable: boolean;
  readonly trackFSM?: TrackCacheEntry;
  readonly tags?: TrackTags;
  readonly subtitle?: string;
  readonly chips?: ReadonlyArray<string>;
}

export class TrackGroupPanel implements Panel {
  readonly kind = 'panel';
  readonly selectable = true;
  readonly groupKey: string;

  constructor(private attrs: Attrs) {
    this.groupKey = attrs.groupKey;
  }

  render(): m.Children {
    const {groupKey, title, subtitle, chips, collapsed, trackFSM, tooltip} =
      this.attrs;

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
          className: classNames(
            this.attrs.collapsable && 'pf-clickable',
            highlightClass,
          ),
          onclick: (e: MouseEvent) => {
            if (e.defaultPrevented) return;
            if (this.attrs.collapsable) {
              globals.dispatch(
                Actions.toggleTrackGroupCollapsed({
                  groupKey,
                }),
              );
            }
            e.stopPropagation();
          },
        },
        this.attrs.collapsable &&
          m(
            '.fold-button',
            m(
              'i.material-icons',
              collapsed ? Icons.ExpandDown : Icons.ExpandUp,
            ),
          ),
        m(
          '.title-wrapper',
          m(
            'h1.track-title',
            {title: tooltip},
            title,
            chips && renderChips(chips),
          ),
          collapsed && exists(subtitle) && m('h2.track-subtitle', subtitle),
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
            !collapsed && subtitle !== null ? m('span', subtitle) : null,
          )
        : null,
    );
  }

  private onupdate() {
    if (this.attrs.trackFSM !== undefined) {
      this.attrs.trackFSM.track.onFullRedraw?.();
    }
  }

  highlightIfTrackSelected(
    ctx: CanvasRenderingContext2D,
    timescale: TimeScale,
    size: Size,
  ) {
    const selection = globals.state.selection;
    if (selection.kind !== 'area') return;
    const selectedAreaDuration = selection.end - selection.start;
    if (selection.tracks.includes(this.groupKey)) {
      ctx.fillStyle = 'rgba(131, 152, 230, 0.3)';
      ctx.fillRect(
        timescale.timeToPx(selection.start),
        0,
        timescale.durationToPx(selectedAreaDuration),
        size.height,
      );
    }
  }

  renderCanvas(ctx: CanvasRenderingContext2D, size: Size) {
    const {collapsed, trackFSM: track} = this.attrs;

    if (!collapsed) return;

    const trackSize = {
      width: size.width - TRACK_SHELL_WIDTH,
      height: size.height,
    };

    ctx.save();
    ctx.translate(TRACK_SHELL_WIDTH, 0);
    canvasClip(ctx, 0, 0, trackSize.width, trackSize.height);

    const visibleWindow = globals.timeline.visibleWindow;
    const timespan = visibleWindow.toTimeSpan();
    const timescale = new TimeScale(
      visibleWindow,
      new PxSpan(0, trackSize.width),
    );

    drawGridLines(ctx, timespan, timescale, trackSize);

    if (track) {
      if (!track.getError()) {
        const trackRenderCtx: TrackRenderContext = {
          visibleWindow,
          size: trackSize,
          ctx,
          trackKey: track.trackKey,
          resolution: calculateResolution(visibleWindow, trackSize.width),
          timescale,
        };
        track.render(trackRenderCtx);
      }
    }

    this.highlightIfTrackSelected(ctx, timescale, size);

    // Draw vertical line when hovering on the notes panel.
    renderHoveredNoteVertical(ctx, timescale, size);
    renderHoveredCursorVertical(ctx, timescale, size);
    renderWakeupVertical(ctx, timescale, size);
    renderNoteVerticals(ctx, timescale, size);

    ctx.restore();
  }
}
