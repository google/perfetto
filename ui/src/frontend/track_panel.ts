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
import {time} from '../base/time';
import {Actions} from '../common/actions';
import {TrackCacheEntry} from '../common/track_cache';
import {raf} from '../core/raf_scheduler';
import {SliceRect, Track, TrackTags} from '../public';

import {checkerboard} from './checkerboard';
import {SELECTION_FILL_COLOR, TRACK_SHELL_WIDTH} from './css_constants';
import {globals} from './globals';
import {drawGridLines} from './gridline_helper';
import {PanelSize} from './panel';
import {Panel} from './panel_container';
import {verticalScrollToTrack} from './scroll_helper';
import {
  drawVerticalLineAtTime,
} from './vertical_line_helper';
import {classNames} from '../base/classnames';
import {Button} from '../widgets/button';
import {Popup} from '../widgets/popup';

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

export function renderChips(tags?: TrackTags) {
  return [
    tags?.metric && m(TrackChip, {text: 'metric'}),
    tags?.debuggable && m(TrackChip, {text: 'debuggable'}),
  ];
}

export interface CrashButtonAttrs {
  error: Error;
}

export class CrashButton implements m.ClassComponent<CrashButtonAttrs> {
  view({attrs}: m.Vnode<CrashButtonAttrs>): m.Children {
    return m(
      Popup,
      {
        trigger: m(Button, {
          icon: Icons.Crashed,
          minimal: true,
        }),
      },
      this.renderErrorMessage(attrs.error),
    );
  }

  private renderErrorMessage(error: Error): m.Children {
    return m('',
      'This track has crashed',
      m(Button, {
        label: 'Re-raise exception',
        className: Popup.DISMISS_POPUP_GROUP_CLASS,
        onclick: () => {
          throw error;
        }},
      ),
    );
  }
}

interface TrackShellAttrs {
  trackKey: string;
  title: string;
  buttons: m.Children;
  tags?: TrackTags;
  button?: string;
}

class TrackShell implements m.ClassComponent<TrackShellAttrs> {
  // Set to true when we click down and drag the
  private dragging = false;
  private dropping: 'before'|'after'|undefined = undefined;

  view({attrs}: m.CVnode<TrackShellAttrs>) {
    // The shell should be highlighted if the current search result is inside
    // this track.
    let highlightClass = '';
    const searchIndex = globals.state.searchIndex;
    if (searchIndex !== -1) {
      const trackKey = globals.currentSearchResults.trackKeys[searchIndex];
      if (trackKey === attrs.trackKey) {
        highlightClass = 'flash';
      }
    }

    const dragClass = this.dragging ? `drag` : '';
    const dropClass = this.dropping ? `drop-${this.dropping}` : '';
    return m(
      `.track-shell[draggable=true]`,
      {
        class: `${highlightClass} ${dragClass} ${dropClass}`,
        ondragstart: (e: DragEvent) => this.ondragstart(e, attrs.trackKey),
        ondragend: this.ondragend.bind(this),
        ondragover: this.ondragover.bind(this),
        ondragleave: this.ondragleave.bind(this),
        ondrop: (e: DragEvent) => this.ondrop(e, attrs.trackKey),
      },
      m(
        'h1',
        {
          title: attrs.title,
          style: {
            'font-size': getTitleSize(attrs.title),
          },
        },
        attrs.title,
        renderChips(attrs.tags),
      ),
      m('.track-buttons',
        attrs.buttons,
        m(TrackButton, {
          action: () => {
            globals.dispatch(
              Actions.toggleTrackPinned({trackKey: attrs.trackKey}));
          },
          i: Icons.Pin,
          filledIcon: isPinned(attrs.trackKey),
          tooltip: isPinned(attrs.trackKey) ? 'Unpin' : 'Pin to top',
          showButton: isPinned(attrs.trackKey),
          fullHeight: true,
        }),
        globals.state.currentSelection !== null &&
                  globals.state.currentSelection.kind === 'AREA' ?
          m(TrackButton, {
            action: (e: MouseEvent) => {
              globals.dispatch(Actions.toggleTrackSelection(
                {id: attrs.trackKey, isTrackGroup: false}));
              e.stopPropagation();
            },
            i: isSelected(attrs.trackKey) ? Icons.Checkbox :
              Icons.BlankCheckbox,
            tooltip: isSelected(attrs.trackKey) ? 'Remove track' :
              'Add track to selection',
            showButton: true,
          }) :
          ''));
  }

  ondragstart(e: DragEvent, trackKey: string) {
    const dataTransfer = e.dataTransfer;
    if (dataTransfer === null) return;
    this.dragging = true;
    raf.scheduleFullRedraw();
    dataTransfer.setData('perfetto/track', `${trackKey}`);
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

  ondrop(e: DragEvent, trackKey: string) {
    if (this.dropping === undefined) return;
    const dataTransfer = e.dataTransfer;
    if (dataTransfer === null) return;
    raf.scheduleFullRedraw();
    const srcId = dataTransfer.getData('perfetto/track');
    const dstId = trackKey;
    globals.dispatch(Actions.moveTrack({srcId, op: this.dropping, dstId}));
    this.dropping = undefined;
  }
}

export interface TrackContentAttrs {
  track: Track;
  hasError?: boolean;
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
        className: classNames(attrs.hasError && 'pf-track-content-error'),
        onmousemove: (e: MouseEvent) => {
          attrs.track.onMouseMove?.(currentTargetOffset(e));
          raf.scheduleRedraw();
        },
        onmouseout: () => {
          attrs.track.onMouseOut?.();
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
          if (attrs.track.onMouseClick?.(currentTargetOffset(e))) {
            e.stopPropagation();
          }
          raf.scheduleRedraw();
        },
      },
      node.children);
  }
}

