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
import * as m from 'mithril';

import {Actions} from '../common/actions';
import {TrackState} from '../common/state';

import {SELECTION_FILL_COLOR, TRACK_SHELL_WIDTH} from './css_constants';
import {PerfettoMouseEvent} from './events';
import {globals} from './globals';
import {drawGridLines} from './gridline_helper';
import {BLANK_CHECKBOX, CHECKBOX, STAR, STAR_BORDER} from './icons';
import {Panel, PanelSize} from './panel';
import {verticalScrollToTrack} from './scroll_helper';
import {SliceRect, Track} from './track';
import {trackRegistry} from './track_registry';
import {
  drawVerticalLineAtTime,
} from './vertical_line_helper';

function isPinned(id: string) {
  return globals.state.pinnedTracks.indexOf(id) !== -1;
}

function isSelected(id: string) {
  const selection = globals.state.currentSelection;
  if (selection === null || selection.kind !== 'AREA') return false;
  const selectedArea = globals.state.areas[selection.areaId];
  return selectedArea.tracks.includes(id);
}

interface TrackShellAttrs {
  track: Track;
  trackState: TrackState;
}

class TrackShell implements m.ClassComponent<TrackShellAttrs> {
  // Set to true when we click down and drag the
  private dragging = false;
  private dropping: 'before'|'after'|undefined = undefined;
  private attrs?: TrackShellAttrs;

  oninit(vnode: m.Vnode<TrackShellAttrs>) {
    this.attrs = vnode.attrs;
  }

  view({attrs}: m.CVnode<TrackShellAttrs>) {
    // The shell should be highlighted if the current search result is inside
    // this track.
    let highlightClass = '';
    const searchIndex = globals.state.searchIndex;
    if (searchIndex !== -1) {
      const trackId = globals.currentSearchResults.trackIds[searchIndex];
      if (trackId === attrs.trackState.id) {
        highlightClass = 'flash';
      }
    }

    const dragClass = this.dragging ? `drag` : '';
    const dropClass = this.dropping ? `drop-${this.dropping}` : '';
    return m(
        `.track-shell[draggable=true]`,
        {
          class: `${highlightClass} ${dragClass} ${dropClass}`,
          onmousedown: this.onmousedown.bind(this),
          ondragstart: this.ondragstart.bind(this),
          ondragend: this.ondragend.bind(this),
          ondragover: this.ondragover.bind(this),
          ondragleave: this.ondragleave.bind(this),
          ondrop: this.ondrop.bind(this),
        },
        m(
            'h1',
            {
              title: attrs.trackState.name,
            },
            attrs.trackState.name,
            ('namespace' in attrs.trackState.config) &&
                m('span.chip', 'metric'),
            ),
        m('.track-buttons',
          attrs.track.getTrackShellButtons(),
          attrs.track.getContextMenu(),
          m(TrackButton, {
            action: () => {
              globals.dispatch(
                  Actions.toggleTrackPinned({trackId: attrs.trackState.id}));
            },
            i: isPinned(attrs.trackState.id) ? STAR : STAR_BORDER,
            tooltip: isPinned(attrs.trackState.id) ? 'Unpin' : 'Pin to top',
            showButton: isPinned(attrs.trackState.id),
            fullHeight: true,
          }),
          globals.state.currentSelection !== null &&
                  globals.state.currentSelection.kind === 'AREA' ?
              m(TrackButton, {
                action: (e: PerfettoMouseEvent) => {
                  globals.dispatch(Actions.toggleTrackSelection(
                      {id: attrs.trackState.id, isTrackGroup: false}));
                  e.stopPropagation();
                },
                i: isSelected(attrs.trackState.id) ? CHECKBOX : BLANK_CHECKBOX,
                tooltip: isSelected(attrs.trackState.id) ?
                    'Remove track' :
                    'Add track to selection',
                showButton: true,
              }) :
              ''));
  }

  onmousedown(e: MouseEvent) {
    // Prevent that the click is intercepted by the PanAndZoomHandler and that
    // we start panning while dragging.
    e.stopPropagation();
  }

  ondragstart(e: DragEvent) {
    const dataTransfer = e.dataTransfer;
    if (dataTransfer === null) return;
    this.dragging = true;
    globals.rafScheduler.scheduleFullRedraw();
    dataTransfer.setData('perfetto/track', `${this.attrs!.trackState.id}`);
    dataTransfer.setDragImage(new Image(), 0, 0);
    e.stopImmediatePropagation();
  }

  ondragend() {
    this.dragging = false;
    globals.rafScheduler.scheduleFullRedraw();
  }

