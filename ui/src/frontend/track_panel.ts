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
import {TimeSpan} from '../base/time';
import {TrackRenderer} from '../core/track_manager';
import {raf} from '../core/raf_scheduler';
import {Track, TrackTags} from '../public/track';
import {checkerboard} from './checkerboard';
import {
  SELECTION_FILL_COLOR,
  TRACK_BORDER_COLOR,
  TRACK_SHELL_WIDTH,
} from './css_constants';
import {globals} from './globals';
import {generateTicks, TickType, getMaxMajorTicks} from './gridline_helper';
import {Size2D, VerticalBounds} from '../base/geom';
import {Panel} from './panel_container';
import {drawVerticalLineAtTime} from './vertical_line_helper';
import {classNames} from '../base/classnames';
import {Button, ButtonBar} from '../widgets/button';
import {Popup, PopupPosition} from '../widgets/popup';
import {canvasClip} from '../base/canvas_utils';
import {TimeScale} from '../base/time_scale';
import {exists, Optional} from '../base/utils';
import {Intent} from '../widgets/common';
import {TrackRenderContext} from '../public/track';
import {calculateResolution} from '../common/resolution';
import {featureFlags} from '../core/feature_flags';
import {Tree, TreeNode} from '../widgets/tree';
import {TrackNode} from '../public/workspace';
import {MiddleEllipsis} from '../widgets/middle_ellipsis';

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

function isTrackSelected(track: TrackNode) {
  const selection = globals.selectionManager.selection;
  if (selection.kind !== 'area') return false;
  return selection.trackUris.includes(track.uri);
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
      'This track has crashed',
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
  readonly title: string;
  readonly buttons: m.Children;
  readonly tags?: TrackTags;
  readonly chips?: ReadonlyArray<string>;
  readonly button?: string;
  readonly pluginId?: string;
  readonly track: TrackNode;
}

class TrackShell implements m.ClassComponent<TrackShellAttrs> {
  // Set to true when we click down and drag the
  private dragging = false;
  private dropping: 'before' | 'after' | undefined = undefined;

  view({attrs}: m.CVnode<TrackShellAttrs>) {
    // The shell should be highlighted if the current search result is inside
    // this track.
    let highlightClass = undefined;
    const searchIndex = globals.searchManager.resultIndex;
    const searchResults = globals.searchManager.searchResults;
    if (searchIndex !== -1 && searchResults !== undefined) {
      const uri = searchResults.trackUris[searchIndex];
      if (uri === attrs.track.uri) {
        highlightClass = 'flash';
      }
    }

    const currentSelection = globals.selectionManager.selection;
    const pinned = attrs.track.isPinned;
    const chips = attrs.chips && renderChips(attrs.chips);

    return m(
      `.track-shell[draggable=true]`,
      {
        className: classNames(
          highlightClass,
          this.dragging && 'drag',
          this.dropping && `drop-${this.dropping}`,
        ),
        ondragstart: (e: DragEvent) => this.ondragstart(e, attrs.track),
        ondragend: this.ondragend.bind(this),
        ondragover: this.ondragover.bind(this),
        ondragleave: this.ondragleave.bind(this),
        ondrop: (e: DragEvent) => this.ondrop(e, attrs.track),
      },
      m(
        '.track-menubar',
        m(
          'h1',
          {
            ref: attrs.title,
          },
          m('.popup', attrs.title, chips),
          m(MiddleEllipsis, {text: attrs.title}, chips),
        ),
        m(
          ButtonBar,
          {className: 'track-buttons'},
          attrs.buttons,
          SHOW_TRACK_DETAILS_BUTTON.get() &&
            this.renderTrackDetailsButton(attrs),
          m(Button, {
            className: classNames(!pinned && 'pf-visible-on-hover'),
            onclick: () => {
              pinned ? attrs.track.unpin() : attrs.track.pin();
              raf.scheduleFullRedraw();
            },
            icon: Icons.Pin,
            iconFilled: pinned,
            title: pinned ? 'Unpin' : 'Pin to top',
            compact: true,
          }),
          currentSelection.kind === 'area'
            ? m(Button, {
                onclick: (e: MouseEvent) => {
                  globals.selectionManager.toggleTrackAreaSelection(
                    attrs.track.uri,
                  );
                  e.stopPropagation();
                },
                compact: true,
                icon: isTrackSelected(attrs.track)
                  ? Icons.Checkbox
                  : Icons.BlankCheckbox,
                title: isTrackSelected(attrs.track)
                  ? 'Remove track'
                  : 'Add track to selection',
              })
            : '',
        ),
      ),
    );
  }

