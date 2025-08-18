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
import {TimeSpan, time} from '../base/time';
import {Actions} from '../common/actions';
import {TrackCacheEntry} from '../common/track_cache';
import {raf} from '../core/raf_scheduler';
import {SliceRect, Track, TrackTags} from '../public';

import {checkerboard} from './checkerboard';
import {
  SELECTION_FILL_COLOR,
  TRACK_BORDER_COLOR,
  TRACK_SHELL_WIDTH,
} from './css_constants';
import {globals} from './globals';
import {generateTicks, TickType, getMaxMajorTicks} from './gridline_helper';
import {Size} from '../base/geom';
import {Panel} from './panel_container';
import {verticalScrollToTrack} from './scroll_helper';
import {drawVerticalLineAtTime} from './vertical_line_helper';
import {classNames} from '../base/classnames';
import {Button, ButtonBar} from '../widgets/button';
import {Popup, PopupPosition} from '../widgets/popup';
import {canvasClip} from '../common/canvas_utils';
import {PxSpan, TimeScale} from './time_scale';
import {getLegacySelection} from '../common/state';
import {CloseTrackButton} from './close_track_button';
import {exists} from '../base/utils';
import {Intent} from '../widgets/common';
import {TrackRenderContext} from '../public/tracks';
import {calculateResolution} from '../common/resolution';
import {featureFlags} from '../core/feature_flags';
import {Tree, TreeNode} from '../widgets/tree';

export const SHOW_TRACK_DETAILS_BUTTON = featureFlags.register({
  id: 'showTrackDetailsButton',
  name: 'Show track details button',
  description: 'Show track details button in track shells.',
  defaultValue: false,
});

export function getTitleFontSize(title: string): string | undefined {
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

function isTrackPinned(trackKey: string) {
  return globals.state.pinnedTracks.indexOf(trackKey) !== -1;
}

function isTrackSelected(trackKey: string) {
  const selection = globals.state.selection;
  if (selection.kind !== 'area') return false;
  return selection.tracks.includes(trackKey);
}

interface TrackChipAttrs {
  text: string;
}

class TrackChip implements m.ClassComponent<TrackChipAttrs> {
  view({attrs}: m.CVnode<TrackChipAttrs>) {
    return m('span.chip', attrs.text);
  }
}

export function renderChips(chips: ReadonlyArray<string>) {
  return chips.map((chip) => m(TrackChip, {text: chip}));
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
          compact: true,
        }),
      },
      this.renderErrorMessage(attrs.error),
    );
  }

  private renderErrorMessage(error: Error): m.Children {
    return m(
      '',
      'This track has crashed (possibly due to a long period of inactivity), consider reloading the page',
      m(Button, {
        label: 'Re-raise exception',
        intent: Intent.Primary,
        className: Popup.DISMISS_POPUP_GROUP_CLASS,
        onclick: () => {
          throw error;
        },
      }),
    );
  }
}

interface TrackShellAttrs {
  readonly trackKey: string;
  readonly title: m.Children;
  readonly buttons: m.Children;
  readonly tags?: TrackTags;
  readonly chips?: ReadonlyArray<string>;
  readonly button?: string;
  readonly pluginId?: string;
}

class TrackShell implements m.ClassComponent<TrackShellAttrs> {
  // Set to true when we click down and drag the
  private dragging = false;
  private dropping: 'before' | 'after' | undefined = undefined;

