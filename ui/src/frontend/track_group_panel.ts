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
import {Actions, RemoveTrackArgs, RemoveTrackGroupArgs} from '../common/actions';
import {
  getContainingTrackIds,
  TrackGroupState,
  TrackState,
} from '../common/state';

import {globals} from './globals';
import {drawGridLines} from './gridline_helper';
import {
  BLANK_CHECKBOX,
  CHECKBOX,
  CHEVRON_RIGHT,
  EXPAND_DOWN,
  INDETERMINATE_CHECKBOX,
} from './icons';
import {Panel, PanelSize} from './panel';
import {Track} from './track';
import {TrackButton, TrackContent} from './track_panel';
import {trackRegistry} from './track_registry';
import {
  drawVerticalLineAtTime,
} from './vertical_line_helper';
import {getActiveVsyncData, renderVsyncColumns} from './vsync_helper';
import {getCssStr} from './css_constants';

interface Attrs {
  trackGroupId: string;
  selectable: boolean;
}

export class TrackGroupPanel extends Panel<Attrs> {
  private readonly trackGroupId: string;
  private shellWidth = 0;
  private backgroundColor = '#ffffff';  // Updated from CSS later.
  private summaryTrack: Track|undefined;

  // Caches the last state.trackGroups[this.trackGroupId].
  // This is to deal with track group deletion. See comments
  // in trackGroupState() below.
  private lastTrackGroupState: TrackGroupState;

  constructor({attrs}: m.CVnode<Attrs>) {
    super();
    this.trackGroupId = attrs.trackGroupId;
    const trackCreator = trackRegistry.get(this.summaryTrackState.kind);
    const engineId = this.summaryTrackState.engineId;
    const engine = globals.engines.get(engineId);
    if (engine !== undefined) {
      this.summaryTrack =
          trackCreator.create({trackId: this.summaryTrackState.id, engine});
    }
    this.lastTrackGroupState = assertExists(
      globals.state.trackGroups[this.trackGroupId]);
  }

  get trackGroupState(): TrackGroupState {
    // We can end up in a state where a Track Group is still in the mithril
    // renderer tree but its corresponding state has been deleted. This can
    // happen in the interval of time between a group being removed from the
    // state and the next animation frame that would remove the group object.
    // If a mouse event is dispatched in the meanwhile (or a promise is
    // resolved), we need to be able to access the state. Hence the caching
    // logic here.
    const result = globals.state.trackGroups[this.trackGroupId];
    if (result === undefined) {
      return this.lastTrackGroupState;
    }
    this.lastTrackGroupState = result;
    return result;
  }

  get summaryTrackState(): TrackState {
    return assertExists(globals.state.tracks[this.trackGroupState.tracks[0]]);
  }

  view({attrs}: m.CVnode<Attrs>) {
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
      const trackId = globals.currentSearchResults.trackIds[searchIndex];
      const parentTrackIds = getContainingTrackIds(globals.state, trackId);
      if (parentTrackIds && parentTrackIds.includes(attrs.trackGroupId)) {
        highlightClass = 'flash';
      }
    }

    const selection = globals.state.currentSelection;

    const trackGroup = globals.state.trackGroups[attrs.trackGroupId];
    let checkBox = BLANK_CHECKBOX;
    if (selection !== null && selection.kind === 'AREA') {
      const selectedArea = globals.state.areas[selection.areaId];
      if (selectedArea.tracks.includes(attrs.trackGroupId) &&
          trackGroup.tracks.every((id) => selectedArea.tracks.includes(id))) {
        checkBox = CHECKBOX;
      } else if (
          selectedArea.tracks.includes(attrs.trackGroupId) ||
          trackGroup.tracks.some((id) => selectedArea.tracks.includes(id))) {
        checkBox = INDETERMINATE_CHECKBOX;
      }
    }

    let child = null;
    if (this.summaryTrackState.labels &&
        this.summaryTrackState.labels.length > 0) {
      child = this.summaryTrackState.labels.join(', ');
    }

    const depth: (group?: TrackGroupState) => number =
      (group?: TrackGroupState) =>
        group?.parentGroup ?
          depth(globals.state.trackGroups[group.parentGroup]) + 1 :
          0;
    const indent = (depth: number) => depth <= 0 ?
      {} :
      {style: {marginLeft: `${depth/2}rem`}};

