// Copyright (C) 2024 The Android Open Source Project
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
import {classNames} from '../base/classnames';
import {currentTargetOffset} from '../base/dom_utils';
import {Bounds2D, Point2D, Vector2D} from '../base/geom';
import {Icons} from '../base/semantic_icons';
import {ButtonBar} from './button';
import {Chip, ChipBar} from './chip';
import {Icon} from './icon';
import {MiddleEllipsis} from './middle_ellipsis';
import {clamp} from '../base/math_utils';

/**
 * The TrackWidget defines the look and style of a track.
 *
 * ┌──────────────────────────────────────────────────────────────────┐
 * │pf-track (grid)                                                   │
 * │┌─────────────────────────────────────────┐┌─────────────────────┐│
 * ││pf-track-shell                           ││pf-track-content     ││
 * ││┌───────────────────────────────────────┐││                     ││
 * │││pf-track-menubar (sticky)              │││                     ││
 * │││┌───────────────┐┌────────────────────┐│││                     ││
 * ││││pf-track-title ││pf-track-buttons    ││││                     ││
 * │││└───────────────┘└────────────────────┘│││                     ││
 * ││└───────────────────────────────────────┘││                     ││
 * │└─────────────────────────────────────────┘└─────────────────────┘│
 * └──────────────────────────────────────────────────────────────────┘
 */

export interface TrackComponentAttrs {
  // The title of this track.
  readonly title: string;

  // The full path to this track.
  readonly path?: string;

  // Show dropdown arrow and make clickable. Defaults to false.
  readonly collapsible?: boolean;

  // Show an up or down dropdown arrow.
  readonly collapsed: boolean;

  // Height of the track in pixels. All tracks have a fixed height.
  readonly heightPx: number;

  // Optional buttons to place on the RHS of the track shell.
  readonly buttons?: m.Children;

  // Optional list of chips to display after the track title.
  readonly chips?: ReadonlyArray<string>;

  // Render this track in error colours.
  readonly error?: boolean;

  // The integer indentation level of this track. If omitted, defaults to 0.
  readonly indentationLevel?: number;

  // Track titles are sticky. This is the offset in pixels from the top of the
  // scrolling parent. Defaults to 0.
  readonly topOffsetPx?: number;

  // Issues a scrollTo() on this DOM element at creation time. Default: false.
  readonly revealOnCreate?: boolean;

  // Called when arrow clicked.
  readonly onToggleCollapsed?: () => void;

  // Style the component differently if it has children.
  readonly isSummary?: boolean;

  // HTML id applied to the root element.
  readonly id: string;

  // Whether to highlight the track or not.
  readonly highlight?: boolean;

  // Whether the shell should be draggable and emit drag/drop events.
  readonly reorderable?: boolean;

  // Mouse events.
  readonly onTrackContentMouseMove?: (
    pos: Point2D,
    contentSize: Bounds2D,
  ) => void;
  readonly onTrackContentMouseOut?: () => void;
  readonly onTrackContentClick?: (
    pos: Point2D,
    contentSize: Bounds2D,
  ) => boolean;

  // If reorderable, these functions will be called when track shells are
  // dragged and dropped.
  readonly onMoveBefore?: (nodeId: string) => void;
  readonly onMoveAfter?: (nodeId: string) => void;
}

const TRACK_HEIGHT_MIN_PX = 18;
const INDENTATION_LEVEL_MAX = 16;

export class TrackWidget implements m.ClassComponent<TrackComponentAttrs> {
  view({attrs}: m.CVnode<TrackComponentAttrs>) {
    const {
      indentationLevel = 0,
      collapsible,
      collapsed,
      highlight,
      heightPx,
      id,
      isSummary,
    } = attrs;

    const trackHeight = Math.max(heightPx, TRACK_HEIGHT_MIN_PX);
    const expanded = collapsible && !collapsed;

    return m(
      '.pf-track',
      {
        id,
        className: classNames(
          expanded && 'pf-expanded',
          highlight && 'pf-highlight',
          isSummary && 'pf-is-summary',
        ),
        style: {
          // Note: Sub-pixel track heights can mess with sticky elements.
          // Round up to the nearest integer number of pixels.
          '--indent': clamp(indentationLevel, 0, INDENTATION_LEVEL_MAX),
          'height': `${Math.ceil(trackHeight)}px`,
        },
      },
      this.renderShell(attrs),
      this.renderContent(attrs),
    );
  }

  oncreate(vnode: m.VnodeDOM<TrackComponentAttrs>) {
    this.onupdate(vnode);

    if (vnode.attrs.revealOnCreate) {
      vnode.dom.scrollIntoView({behavior: 'smooth', block: 'nearest'});
    }
  }

  onupdate(vnode: m.VnodeDOM<TrackComponentAttrs>) {
    this.decidePopupRequired(vnode.dom);
  }

