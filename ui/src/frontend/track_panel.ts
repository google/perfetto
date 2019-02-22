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
import {drawVerticalSelection,
        drawVerticalLineAtTime} from './vertical_line_helper';
import {Panel, PanelSize} from './panel';
import {Track} from './track';
import {TRACK_SHELL_WIDTH} from './track_constants';
import {trackRegistry} from './track_registry';

function isPinned(id: string) {
  return globals.state.pinnedTracks.indexOf(id) !== -1;
}

interface TrackShellAttrs {
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
    const dragClass = this.dragging ? `.drag` : '';
    const dropClass = this.dropping ? `.drop-${this.dropping}` : '';
    return m(
        `.track-shell${dragClass}${dropClass}[draggable=true]`,
        {
          onmousedown: this.onmousedown.bind(this),
          ondragstart: this.ondragstart.bind(this),
          ondragend: this.ondragend.bind(this),
          ondragover: this.ondragover.bind(this),
          ondragleave: this.ondragleave.bind(this),
          ondrop: this.ondrop.bind(this),
        },
        m('h1',
          {
            title: attrs.trackState.name,
          },
          attrs.trackState.name,
          m.trust('&#x200E;')),
        m(TrackButton, {
          action: Actions.toggleTrackPinned({trackId: attrs.trackState.id}),
          i: isPinned(attrs.trackState.id) ? 'star' : 'star_border',
        }));
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
      onclick: (e:MouseEvent) => {
        // If we are selecting a timespan - do not pass the click to the track.
        const selection = globals.state.currentSelection;
        if (selection && selection.kind === 'TIMESPAN') return;

        if (attrs.track.onMouseClick({x: e.layerX, y: e.layerY})) {
          e.stopPropagation();
        }
        globals.rafScheduler.scheduleRedraw();
      }
    });
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
    drawGridLines(
        ctx,
        globals.frontendLocalState.timeScale,
        globals.frontendLocalState.visibleWindowTime,
        size.width,
        size.height);

    ctx.translate(TRACK_SHELL_WIDTH, 0);

    this.track.renderCanvas(ctx);
    ctx.restore();

    const localState = globals.frontendLocalState;
    // Draw vertical line when hovering on the the notes panel.
    if (localState.showNotePreview) {
      drawVerticalLineAtTime(ctx,
                             localState.timeScale,
                             localState.hoveredTimestamp,
                             size.height,
                             `#aaa`);
    }
    // Draw vertical line when shift is pressed.
    if (localState.showTimeSelectPreview) {
      drawVerticalLineAtTime(ctx,
                             localState.timeScale,
                             localState.hoveredTimestamp,
                             size.height,
                             `rgb(52,69,150)`);
    }

    if (globals.state.currentSelection !== null) {
      if (globals.state.currentSelection.kind === 'NOTE') {
        const note = globals.state.notes[globals.state.currentSelection.id];
        drawVerticalLineAtTime(ctx,
                               localState.timeScale,
                               note.timestamp,
                               size.height,
                               note.color);
      }
      if (globals.state.currentSelection.kind === 'TIMESPAN') {
        drawVerticalSelection(ctx,
                              localState.timeScale,
                              globals.state.currentSelection.startTs,
                              globals.state.currentSelection.endTs,
                              size.height,
                              `rgba(52,69,150,0.3)`);
      }
    }
  }
}
