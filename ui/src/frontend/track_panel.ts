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

import {currentTargetOffset} from '../base/dom_utils';
import {Icons} from '../base/semantic_icons';
import {duration, Span, time} from '../base/time';
import {Actions} from '../common/actions';
import {pluginManager} from '../common/plugins';
import {TrackState} from '../common/state';
import {raf} from '../core/raf_scheduler';
import {Migrate, Track, TrackContext} from '../public';

import {SELECTION_FILL_COLOR, TRACK_SHELL_WIDTH} from './css_constants';
import {globals} from './globals';
import {drawGridLines} from './gridline_helper';
import {Panel, PanelSize} from './panel';
import {verticalScrollToTrack} from './scroll_helper';
import {PxSpan, TimeScale} from './time_scale';
import {SliceRect} from './track';
import {
  drawVerticalLineAtTime,
} from './vertical_line_helper';

function getTitleSize(title: string): string|undefined {
  const length = title.length;
  if (length > 55) {
    return '9px';
  }
  if (length > 50) {
    return '10px';
  }
  if (length > 45) {
    return '11px';
  }
  if (length > 40) {
    return '12px';
  }
  if (length > 35) {
    return '13px';
  }
  return undefined;
}

function isPinned(id: string) {
  return globals.state.pinnedTracks.indexOf(id) !== -1;
}

function isSelected(id: string) {
  const selection = globals.state.currentSelection;
  if (selection === null || selection.kind !== 'AREA') return false;
  const selectedArea = globals.state.areas[selection.areaId];
  return selectedArea.tracks.includes(id);
}

interface TrackChipAttrs {
  text: string;
}

class TrackChip implements m.ClassComponent<TrackChipAttrs> {
  view({attrs}: m.CVnode<TrackChipAttrs>) {
    return m('span.chip', attrs.text);
  }
}

export function renderChips({uri}: TrackState) {
  const tagElements: m.Children = [];
  const trackInfo = pluginManager.resolveTrackInfo(uri);
  const tags = trackInfo?.tags;
  tags?.metric && tagElements.push(m(TrackChip, {text: 'metric'}));
  tags?.debuggable && tagElements.push(m(TrackChip, {text: 'debuggable'}));
  return tagElements;
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
      const trackKey = globals.currentSearchResults.trackKeys[searchIndex];
      if (trackKey === attrs.trackState.key) {
        highlightClass = 'flash';
      }
    }

    const dragClass = this.dragging ? `drag` : '';
    const dropClass = this.dropping ? `drop-${this.dropping}` : '';
    return m(
        `.track-shell[draggable=true]`,
        {
          class: `${highlightClass} ${dragClass} ${dropClass}`,
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
              style: {
                'font-size': getTitleSize(attrs.trackState.name),
              },
            },
            attrs.trackState.name,
            renderChips(attrs.trackState),
            ),
        m('.track-buttons',
          attrs.track.getTrackShellButtons(),
          m(TrackButton, {
            action: () => {
              globals.dispatch(
                  Actions.toggleTrackPinned({trackKey: attrs.trackState.key}));
            },
            i: Icons.Pin,
            filledIcon: isPinned(attrs.trackState.key),
            tooltip: isPinned(attrs.trackState.key) ? 'Unpin' : 'Pin to top',
            showButton: isPinned(attrs.trackState.key),
            fullHeight: true,
          }),
          globals.state.currentSelection !== null &&
                  globals.state.currentSelection.kind === 'AREA' ?
              m(TrackButton, {
                action: (e: MouseEvent) => {
                  globals.dispatch(Actions.toggleTrackSelection(
                      {id: attrs.trackState.key, isTrackGroup: false}));
                  e.stopPropagation();
                },
                i: isSelected(attrs.trackState.key) ? Icons.Checkbox :
                                                      Icons.BlankCheckbox,
                tooltip: isSelected(attrs.trackState.key) ?
                    'Remove track' :
                    'Add track to selection',
                showButton: true,
              }) :
              ''));
  }

  ondragstart(e: DragEvent) {
    const dataTransfer = e.dataTransfer;
    if (dataTransfer === null) return;
    this.dragging = true;
    raf.scheduleFullRedraw();
    dataTransfer.setData('perfetto/track', `${this.attrs!.trackState.key}`);
    dataTransfer.setDragImage(new Image(), 0, 0);
  }

  ondragend() {
    this.dragging = false;
    raf.scheduleFullRedraw();
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
    raf.scheduleFullRedraw();
  }

  ondragleave() {
    this.dropping = undefined;
    raf.scheduleFullRedraw();
  }

  ondrop(e: DragEvent) {
    if (this.dropping === undefined) return;
    const dataTransfer = e.dataTransfer;
    if (dataTransfer === null) return;
    raf.scheduleFullRedraw();
    const srcId = dataTransfer.getData('perfetto/track');
    const dstId = this.attrs!.trackState.key;
    globals.dispatch(Actions.moveTrack({srcId, op: this.dropping, dstId}));
    this.dropping = undefined;
  }
}

export interface TrackContentAttrs {
  track: Track;
}
export class TrackContent implements m.ClassComponent<TrackContentAttrs> {
  private mouseDownX?: number;
  private mouseDownY?: number;
  private selectionOccurred = false;

