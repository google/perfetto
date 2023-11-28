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

import {assertExists} from '../base/logging';
import {Icons} from '../base/semantic_icons';
import {Actions} from '../common/actions';
import {pluginManager} from '../common/plugins';
import {
  getContainingTrackId,
  TrackGroupState,
  TrackState,
} from '../common/state';
import {Migrate, Track, TrackContext} from '../public';

import {globals} from './globals';
import {drawGridLines} from './gridline_helper';
import {Panel, PanelSize} from './panel';
import {renderChips, TrackContent} from './track_panel';
import {
  drawVerticalLineAtTime,
} from './vertical_line_helper';

interface Attrs {
  trackGroupId: string;
  selectable: boolean;
}

export class TrackGroupPanel extends Panel<Attrs> {
  private readonly trackGroupId: string;
  private shellWidth = 0;
  private backgroundColor = '#ffffff';  // Updated from CSS later.
  private summaryTrack?: Track;

  constructor({attrs}: m.CVnode<Attrs>) {
    super();
    this.trackGroupId = attrs.trackGroupId;
  }

  private tryLoadTrack() {
    const groupId = this.trackGroupId;
    const trackState = this.summaryTrackState;

    const {key, uri, params} = trackState;

    const ctx: TrackContext = {
      trackKey: key,
      mountStore: <T>(migrate: Migrate<T>) => {
        const {store, state} = globals;
        const migratedState = migrate(state.trackGroups[groupId].state);
        store.edit((draft) => {
          draft.trackGroups[groupId].state = migratedState;
        });
        return store.createProxy<T>(['trackGroups', groupId, 'state']);
      },
      params,
    };

    this.summaryTrack = pluginManager.createTrack(uri, ctx);
  }

  get trackGroupState(): TrackGroupState {
    return assertExists(globals.state.trackGroups[this.trackGroupId]);
  }

  get summaryTrackState(): TrackState {
    return assertExists(globals.state.tracks[this.trackGroupState.tracks[0]]);
  }

  view({attrs}: m.CVnode<Attrs>) {
    if (!this.summaryTrack) {
      this.tryLoadTrack();
    }

    const collapsed = this.trackGroupState.collapsed;
    let name = this.trackGroupState.name;
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
      if (parentTrackId === attrs.trackGroupId) {
        highlightClass = 'flash';
      }
    }

    const selection = globals.state.currentSelection;

    const trackGroup = globals.state.trackGroups[attrs.trackGroupId];
    let checkBox = Icons.BlankCheckbox;
    if (selection !== null && selection.kind === 'AREA') {
      const selectedArea = globals.state.areas[selection.areaId];
      if (selectedArea.tracks.includes(attrs.trackGroupId) &&
          trackGroup.tracks.every((id) => selectedArea.tracks.includes(id))) {
        checkBox = Icons.Checkbox;
      } else if (
          selectedArea.tracks.includes(attrs.trackGroupId) ||
          trackGroup.tracks.some((id) => selectedArea.tracks.includes(id))) {
        checkBox = Icons.IndeterminateCheckbox;
      }
    }

    let child = null;
    if (this.summaryTrackState.labels &&
        this.summaryTrackState.labels.length > 0) {
      child = this.summaryTrackState.labels.join(', ');
    }

    return m(
        `.track-group-panel[collapsed=${collapsed}]`,
        {id: 'track_' + this.trackGroupId},
        m(`.shell`,
          {
            onclick: (e: MouseEvent) => {
              globals.dispatch(Actions.toggleTrackGroupCollapsed({
                trackGroupId: attrs.trackGroupId,
              })),
                  e.stopPropagation();
            },
            class: `${highlightClass}`,
          },

          m('.fold-button',
            m('i.material-icons',
              this.trackGroupState.collapsed ? Icons.ExpandDown :
                                               Icons.ExpandUp)),
          m('.title-wrapper',
            m(
                'h1.track-title',
                {title: name},
                name,
                renderChips(this.summaryTrackState),
                ),
            (this.trackGroupState.collapsed && child !== null) ?
                m('h2.track-subtitle', child) :
                null),
          selection && selection.kind === 'AREA' ?
              m('i.material-icons.track-button',
                {
                  onclick: (e: MouseEvent) => {
                    globals.dispatch(Actions.toggleTrackSelection(
                        {id: attrs.trackGroupId, isTrackGroup: true}));
                    e.stopPropagation();
                  },
                },
                checkBox) :
              ''),

        this.summaryTrack ?
            m(TrackContent,
              {track: this.summaryTrack},
              (!this.trackGroupState.collapsed && child !== null) ?
                  m('span', child) :
                  null) :
            null);
  }

  oncreate(vnode: m.CVnodeDOM<Attrs>) {
    this.onupdate(vnode);
  }

  onupdate({dom}: m.CVnodeDOM<Attrs>) {
    const shell = assertExists(dom.querySelector('.shell'));
    this.shellWidth = shell.getBoundingClientRect().width;
    // TODO(andrewbb): move this to css_constants
    if (this.trackGroupState.collapsed) {
      this.backgroundColor =
          getComputedStyle(dom).getPropertyValue('--collapsed-background');
    } else {
      this.backgroundColor =
          getComputedStyle(dom).getPropertyValue('--expanded-background');
    }
    if (this.summaryTrack !== undefined) {
      this.summaryTrack.onFullRedraw();
    }
  }

  onremove() {
    if (this.summaryTrack !== undefined) {
      this.summaryTrack.onDestroy();
      this.summaryTrack = undefined;
    }
  }

  highlightIfTrackSelected(ctx: CanvasRenderingContext2D, size: PanelSize) {
    const {visibleTimeScale} = globals.frontendLocalState;
    const selection = globals.state.currentSelection;
    if (!selection || selection.kind !== 'AREA') return;
    const selectedArea = globals.state.areas[selection.areaId];
    const selectedAreaDuration = selectedArea.end - selectedArea.start;
    if (selectedArea.tracks.includes(this.trackGroupId)) {
      ctx.fillStyle = 'rgba(131, 152, 230, 0.3)';
      ctx.fillRect(
          visibleTimeScale.timeToPx(selectedArea.start) + this.shellWidth,
          0,
          visibleTimeScale.durationToPx(selectedAreaDuration),
          size.height);
    }
  }

  renderCanvas(ctx: CanvasRenderingContext2D, size: PanelSize) {
    const collapsed = this.trackGroupState.collapsed;

    ctx.fillStyle = this.backgroundColor;
    ctx.fillRect(0, 0, size.width, size.height);

    if (!collapsed) return;

    this.highlightIfTrackSelected(ctx, size);

    drawGridLines(
        ctx,
        size.width,
        size.height);

    ctx.save();
    ctx.translate(this.shellWidth, 0);
    if (this.summaryTrack) {
      this.summaryTrack.render(ctx);
    }
    ctx.restore();

    this.highlightIfTrackSelected(ctx, size);

    const {visibleTimeScale} = globals.frontendLocalState;
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