  ondragstart(e: DragEvent, track: TrackNode) {
    const dataTransfer = e.dataTransfer;
    if (dataTransfer === null) return;
    this.dragging = true;
    raf.scheduleFullRedraw();
    dataTransfer.setData('perfetto/track', `${track.uri}`);
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

  ondrop(e: DragEvent, track: TrackNode) {
    if (this.dropping === undefined) return;
    const dataTransfer = e.dataTransfer;
    if (dataTransfer === null) return;
    raf.scheduleFullRedraw();
    const srcId = dataTransfer.getData('perfetto/track');
    const dstId = track.uri;
    console.log(srcId, dstId);
    // globals.dispatch(Actions.moveTrack({srcId, op: this.dropping, dstId}));
    this.dropping = undefined;
  }

  private renderTrackDetailsButton(attrs: TrackShellAttrs) {
    let parent = attrs.track.parent;
    let fullPath: m.ChildArray = [attrs.track.displayName];
    while (parent && parent !== globals.workspace) {
      fullPath = [parent.displayName, ' \u2023 ', ...fullPath];
      parent = parent.parent;
    }
    return m(
      Popup,
      {
        trigger: m(Button, {
          className: 'pf-visible-on-hover',
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
            right: attrs.track.uri,
          }),
          m(TreeNode, {
            left: 'Key',
            right: attrs.track.uri,
          }),
          m(TreeNode, {left: 'Path', right: fullPath}),
          m(TreeNode, {left: 'Display Name', right: attrs.track.displayName}),
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
    return new TimeScale(timeWindow, {
      left: 0,
      right: this.getTargetContainerSize(event),
    });
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
  readonly heightPx?: number;
  readonly title: string;
  readonly buttons?: m.Children;
  readonly tags?: TrackTags;
  readonly chips?: ReadonlyArray<string>;
  readonly track?: Track;
  readonly error?: Error | undefined;
  readonly pluginId?: string;
  readonly trackNode: TrackNode;

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
        id: 'track_' + attrs.trackNode.uri,
      },
      [
        m(TrackShell, {
          buttons: [
            attrs.error && m(CrashButton, {error: attrs.error}),
            attrs.buttons,
          ],
          title: attrs.title,
          tags: attrs.tags,
          chips: attrs.chips,
          pluginId: attrs.pluginId,
          track: attrs.trackNode,
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
    if (globals.trackManager.scrollToTrackUriOnCreate === attrs.trackNode.uri) {
      vnode.dom.scrollIntoView();
      globals.trackManager.scrollToTrackUriOnCreate = undefined;
    }
    this.onupdate(vnode);

    if (attrs.revealOnCreate) {
      vnode.dom.scrollIntoView();
    }
  }

  onupdate(vnode: m.VnodeDOM<TrackComponentAttrs>) {
    vnode.attrs.track?.onFullRedraw?.();
    this.decidePopupRequired(vnode.dom);
  }

  // Works out whether to display a title popup on hover, based on whether the
  // current title is truncated.
  private decidePopupRequired(dom: Element) {
    const popupElement = dom.querySelector('.popup') as HTMLElement;
    const titleElement = dom.querySelector(
      '.pf-middle-ellipsis',
    ) as HTMLElement;

    if (popupElement.clientWidth >= titleElement.clientWidth) {
      popupElement.classList.add('show-popup');
    } else {
      popupElement.classList.remove('show-popup');
    }
  }
}

interface TrackPanelAttrs {
  readonly title: string;
  readonly tags?: TrackTags;
  readonly chips?: ReadonlyArray<string>;
  readonly trackRenderer?: TrackRenderer;
  readonly revealOnCreate?: boolean;
  readonly pluginId?: string;
  readonly track: TrackNode;
}

export class TrackPanel implements Panel {
  readonly kind = 'panel';
  readonly selectable = true;

  constructor(private readonly attrs: TrackPanelAttrs) {}

  get trackUri(): string {
    return this.attrs.track.uri;
  }

  render(): m.Children {
    const attrs = this.attrs;

    if (attrs.trackRenderer) {
      if (attrs.trackRenderer.getError()) {
        return m(TrackComponent, {
          title: attrs.title,
          error: attrs.trackRenderer.getError(),
          track: attrs.trackRenderer.track,
          chips: attrs.chips,
          pluginId: attrs.pluginId,
          trackNode: attrs.track,
        });
      }
      return m(TrackComponent, {
        title: attrs.title,
        heightPx: attrs.trackRenderer.track.getHeight(),
        buttons: attrs.trackRenderer.track.getTrackShellButtons?.(),
        tags: attrs.tags,
        track: attrs.trackRenderer.track,
        error: attrs.trackRenderer.getError(),
        revealOnCreate: attrs.revealOnCreate,
        chips: attrs.chips,
        pluginId: attrs.pluginId,
        trackNode: attrs.track,
      });
    } else {
      return m(TrackComponent, {
        title: attrs.title,
        revealOnCreate: attrs.revealOnCreate,
        chips: attrs.chips,
        pluginId: attrs.pluginId,
        trackNode: attrs.track,
      });
    }
  }

  highlightIfTrackSelected(
    ctx: CanvasRenderingContext2D,
    timescale: TimeScale,
    size: Size2D,
  ) {
    const selection = globals.selectionManager.selection;
    if (selection.kind !== 'area') {
      return;
    }
    const selectedAreaDuration = selection.end - selection.start;
    if (selection.trackUris.includes(this.attrs.track.uri)) {
      ctx.fillStyle = SELECTION_FILL_COLOR;
      ctx.fillRect(
        timescale.timeToPx(selection.start),
        0,
        timescale.durationToPx(selectedAreaDuration),
        size.height,
      );
    }
  }

  renderCanvas(ctx: CanvasRenderingContext2D, size: Size2D) {
    const trackSize = {...size, width: size.width - TRACK_SHELL_WIDTH};

    ctx.save();
    ctx.translate(TRACK_SHELL_WIDTH, 0);
    canvasClip(ctx, 0, 0, trackSize.width, trackSize.height);

    const visibleWindow = globals.timeline.visibleWindow;
    const timespan = visibleWindow.toTimeSpan();
    const timescale = new TimeScale(visibleWindow, {
      left: 0,
      right: trackSize.width,
    });
    drawGridLines(ctx, timespan, timescale, trackSize);

    const track = this.attrs.trackRenderer;

    if (track !== undefined) {
      const trackRenderCtx: TrackRenderContext = {
        trackUri: track.desc.uri,
        visibleWindow,
        size: trackSize,
        resolution: calculateResolution(visibleWindow, trackSize.width),
        ctx,
        timescale,
      };
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

  getSliceVerticalBounds(depth: number): Optional<VerticalBounds> {
    if (this.attrs.trackRenderer === undefined) {
      return undefined;
    }
    return this.attrs.trackRenderer.track.getSliceVerticalBounds?.(depth);
  }
}

export function drawGridLines(
  ctx: CanvasRenderingContext2D,
  timespan: TimeSpan,
  timescale: TimeScale,
  size: Size2D,
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
  size: Size2D,
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
  size: Size2D,
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
  size: Size2D,
) {
  const currentSelection = globals.selectionManager.legacySelection;
  const sliceDetails = globals.selectionManager.legacySelectionDetails;
  if (currentSelection !== null) {
    if (
      currentSelection.kind === 'SCHED_SLICE' &&
      exists(sliceDetails) &&
      sliceDetails.wakeupTs !== undefined
    ) {
      drawVerticalLineAtTime(
        ctx,
        timescale,
        sliceDetails.wakeupTs,
        size.height,
        `black`,
      );
    }
  }
}

export function renderNoteVerticals(
  ctx: CanvasRenderingContext2D,
  timescale: TimeScale,
  size: Size2D,
) {
  // All marked areas should have semi-transparent vertical lines
  // marking the start and end.
  for (const note of globals.noteManager.notes.values()) {
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