  view({attrs}: m.CVnode<TrackShellAttrs>) {
    // The shell should be highlighted if the current search result is inside
    // this track.
    let highlightClass = undefined;
    const searchIndex = globals.state.searchIndex;
    if (searchIndex !== -1) {
      const trackKey = globals.currentSearchResults.trackKeys[searchIndex];
      if (trackKey === attrs.trackKey) {
        highlightClass = 'flash';
      }
    }

    const currentSelection = globals.state.selection;
    const pinned = isTrackPinned(attrs.trackKey);

    return m(
      `.track-shell[draggable=true]`,
      {
        className: classNames(
          highlightClass,
          this.dragging && 'drag',
          this.dropping && `drop-${this.dropping}`,
        ),
        ondragstart: (e: DragEvent) => this.ondragstart(e, attrs.trackKey),
        ondragend: this.ondragend.bind(this),
        ondragover: this.ondragover.bind(this),
        ondragleave: this.ondragleave.bind(this),
        ondrop: (e: DragEvent) => this.ondrop(e, attrs.trackKey),
      },
      m(
        '.track-menubar',
        m(
          'h1',
          {
            title: attrs.title,
          },
          attrs.title,
          attrs.chips && renderChips(attrs.chips),
        ),
        m(
          ButtonBar,
          {className: 'track-buttons'},
          attrs.buttons,
          SHOW_TRACK_DETAILS_BUTTON.get() &&
            this.renderTrackDetailsButton(pinned, attrs),
          m(Button, {
            className: classNames(!pinned && 'pf-visible-on-hover'),
            onclick: () => {
              globals.dispatch(
                Actions.toggleTrackPinned({trackKey: attrs.trackKey}),
              );
            },
            icon: Icons.Pin,
            iconFilled: pinned,
            title: pinned ? 'Unpin' : 'Pin to top',
            compact: true,
          }),
          currentSelection.kind === 'area'
            ? m(Button, {
                onclick: (e: MouseEvent) => {
                  globals.dispatch(
                    Actions.toggleTrackSelection({
                      key: attrs.trackKey,
                      isTrackGroup: false,
                    }),
                  );
                  e.stopPropagation();
                },
                compact: true,
                icon: isTrackSelected(attrs.trackKey)
                  ? Icons.Checkbox
                  : Icons.BlankCheckbox,
                title: isTrackSelected(attrs.trackKey)
                  ? 'Remove track'
                  : 'Add track to selection',
              })
            : '',
        ),
      ),
    );
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
    } else if (e.offsetY > (e.target.scrollHeight / 3) * 2) {
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

  private renderTrackDetailsButton(pinned: boolean, attrs: TrackShellAttrs) {
    return m(
      Popup,
      {
        trigger: m(Button, {
          className: classNames(!pinned && 'pf-visible-on-hover'),
          icon: 'info',
          title: 'Show track details',
          compact: true,
        }),
        position: PopupPosition.RightStart,
      },
      m(
        '.pf-track-details-dropdown',
        m(
          Tree,
          m(TreeNode, {
            left: 'URI',
            right: globals.state.tracks[attrs.trackKey]?.uri,
          }),
          m(TreeNode, {left: 'Title', right: attrs.title}),
          m(TreeNode, {left: 'Track Key', right: attrs.trackKey}),
          m(TreeNode, {left: 'Plugin ID', right: attrs.pluginId}),
          m(
            TreeNode,
            {left: 'Tags'},
            attrs.tags &&
              Object.entries(attrs.tags).map(([key, value]) => {
                return m(TreeNode, {left: key, right: value?.toString()});
              }),
          ),
        ),
      ),
    );
  }
}

export interface TrackContentAttrs {
  track: Track;
  hasError?: boolean;
  height?: number;
}
export class TrackContent implements m.ClassComponent<TrackContentAttrs> {
  private mouseDownX?: number;
  private mouseDownY?: number;
  private selectionOccurred = false;

  private getTargetContainerSize(event: MouseEvent): number {
    const target = event.target as HTMLElement;
    return target.getBoundingClientRect().width;
  }

  private getTargetTimeScale(event: MouseEvent): TimeScale {
    const timeWindow = globals.timeline.visibleWindow;
    return new TimeScale(
      timeWindow,
      new PxSpan(0, this.getTargetContainerSize(event)),
    );
  }