  ondragover(e: DragEvent) {
    if (this.dragging) return;
    if (!(e.target instanceof HTMLElement)) return;
    const dataTransfer = e.dataTransfer;
    if (dataTransfer === null) return;
    if (!dataTransfer.types.includes('perfetto/track')) return;
    dataTransfer.dropEffect = 'move';
    e.preventDefault();

    // Apply some hysteresis to the drop logic so that the lightened border
    // changes only when we get close enough to the border.
    if (e.offsetY < e.target.scrollHeight / 3) {
      this.dropping = 'before';
    } else if (e.offsetY > e.target.scrollHeight / 3 * 2) {
      this.dropping = 'after';
    }
    globals.rafScheduler.scheduleFullRedraw();
  }

  ondragleave() {
    this.dropping = undefined;
    globals.rafScheduler.scheduleFullRedraw();
  }

  ondrop(e: DragEvent) {
    if (this.dropping === undefined) return;
    const dataTransfer = e.dataTransfer;
    if (dataTransfer === null) return;
    globals.rafScheduler.scheduleFullRedraw();
    const srcId = dataTransfer.getData('perfetto/track');
    const dstId = this.attrs!.trackState.id;
    globals.dispatch(Actions.moveTrack({srcId, op: this.dropping, dstId}));
    this.dropping = undefined;
  }
}

export interface TrackContentAttrs { track: Track; }
export class TrackContent implements m.ClassComponent<TrackContentAttrs> {
  private mouseDownX?: number;
  private mouseDownY?: number;
  private selectionOccurred = false;

  view(node: m.CVnode<TrackContentAttrs>) {
    const attrs = node.attrs;
    return m(
        '.track-content',
        {
          onmousemove: (e: PerfettoMouseEvent) => {
            attrs.track.onMouseMove(
                {x: e.layerX - TRACK_SHELL_WIDTH, y: e.layerY});
            globals.rafScheduler.scheduleRedraw();
          },
          onmouseout: () => {
            attrs.track.onMouseOut();
            globals.rafScheduler.scheduleRedraw();
          },
          onmousedown: (e: PerfettoMouseEvent) => {
            this.mouseDownX = e.layerX;
            this.mouseDownY = e.layerY;
          },
          onmouseup: (e: PerfettoMouseEvent) => {
            if (this.mouseDownX === undefined ||
                this.mouseDownY === undefined) {
              return;
            }
            if (Math.abs(e.layerX - this.mouseDownX) > 1 ||
                Math.abs(e.layerY - this.mouseDownY) > 1) {
              this.selectionOccurred = true;
            }
            this.mouseDownX = undefined;
            this.mouseDownY = undefined;
          },
          onclick: (e: PerfettoMouseEvent) => {
            // This click event occurs after any selection mouse up/drag events
            // so we have to look if the mouse moved during this click to know
            // if a selection occurred.
            if (this.selectionOccurred) {
              this.selectionOccurred = false;
              return;
            }
            // Returns true if something was selected, so stop propagation.
            if (attrs.track.onMouseClick(
                    {x: e.layerX - TRACK_SHELL_WIDTH, y: e.layerY})) {
              e.stopPropagation();
            }
            globals.rafScheduler.scheduleRedraw();
          },
        },
        node.children);
  }
}

interface TrackComponentAttrs {
  trackState: TrackState;
  track: Track;
}
class TrackComponent implements m.ClassComponent<TrackComponentAttrs> {
  view({attrs}: m.CVnode<TrackComponentAttrs>) {
    return m(
        '.track',
        {
          style: {
            height: `${Math.max(24, attrs.track.getHeight())}px`,
          },
          id: 'track_' + attrs.trackState.id,
        },
        [
          m(TrackShell, {track: attrs.track, trackState: attrs.trackState}),
          m(TrackContent, {track: attrs.track}),
        ]);
  }

  oncreate({attrs}: m.CVnode<TrackComponentAttrs>) {
    if (globals.frontendLocalState.scrollToTrackId === attrs.trackState.id) {
      verticalScrollToTrack(attrs.trackState.id);
      globals.frontendLocalState.scrollToTrackId = undefined;
    }
  }
}

export interface TrackButtonAttrs {
  action: (e: PerfettoMouseEvent) => void;
  i: string;
  tooltip: string;
  showButton: boolean;
  fullHeight?: boolean;
}
export class TrackButton implements m.ClassComponent<TrackButtonAttrs> {
  view({attrs}: m.CVnode<TrackButtonAttrs>) {
    return m(
        'i.material-icons.track-button',
        {
          class: [
            (attrs.showButton ? 'show' : ''),
            (attrs.fullHeight ? 'full-height' : ''),
          ].filter(Boolean)
                     .join(' '),
          onclick: attrs.action,
          title: attrs.tooltip,
        },
        attrs.i);
  }
}