  view(node: m.CVnode<TrackContentAttrs>) {
    const attrs = node.attrs;
    return m(
        '.track-content',
        {
          onmousemove: (e: MouseEvent) => {
            attrs.track.onMouseMove(currentTargetOffset(e));
            raf.scheduleRedraw();
          },
          onmouseout: () => {
            attrs.track.onMouseOut();
            raf.scheduleRedraw();
          },
          onmousedown: (e: MouseEvent) => {
            const {x, y} = currentTargetOffset(e);
            this.mouseDownX = x;
            this.mouseDownY = y;
          },
          onmouseup: (e: MouseEvent) => {
            if (this.mouseDownX === undefined ||
                this.mouseDownY === undefined) {
              return;
            }
            const {x, y} = currentTargetOffset(e);
            if (Math.abs(x - this.mouseDownX) > 1 ||
                Math.abs(y - this.mouseDownY) > 1) {
              this.selectionOccurred = true;
            }
            this.mouseDownX = undefined;
            this.mouseDownY = undefined;
          },
          onclick: (e: MouseEvent) => {
            // This click event occurs after any selection mouse up/drag events
            // so we have to look if the mouse moved during this click to know
            // if a selection occurred.
            if (this.selectionOccurred) {
              this.selectionOccurred = false;
              return;
            }
            // Returns true if something was selected, so stop propagation.
            if (attrs.track.onMouseClick(currentTargetOffset(e))) {
              e.stopPropagation();
            }
            raf.scheduleRedraw();
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
    // TODO(hjd): The min height below must match the track_shell_title
    // max height in common.scss so we should read it from CSS to avoid
    // them going out of sync.
    return m(
        '.track',
        {
          style: {
            height: `${Math.max(18, attrs.track.getHeight())}px`,
          },
          id: 'track_' + attrs.trackState.key,
        },
        [
          m(TrackShell, {track: attrs.track, trackState: attrs.trackState}),
          m(TrackContent, {track: attrs.track}),
        ]);
  }

  oncreate({attrs}: m.CVnode<TrackComponentAttrs>) {
    if (globals.frontendLocalState.scrollToTrackKey === attrs.trackState.key) {
      verticalScrollToTrack(attrs.trackState.key);
      globals.frontendLocalState.scrollToTrackKey = undefined;
    }
  }
}

export interface TrackButtonAttrs {
  action: (e: MouseEvent) => void;
  i: string;
  tooltip: string;
  showButton: boolean;
  fullHeight?: boolean;
  filledIcon?: boolean;
}
export class TrackButton implements m.ClassComponent<TrackButtonAttrs> {
  view({attrs}: m.CVnode<TrackButtonAttrs>) {
    return m(
        'i.track-button',
        {
          class: [
            (attrs.showButton ? 'show' : ''),
            (attrs.fullHeight ? 'full-height' : ''),
            (attrs.filledIcon ? 'material-icons-filled' : 'material-icons'),
          ].filter(Boolean)
                     .join(' '),
          onclick: attrs.action,
          title: attrs.tooltip,
        },
        attrs.i);
  }
}

interface TrackPanelAttrs {
  trackKey: string;
  selectable: boolean;
}

export class TrackPanel extends Panel<TrackPanelAttrs> {
  // TODO(hjd): It would be nicer if these could not be undefined here.
  // We should implement a NullTrack which can be used if the trackState
  // has disappeared.
  private track: Track|undefined;
  private trackState: TrackState|undefined;

  private tryLoadTrack(vnode: m.CVnode<TrackPanelAttrs>) {
    const trackKey = vnode.attrs.trackKey;
    const trackState = globals.state.tracks[trackKey];

    if (!trackState) return;

    const {uri, params} = trackState;

    const trackCtx: TrackContext = {
      trackKey,
      mountStore: <T>(migrate: Migrate<T>) => {
        const {store, state} = globals;
        const migratedState = migrate(state.tracks[trackKey].state);
        globals.store.edit((draft) => {
          draft.tracks[trackKey].state = migratedState;
        });
        return store.createProxy<T>(['tracks', trackKey, 'state']);
      },
      params,
    };

    this.track = pluginManager.createTrack(uri, trackCtx);

    this.track?.onCreate(trackCtx);
    this.trackState = trackState;
  }

  view(vnode: m.CVnode<TrackPanelAttrs>) {
    if (!this.track) {
      this.tryLoadTrack(vnode);
    }

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
    const {visibleTimeScale} = globals.frontendLocalState;
    const selection = globals.state.currentSelection;
    const trackState = this.trackState;
    if (!selection || selection.kind !== 'AREA' || trackState === undefined) {
      return;
    }
    const selectedArea = globals.state.areas[selection.areaId];
    const selectedAreaDuration = selectedArea.end - selectedArea.start;
    if (selectedArea.tracks.includes(trackState.key)) {
      ctx.fillStyle = SELECTION_FILL_COLOR;
      ctx.fillRect(
          visibleTimeScale.timeToPx(selectedArea.start) + TRACK_SHELL_WIDTH,
          0,
          visibleTimeScale.durationToPx(selectedAreaDuration),
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

  getSliceRect(
      visibleTimeScale: TimeScale, visibleWindow: Span<time, duration>,
      windowSpan: PxSpan, tStart: time, tDur: time, depth: number): SliceRect
      |undefined {
    if (this.track === undefined) {
      return undefined;
    }
    return this.track.getSliceRect(
        visibleTimeScale, visibleWindow, windowSpan, tStart, tDur, depth);
  }
}