  view(node: m.CVnode<TrackContentAttrs>) {
    const attrs = node.attrs;
    return m(
      '.track-content',
      {
        style: exists(attrs.height) && {
          height: `${attrs.height}px`,
        },
        className: classNames(attrs.hasError && 'pf-track-content-error'),
        onmousemove: (e: MouseEvent) => {
          const {x, y} = currentTargetOffset(e);
          attrs.track.onMouseMove?.({
            x,
            y,
            timescale: this.getTargetTimeScale(e),
          });
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
          if (this.mouseDownX === undefined || this.mouseDownY === undefined) {
            return;
          }
          const {x, y} = currentTargetOffset(e);
          if (
            Math.abs(x - this.mouseDownX) > 1 ||
            Math.abs(y - this.mouseDownY) > 1
          ) {
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
          const {x, y} = currentTargetOffset(e);
          if (
            attrs.track.onMouseClick?.({
              x,
              y,
              timescale: this.getTargetTimeScale(e),
            })
          ) {
            e.stopPropagation();
          }
          raf.scheduleRedraw();
        },
      },
      node.children,
    );
  }
}

interface TrackComponentAttrs {
  readonly trackKey: string;
  readonly heightPx?: number;
  readonly title: m.Children;
  readonly buttons?: m.Children;
  readonly tags?: TrackTags;
  readonly chips?: ReadonlyArray<string>;
  readonly track?: Track;
  readonly error?: Error | undefined;
  readonly closeable: boolean;
  readonly pluginId?: string;

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
            attrs.closeable && m(CloseTrackButton, {trackKey: attrs.trackKey}),
            attrs.buttons,
          ],
          title: attrs.title,
          trackKey: attrs.trackKey,
          tags: attrs.tags,
          chips: attrs.chips,
          pluginId: attrs.pluginId,
        }),
        attrs.track &&
          m(TrackContent, {
            track: attrs.track,
            hasError: Boolean(attrs.error),
            height: attrs.heightPx,
          }),
      ],
    );
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

interface TrackPanelAttrs {
  readonly trackKey: string;
  readonly title: m.Children;
  readonly tags?: TrackTags;
  readonly chips?: ReadonlyArray<string>;
  readonly trackFSM?: TrackCacheEntry;
  readonly revealOnCreate?: boolean;
  readonly closeable: boolean;
  readonly pluginId?: string;
}

export class TrackPanel implements Panel {
  readonly kind = 'panel';
  readonly selectable = true;
  private previousTrackContext?: TrackRenderContext;

  constructor(private readonly attrs: TrackPanelAttrs) {}

  get trackKey(): string {
    return this.attrs.trackKey;
  }

  render(): m.Children {
    const attrs = this.attrs;

    if (attrs.trackFSM) {
      if (attrs.trackFSM.getError()) {
        return m(TrackComponent, {
          title: attrs.title,
          trackKey: attrs.trackKey,
          error: attrs.trackFSM.getError(),
          track: attrs.trackFSM.track,
          closeable: attrs.closeable,
          chips: attrs.chips,
          pluginId: attrs.pluginId,
        });
      }
      return m(TrackComponent, {
        trackKey: attrs.trackKey,
        title: attrs.title,
        heightPx: attrs.trackFSM.track.getHeight(),
        buttons: attrs.trackFSM.track.getTrackShellButtons?.(),
        tags: attrs.tags,
        track: attrs.trackFSM.track,
        error: attrs.trackFSM.getError(),
        revealOnCreate: attrs.revealOnCreate,
        closeable: attrs.closeable,
        chips: attrs.chips,
        pluginId: attrs.pluginId,
      });
    } else {
      return m(TrackComponent, {
        trackKey: attrs.trackKey,
        title: attrs.title,
        revealOnCreate: attrs.revealOnCreate,
        closeable: attrs.closeable,
        chips: attrs.chips,
        pluginId: attrs.pluginId,
      });
    }
  }

  highlightIfTrackSelected(
    ctx: CanvasRenderingContext2D,
    timescale: TimeScale,
    size: Size,
  ) {
    const selection = globals.state.selection;
    if (selection.kind !== 'area') {
      return;
    }
    const selectedAreaDuration = selection.end - selection.start;
    if (selection.tracks.includes(this.attrs.trackKey)) {
      ctx.fillStyle = SELECTION_FILL_COLOR;
      ctx.fillRect(
        timescale.timeToPx(selection.start),
        0,
        timescale.durationToPx(selectedAreaDuration),
        size.height,
      );
    }
  }