interface TrackPanelAttrs {
  id: string;
  selectable: boolean;
}

export class TrackPanel extends Panel<TrackPanelAttrs> {
  // TODO(hjd): It would be nicer if these could not be undefined here.
  // We should implement a NullTrack which can be used if the trackState
  // has disappeared.
  private track: Track|undefined;
  private trackState: TrackState|undefined;

  constructor(vnode: m.CVnode<TrackPanelAttrs>) {
    super();
    const trackId = vnode.attrs.id;
    const trackState = globals.state.tracks[trackId];
    if (trackState === undefined) {
      return;
    }
    const engine = globals.engines.get(trackState.engineId);
    if (engine === undefined) {
      return;
    }
    const trackCreator = trackRegistry.get(trackState.kind);
    this.track = trackCreator.create({trackId, engine});
    this.trackState = trackState;
  }

  view() {
    if (this.track === undefined || this.trackState === undefined) {
      return m('div', 'No such track');
    }
    return m(TrackComponent, {trackState: this.trackState, track: this.track});
  }

  oncreate() {
    if (this.track !== undefined) {
      this.track.onFullRedraw();
    }
  }

  onupdate() {
    if (this.track !== undefined) {
      this.track.onFullRedraw();
    }
  }

  onremove() {
    if (this.track !== undefined) {
      this.track.onDestroy();
      this.track = undefined;
    }
  }

  highlightIfTrackSelected(ctx: CanvasRenderingContext2D, size: PanelSize) {
    const localState = globals.frontendLocalState;
    const selection = globals.state.currentSelection;
    const trackState = this.trackState;
    if (!selection || selection.kind !== 'AREA' || trackState === undefined) {
      return;
    }
    const selectedArea = globals.state.areas[selection.areaId];
    if (selectedArea.tracks.includes(trackState.id)) {
      const timeScale = localState.timeScale;
      ctx.fillStyle = SELECTION_FILL_COLOR;
      ctx.fillRect(
          timeScale.timeToPx(selectedArea.startSec) + TRACK_SHELL_WIDTH,
          0,
          timeScale.deltaTimeToPx(selectedArea.endSec - selectedArea.startSec),
          size.height);
    }
  }

  renderCanvas(ctx: CanvasRenderingContext2D, size: PanelSize) {
    ctx.save();

    drawGridLines(
        ctx,
        size.width,
        size.height);

    ctx.translate(TRACK_SHELL_WIDTH, 0);
    if (this.track !== undefined) {
      this.track.render(ctx);
    }
    ctx.restore();

    this.highlightIfTrackSelected(ctx, size);

    const localState = globals.frontendLocalState;
    // Draw vertical line when hovering on the notes panel.
    if (globals.state.hoveredNoteTimestamp !== -1) {
      drawVerticalLineAtTime(
          ctx,
          localState.timeScale,
          globals.state.hoveredNoteTimestamp,
          size.height,
          `#aaa`);
    }
    if (globals.state.hoveredLogsTimestamp !== -1) {
      drawVerticalLineAtTime(
          ctx,
          localState.timeScale,
          globals.state.hoveredLogsTimestamp,
          size.height,
          `#344596`);
    }
    if (globals.state.currentSelection !== null) {
      if (globals.state.currentSelection.kind === 'NOTE') {
        const note = globals.state.notes[globals.state.currentSelection.id];
        if (note.noteType === 'DEFAULT') {
          drawVerticalLineAtTime(
              ctx,
              localState.timeScale,
              note.timestamp,
              size.height,
              note.color);
        }
      }

      if (globals.state.currentSelection.kind === 'SLICE' &&
          globals.sliceDetails.wakeupTs !== undefined) {
        drawVerticalLineAtTime(
            ctx,
            localState.timeScale,
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
            localState.timeScale,
            globals.state.areas[note.areaId].startSec,
            size.height,
            transparentNoteColor,
            1);
        drawVerticalLineAtTime(
            ctx,
            localState.timeScale,
            globals.state.areas[note.areaId].endSec,
            size.height,
            transparentNoteColor,
            1);
      }
    }
  }

  getSliceRect(tStart: number, tDur: number, depth: number): SliceRect
      |undefined {
    if (this.track === undefined) {
      return undefined;
    }
    return this.track.getSliceRect(tStart, tDur, depth);
  }
}