interface TrackComponentAttrs {
  trackKey: string;
  heightPx?: number;
  title: string;
  buttons?: m.Children;
  tags?: TrackTags;
  track?: Track;
  error?: Error | undefined;

  // Issues a scrollTo() on this DOM element at creation time. Default: false.
  revealOnCreate?: boolean;
}

class TrackComponent implements m.ClassComponent<TrackComponentAttrs> {
  view({attrs}: m.CVnode<TrackComponentAttrs>) {
    // TODO(hjd): The min height below must match the track_shell_title
    // max height in common.scss so we should read it from CSS to avoid
    // them going out of sync.
    const TRACK_HEIGHT_MIN_PX = 18;
    const TRACK_HEIGHT_DEFAULT_PX = 24;
    const trackHeightRaw = attrs.heightPx ?? TRACK_HEIGHT_DEFAULT_PX;
    const trackHeight = Math.max(trackHeightRaw, TRACK_HEIGHT_MIN_PX);

    return m(
      '.track',
      {
        style: {
          // Note: Sub-pixel track heights can mess with sticky elements.
          // Round up to the nearest integer number of pixels.
          height: `${Math.ceil(trackHeight)}px`,
        },
        id: 'track_' + attrs.trackKey,
      },
      [
        m(TrackShell, {
          buttons: [
            attrs.error && m(CrashButton, {error: attrs.error}),
            attrs.buttons,
          ],
          title: attrs.title,
          trackKey: attrs.trackKey,
          tags: attrs.tags,
        }),
        attrs.track && m(TrackContent, {
          track: attrs.track,
          hasError: Boolean(attrs.error),
        }),
      ]);
  }

  oncreate(vnode: m.VnodeDOM<TrackComponentAttrs>) {
    const {attrs} = vnode;
    if (globals.scrollToTrackKey === attrs.trackKey) {
      verticalScrollToTrack(attrs.trackKey);
      globals.scrollToTrackKey = undefined;
    }
    this.onupdate(vnode);

    if (attrs.revealOnCreate) {
      vnode.dom.scrollIntoView();
    }
  }

  onupdate(vnode: m.VnodeDOM<TrackComponentAttrs>) {
    vnode.attrs.track?.onFullRedraw?.();
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
  key: string;
  trackKey: string;
  title: string;
  tags?: TrackTags;
  trackFSM?: TrackCacheEntry;
  revealOnCreate?: boolean;
}

export class TrackPanel implements Panel {
  readonly kind = 'panel';
  readonly selectable = true;

  constructor(private readonly attrs: TrackPanelAttrs) {}

  get key(): string {
    return this.attrs.key;
  }

  get trackKey(): string {
    return this.attrs.trackKey;
  }

  get mithril(): m.Children {
    const attrs = this.attrs;

    if (attrs.trackFSM) {
      if (attrs.trackFSM.getError()) {
        return m(TrackComponent, {
          title: attrs.title,
          trackKey: attrs.trackKey,
          error: attrs.trackFSM.getError(),
          track: attrs.trackFSM.track,
        });
      }
      return m(TrackComponent, {
        key: attrs.key,
        trackKey: attrs.trackKey,
        title: attrs.title,
        heightPx: attrs.trackFSM.track.getHeight(),
        buttons: attrs.trackFSM.track.getTrackShellButtons?.(),
        tags: attrs.tags,
        track: attrs.trackFSM.track,
        error: attrs.trackFSM.getError(),
        revealOnCreate: attrs.revealOnCreate,
      });
    } else {
      return m(TrackComponent, {
        key: attrs.key,
        trackKey: attrs.trackKey,
        title: attrs.title,
        revealOnCreate: attrs.revealOnCreate,
      });
    }
  }

  highlightIfTrackSelected(ctx: CanvasRenderingContext2D, size: PanelSize) {
    const {visibleTimeScale} = globals.timeline;
    const selection = globals.state.currentSelection;
    if (!selection || selection.kind !== 'AREA') {
      return;
    }
    const selectedArea = globals.state.areas[selection.areaId];
    const selectedAreaDuration = selectedArea.end - selectedArea.start;
    if (selectedArea.tracks.includes(this.attrs.trackKey)) {
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

    const track = this.attrs.trackFSM;

    ctx.translate(TRACK_SHELL_WIDTH, 0);
    if (track !== undefined) {
      const trackSize = {...size, width: size.width - TRACK_SHELL_WIDTH};
      if (!track.getError()) {
        track.update();
        track.track.render(ctx, trackSize);
      }
    } else {
      checkerboard(ctx, size.height, 0, size.width - TRACK_SHELL_WIDTH);
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

  getSliceRect(tStart: time, tDur: time, depth: number): SliceRect|undefined {
    if (this.attrs.trackFSM === undefined) {
      return undefined;
    }
    return this.attrs.trackFSM.track.getSliceRect?.(tStart, tDur, depth);
  }
}