  // Works out whether to display a title popup on hover, based on whether the
  // current title is truncated.
  private decidePopupRequired(dom: Element) {
    const popupTitleElement = dom.querySelector(
      '.pf-track-title-popup',
    ) as HTMLElement;
    const truncatedTitleElement = dom.querySelector(
      '.pf-middle-ellipsis',
    ) as HTMLElement;

    if (popupTitleElement.clientWidth > truncatedTitleElement.clientWidth) {
      popupTitleElement.classList.add('pf-visible');
    } else {
      popupTitleElement.classList.remove('pf-visible');
    }
  }

  private renderShell(attrs: TrackComponentAttrs): m.Children {
    const chips =
      attrs.chips &&
      m(
        ChipBar,
        attrs.chips.map((chip) =>
          m(Chip, {label: chip, compact: true, rounded: true}),
        ),
      );

    const {
      id,
      topOffsetPx = 0,
      collapsible,
      collapsed,
      reorderable = false,
      onMoveAfter = () => {},
      onMoveBefore = () => {},
    } = attrs;

    return m(
      `.pf-track-shell[data-track-node-id=${id}]`,
      {
        className: classNames(collapsible && 'pf-clickable'),
        onclick: (e: MouseEvent) => {
          // Block all clicks on the shell from propagating through to the
          // canvas
          e.stopPropagation();
          if (collapsible) {
            attrs.onToggleCollapsed?.();
          }
        },
        draggable: reorderable,
        ondragstart: (e: DragEvent) => {
          e.dataTransfer?.setData('text/plain', id);
        },
        ondragover: (e: DragEvent) => {
          if (!reorderable) {
            return;
          }
          const target = e.currentTarget as HTMLElement;
          const threshold = target.offsetHeight / 2;
          if (e.offsetY > threshold) {
            target.classList.remove('pf-drag-before');
            target.classList.add('pf-drag-after');
          } else {
            target.classList.remove('pf-drag-after');
            target.classList.add('pf-drag-before');
          }
        },
        ondragleave: (e: DragEvent) => {
          if (!reorderable) {
            return;
          }
          const target = e.currentTarget as HTMLElement;
          const related = e.relatedTarget as HTMLElement | null;
          if (related && !target.contains(related)) {
            target.classList.remove('pf-drag-after');
            target.classList.remove('pf-drag-before');
          }
        },
        ondrop: (e: DragEvent) => {
          if (!reorderable) {
            return;
          }
          const id = e.dataTransfer?.getData('text/plain');
          const target = e.currentTarget as HTMLElement;
          const threshold = target.offsetHeight / 2;
          if (id !== undefined) {
            if (e.offsetY > threshold) {
              onMoveAfter(id);
            } else {
              onMoveBefore(id);
            }
          }
          target.classList.remove('pf-drag-after');
          target.classList.remove('pf-drag-before');
        },
      },
      m(
        '.pf-track-menubar',
        {
          style: {
            position: 'sticky',
            top: `${topOffsetPx}px`,
          },
        },
        m(
          'h1.pf-track-title',
          {
            ref: attrs.path, // TODO(stevegolton): Replace with aria tags?
          },
          collapsible &&
            m(Icon, {icon: collapsed ? Icons.ExpandDown : Icons.ExpandUp}),
          m(
            MiddleEllipsis,
            {text: attrs.title},
            m('.pf-track-title-popup', attrs.title),
          ),
          chips,
        ),
        m(
          ButtonBar,
          {
            className: 'pf-track-buttons',
            // Block button clicks from hitting the shell's on click event
            onclick: (e: MouseEvent) => e.stopPropagation(),
          },
          attrs.buttons,
        ),
      ),
    );
  }

  private mouseDownPos?: Vector2D;
  private selectionOccurred = false;

  private renderContent(attrs: TrackComponentAttrs): m.Children {
    const {
      heightPx,
      onTrackContentMouseMove,
      onTrackContentMouseOut,
      onTrackContentClick,
    } = attrs;
    const trackHeight = Math.max(heightPx, TRACK_HEIGHT_MIN_PX);

    return m('.pf-track-content', {
      style: {
        height: `${trackHeight}px`,
      },
      className: classNames(attrs.error && 'pf-track-content-error'),
      onmousemove: (e: MouseEvent) => {
        onTrackContentMouseMove?.(
          currentTargetOffset(e),
          getTargetContainerSize(e),
        );
      },
      onmouseout: () => {
        onTrackContentMouseOut?.();
      },
      onmousedown: (e: MouseEvent) => {
        this.mouseDownPos = currentTargetOffset(e);
      },
      onmouseup: (e: MouseEvent) => {
        if (!this.mouseDownPos) return;
        if (
          this.mouseDownPos.sub(currentTargetOffset(e)).manhattanDistance > 1
        ) {
          this.selectionOccurred = true;
        }
        this.mouseDownPos = undefined;
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
        if (
          onTrackContentClick?.(
            currentTargetOffset(e),
            getTargetContainerSize(e),
          )
        ) {
          e.stopPropagation();
        }
      },
    });
  }
}

function getTargetContainerSize(event: MouseEvent): Bounds2D {
  const target = event.target as HTMLElement;
  return target.getBoundingClientRect();
}
