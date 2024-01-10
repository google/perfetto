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

import {hex} from 'color-convert';
import m from 'mithril';

import {Icons} from '../base/semantic_icons';
import {Actions} from '../common/actions';
import {
  getContainingTrackId,
} from '../common/state';
import {TrackCacheEntry} from '../common/track_cache';
import {TrackTags} from '../public';

import {
  COLLAPSED_BACKGROUND,
  EXPANDED_BACKGROUND,
  TRACK_SHELL_WIDTH,
} from './css_constants';
import {globals} from './globals';
import {drawGridLines} from './gridline_helper';
import {PanelSize} from './panel';
import {Panel} from './panel_container';
import {renderChips, TrackContent} from './track_panel';
import {
  drawVerticalLineAtTime,
} from './vertical_line_helper';

interface Attrs {
  trackGroupId: string;
  key: string;
  title: string;
  collapsed: boolean;
  trackFSM?: TrackCacheEntry;
  tags?: TrackTags;
  labels?: string[];
}

export class TrackGroupPanel implements Panel {
  readonly kind = 'panel';
  readonly selectable = true;
  readonly key: string;
  readonly trackGroupId: string;

  constructor(private attrs: Attrs) {
    this.trackGroupId = attrs.trackGroupId;
    this.key = attrs.key;
  }

  get mithril(): m.Children {
    const {
      trackGroupId,
      title,
      labels,
      tags,
      collapsed,
      trackFSM,
    } = this.attrs;

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
      const parentTrackId = getContainingTrackId(globals.state, trackKey);
      if (parentTrackId === trackGroupId) {
        highlightClass = 'flash';
      }
    }

    const selection = globals.state.currentSelection;

    const trackGroup = globals.state.trackGroups[trackGroupId];
    let checkBox = Icons.BlankCheckbox;
    if (selection !== null && selection.kind === 'AREA') {
      const selectedArea = globals.state.areas[selection.areaId];
      if (selectedArea.tracks.includes(trackGroupId) &&
          trackGroup.tracks.every((id) => selectedArea.tracks.includes(id))) {
        checkBox = Icons.Checkbox;
      } else if (
          selectedArea.tracks.includes(trackGroupId) ||
          trackGroup.tracks.some((id) => selectedArea.tracks.includes(id))) {
        checkBox = Icons.IndeterminateCheckbox;
      }
    }

    let child = null;
    if (labels && labels.length > 0) {
      child = labels.join(', ');
    }

    return m(
        `.track-group-panel[collapsed=${collapsed}]`,
        {
          id: 'track_' + trackGroupId,
          oncreate: () => this.onupdate(),
          onupdate: () => this.onupdate(),
        },
        m(`.shell`,
          {
            onclick: (e: MouseEvent) => {
              globals.dispatch(Actions.toggleTrackGroupCollapsed({
                trackGroupId,
              })),
                  e.stopPropagation();
            },
            class: `${highlightClass}`,
          },

          m('.fold-button',
            m('i.material-icons',
              collapsed ? Icons.ExpandDown : Icons.ExpandUp)),
          m('.title-wrapper',
            m(
                'h1.track-title',
                {title: name},
                name,
                renderChips(tags),
                ),
            (collapsed && child !== null) ? m('h2.track-subtitle', child) :
                                            null),
          selection && selection.kind === 'AREA' ?
              m('i.material-icons.track-button',
                {
                  onclick: (e: MouseEvent) => {
                    globals.dispatch(Actions.toggleTrackSelection(
                        {id: trackGroupId, isTrackGroup: true}));
                    e.stopPropagation();
                  },
                },
                checkBox) :
              ''),

        trackFSM ? m(TrackContent,
                     {track: trackFSM.track},
                     (!collapsed && child !== null) ? m('span', child) : null) :
                   null);
  }

  private onupdate() {
    if (this.attrs.trackFSM !== undefined) {
      this.attrs.trackFSM.track.onFullRedraw?.();
    }
  }

  highlightIfTrackSelected(ctx: CanvasRenderingContext2D, size: PanelSize) {
    const {visibleTimeScale} = globals.timeline;
    const selection = globals.state.currentSelection;
    if (!selection || selection.kind !== 'AREA') return;
    const selectedArea = globals.state.areas[selection.areaId];
    const selectedAreaDuration = selectedArea.end - selectedArea.start;
    if (selectedArea.tracks.includes(this.trackGroupId)) {
      ctx.fillStyle = 'rgba(131, 152, 230, 0.3)';
      ctx.fillRect(
          visibleTimeScale.timeToPx(selectedArea.start) + TRACK_SHELL_WIDTH,
          0,
          visibleTimeScale.durationToPx(selectedAreaDuration),
          size.height);
    }
  }

  renderCanvas(ctx: CanvasRenderingContext2D, size: PanelSize) {
    const {
      collapsed,
      trackFSM: track,
    } = this.attrs;

    ctx.fillStyle = collapsed ? COLLAPSED_BACKGROUND : EXPANDED_BACKGROUND;
    ctx.fillRect(0, 0, size.width, size.height);

    if (!collapsed) return;

    this.highlightIfTrackSelected(ctx, size);

    drawGridLines(
        ctx,
        size.width,
        size.height);

    ctx.save();
    ctx.translate(TRACK_SHELL_WIDTH, 0);
    if (track) {
      const trackSize = {...size, width: size.width - TRACK_SHELL_WIDTH};
      track.update();
      track.track.render(ctx, trackSize);
    }
    ctx.restore();

    this.highlightIfTrackSelected(ctx, size);

    const {visibleTimeScale} = globals.timeline;
    // Draw vertical line when hovering on the notes panel.
    if (globals.state.hoveredNoteTimestamp !== -1n) {
      drawVerticalLineAtTime(
          ctx,
          visibleTimeScale,
          globals.state.hoveredNoteTimestamp,
          size.height,
          `#aaa`);
    }
    if (globals.state.hoverCursorTimestamp !== -1n) {
      drawVerticalLineAtTime(
          ctx,
          visibleTimeScale,
          globals.state.hoverCursorTimestamp,
          size.height,
          `#344596`);
    }

    if (globals.state.currentSelection !== null) {
      if (globals.state.currentSelection.kind === 'SLICE' &&
          globals.sliceDetails.wakeupTs !== undefined) {
        drawVerticalLineAtTime(
            ctx,
            visibleTimeScale,
            globals.sliceDetails.wakeupTs,
            size.height,
            `black`);
      }
    }
    // All marked areas should have semi-transparent vertical lines
    // marking the start and end.
    for (const note of Object.values(globals.state.notes)) {
      if (note.noteType === 'AREA') {
        const transparentNoteColor =
            'rgba(' + hex.rgb(note.color.substr(1)).toString() + ', 0.65)';
        drawVerticalLineAtTime(
            ctx,
            visibleTimeScale,
            globals.state.areas[note.areaId].start,
            size.height,
            transparentNoteColor,
            1);
        drawVerticalLineAtTime(
            ctx,
            visibleTimeScale,
            globals.state.areas[note.areaId].end,
            size.height,
            transparentNoteColor,
            1);
      } else if (note.noteType === 'DEFAULT') {
        drawVerticalLineAtTime(
            ctx, visibleTimeScale, note.timestamp, size.height, note.color);
      }
    }
  }
}

function StripPathFromExecutable(path: string) {
  return path.split('/').slice(-1)[0];
}