  renderCanvas(ctx: CanvasRenderingContext2D, size: Size) {
    const trackSize = {...size, width: size.width - TRACK_SHELL_WIDTH};

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

    const track = this.attrs.trackFSM;

    if (track !== undefined) {
      const trackRenderCtx: TrackRenderContext = {
        trackKey: track.trackKey,
        visibleWindow,
        size: trackSize,
        resolution: calculateResolution(visibleWindow, trackSize.width),
        ctx,
        timescale,
      };
      this.previousTrackContext = trackRenderCtx;
      if (!track.getError()) {
        track.render(trackRenderCtx);
      }
    } else {
      checkerboard(ctx, trackSize.height, 0, trackSize.width);
    }

    this.highlightIfTrackSelected(ctx, timescale, trackSize);

    // Draw vertical line when hovering on the notes panel.
    renderHoveredNoteVertical(ctx, timescale, trackSize);
    renderHoveredCursorVertical(ctx, timescale, trackSize);
    renderWakeupVertical(ctx, timescale, trackSize);
    renderNoteVerticals(ctx, timescale, trackSize);

    ctx.restore();
  }

  getSliceRect(tStart: time, tDur: time, depth: number): SliceRect | undefined {
    if (
      this.attrs.trackFSM === undefined ||
      this.previousTrackContext === undefined
    ) {
      return undefined;
    }
    return this.attrs.trackFSM.track.getSliceRect?.(
      this.previousTrackContext,
      tStart,
      tDur,
      depth,
    );
  }
}

export function drawGridLines(
  ctx: CanvasRenderingContext2D,
  timespan: TimeSpan,
  timescale: TimeScale,
  size: Size,
): void {
  ctx.strokeStyle = TRACK_BORDER_COLOR;
  ctx.lineWidth = 1;

  if (size.width > 0 && timespan.duration > 0n) {
    const maxMajorTicks = getMaxMajorTicks(size.width);
    const offset = globals.timestampOffset();
    for (const {type, time} of generateTicks(timespan, maxMajorTicks, offset)) {
      const px = Math.floor(timescale.timeToPx(time));
      if (type === TickType.MAJOR) {
        ctx.beginPath();
        ctx.moveTo(px + 0.5, 0);
        ctx.lineTo(px + 0.5, size.height);
        ctx.stroke();
      }
    }
  }
}

export function renderHoveredCursorVertical(
  ctx: CanvasRenderingContext2D,
  timescale: TimeScale,
  size: Size,
) {
  if (globals.state.hoverCursorTimestamp !== -1n) {
    drawVerticalLineAtTime(
      ctx,
      timescale,
      globals.state.hoverCursorTimestamp,
      size.height,
      `#344596`,
    );
  }
}

export function renderHoveredNoteVertical(
  ctx: CanvasRenderingContext2D,
  timescale: TimeScale,
  size: Size,
) {
  if (globals.state.hoveredNoteTimestamp !== -1n) {
    drawVerticalLineAtTime(
      ctx,
      timescale,
      globals.state.hoveredNoteTimestamp,
      size.height,
      `#aaa`,
    );
  }
}

export function renderWakeupVertical(
  ctx: CanvasRenderingContext2D,
  timescale: TimeScale,
  size: Size,
) {
  const currentSelection = getLegacySelection(globals.state);
  if (currentSelection !== null) {
    if (
      currentSelection.kind === 'SCHED_SLICE' &&
      globals.sliceDetails.wakeupTs !== undefined
    ) {
      drawVerticalLineAtTime(
        ctx,
        timescale,
        globals.sliceDetails.wakeupTs,
        size.height,
        `black`,
      );
    }
  }
}

export function renderNoteVerticals(
  ctx: CanvasRenderingContext2D,
  timescale: TimeScale,
  size: Size,
) {
  // All marked areas should have semi-transparent vertical lines
  // marking the start and end.
  for (const note of Object.values(globals.state.notes)) {
    if (note.noteType === 'SPAN') {
      const transparentNoteColor =
        'rgba(' + hex.rgb(note.color.substr(1)).toString() + ', 0.65)';
      drawVerticalLineAtTime(
        ctx,
        timescale,
        note.start,
        size.height,
        transparentNoteColor,
        1,
      );
      drawVerticalLineAtTime(
        ctx,
        timescale,
        note.end,
        size.height,
        transparentNoteColor,
        1,
      );
    } else if (note.noteType === 'DEFAULT') {
      drawVerticalLineAtTime(
        ctx,
        timescale,
        note.timestamp,
        size.height,
        note.color,
      );
    }
  }
}