    const titleStyling = indent(depth(trackGroup));
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
            {...titleStyling},
            m('i.material-icons',
              this.trackGroupState.collapsed ? CHEVRON_RIGHT : EXPAND_DOWN)),
          m('.title-wrapper',
            {...titleStyling},
            m('h1.track-title',
              {title: trackGroup.description},
              name,
              ('namespace' in this.summaryTrackState.config) &&
                  m('span.chip', 'metric')),
            (this.trackGroupState.collapsed && child !== null) ?
              m('h2.track-subtitle', child) :
                null),
          m('.track-buttons', ...this.getTrackGroupActionButtons(),
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
          ),

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
    const trackGroupId = vnode.attrs.trackGroupId;
    if (globals.frontendLocalState.expandTrackGroupIds.has(trackGroupId)) {
      // An attempt to scroll to reveal a track that is contained within
      // this group was waiting for it to be created by expansion of an
      // ancestor, so make sure that it is expanded now that it exists
      if (this.trackGroupState.collapsed) {
        globals.dispatch(Actions.toggleTrackGroupCollapsed({trackGroupId}));
      }
      globals.frontendLocalState.expandTrackGroupIds.delete(trackGroupId);
    }
  }

  onupdate({dom}: m.CVnodeDOM<Attrs>) {
    const shell = assertExists(dom.querySelector('.shell'));
    this.shellWidth = shell.getBoundingClientRect().width;
    // TODO(andrewbb): move this to css_constants
    this.backgroundColor =
          getComputedStyle(dom).getPropertyValue('--collapsed-background');
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

  getTrackGroupActionButtons(): m.Vnode<any>[] {
    const result: m.Vnode<any>[] = [];
  const disabled = !this.canDeleteTrackGroup(this.trackGroupState);
      result.push(
        m(TrackButton, {
          action: (e: MouseEvent) => {
            const removeTracks: RemoveTrackArgs[] = [];
            const removeGroups: RemoveTrackGroupArgs[] = [];
            this.collectRemoveTrackGroupActions(
              this.trackGroupState,
              removeTracks,
              removeGroups,
            );
            globals.dispatchMultiple([
              Actions.removeTracks({tracks: removeTracks}),
              Actions.removeTrackGroups({trackGroups: removeGroups}),
            ]);
            e.stopPropagation();
          },
          i: 'delete',
          disabled,
          tooltip: 'Remove track group',
          showButton: false, // Only show on roll-over
          fullHeight: true,
        }));
    return result;
  }

  // Collect, recursively, the nested track groups and tracks to remove
  // along with the given trackGroup, as deferred actions to be dispatched
  protected collectRemoveTrackGroupActions(
      trackGroup: TrackGroupState,
      removeTracks: RemoveTrackArgs[],
      removeGroups: RemoveTrackGroupArgs[]): void {
    // First, recursively remove subgroups, if any
    for (const subgroupId of trackGroup.subgroups) {
      const subgroup = globals.state.trackGroups[subgroupId];
      if (subgroup) {
        this.collectRemoveTrackGroupActions(subgroup,
          removeTracks, removeGroups);
      }
    }

    // Then tracks, except the summary, which is handled by
    // the track group, below
    trackGroup.tracks.slice(1).forEach((id) => removeTracks.push({id}));

    // Then the group
    removeGroups.push({
      id: trackGroup.id,
      summaryTrackId: trackGroup.tracks[0],
    });
  }

  // We cannot delete a track group while its tracks are loading,
  // otherwise we'll try to read data from tables that have been dropped.
  // We assume a track group may be loading if its engine is busy.
  protected canDeleteTrackGroup(trackGroupState: TrackGroupState): boolean {
    const engine = globals.engines.get(trackGroupState.engineId);
    return !engine || !engine.hasDataPending;
  }

  highlightIfTrackSelected(ctx: CanvasRenderingContext2D, size: PanelSize) {
    const {visibleTimeScale} = globals.frontendLocalState;
    const selection = globals.state.currentSelection;
    if (!selection || selection.kind !== 'AREA') return;
    const selectedArea = globals.state.areas[selection.areaId];
    const selectedAreaDuration = selectedArea.end - selectedArea.start;
    if (selectedArea.tracks.includes(this.trackGroupId)) {
      ctx.fillStyle = getCssStr('--selection-fill-color');
      ctx.fillRect(
          visibleTimeScale.tpTimeToPx(selectedArea.start) + this.shellWidth,
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

    // If we have vsync data, render columns under the track group
    const vsync = getActiveVsyncData();
    if (vsync) {
      ctx.save();
      ctx.translate(this.shellWidth, 0);
      renderVsyncColumns(ctx, size.height, vsync);
      ctx.restore();
    }

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
            getCssStr('--main-foreground-color'));
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
