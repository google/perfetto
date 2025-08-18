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
import {DisposableStack} from '../base/disposable_stack';
import {currentTargetOffset} from '../base/dom_utils';
import {Bounds2D, Point2D, Vector2D} from '../base/geom';
import {assertExists} from '../base/logging';
import {clamp} from '../base/math_utils';
import {hasChildren, MithrilEvent} from '../base/mithril_utils';
import {Icons} from '../base/semantic_icons';
import {Button, ButtonBar, ButtonVariant} from './button';
import {Chip} from './chip';
import {HTMLAttrs, Intent} from './common';
import {MiddleEllipsis} from './middle_ellipsis';
import {Popup} from './popup';
import {Stack} from './stack';

/**
 * This component defines the look and style of the DOM parts of a track (mainly
 * the 'shell' part).
 *
 * ┌───────────────────────────────────────────────────────────────────────────┐
 * │ pf-track                                                                  │
 * |┌─────────────────────────────────────────────────────────────────────────┐|
 * || pf-track__header                                                        ||
 * │|┌─────────┐┌─────────────────────────────────────────┐┌─────────────────┐│|
 * │|│::before ||pf-track__shell                          ││pf-track__content││|
 * │|│(Indent) ||┌───────────────────────────────────────┐││                 ││|
 * │|│         ||│pf-track__menubar (sticky)             │││                 ││|
 * │|│         ||│┌───────────────┐┌────────────────────┐│││                 ││|
 * │|│         ||││pf-track__title││pf-track__buttons   ││││                 ││|
 * │|│         ||│└───────────────┘└────────────────────┘│││                 ││|
 * │|│         ||└───────────────────────────────────────┘││                 ││|
 * │|└─────────┘└─────────────────────────────────────────┘└─────────────────┘│|
 * |└─────────────────────────────────────────────────────────────────────────┘|
 * |┌─────────────────────────────────────────────────────────────────────────┐|
 * || pf-track__children (if children supplied)                               ||
 * |└─────────────────────────────────────────────────────────────────────────┘|
 * └───────────────────────────────────────────────────────────────────────────┘
 */

export interface TrackShellAttrs extends HTMLAttrs {
  // The title of this track.
  readonly title: string;

  // Optional subtitle to display underneath the track name.
  readonly subtitle?: string;

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
  readonly error?: Error;

  // Issues a scrollTo() on this DOM element at creation time. Default: false.
  readonly scrollToOnCreate?: boolean;

  // Style the component differently.
  readonly summary?: boolean;

  // Whether to highlight the track or not.
  readonly highlight?: boolean;

  // Whether the shell should be draggable and emit drag/drop events.
  readonly reorderable?: boolean;

  // This is the depth of the track in the tree - controls the indent level and
  // the z-index of sticky headers.
  readonly depth?: number;

  // The stick top offset - this is the offset from the top of sticky summary
  // track headers and sticky menu bars stick from the top of the viewport. This
  // is used to allow nested sticky track headers and menubars of nested tracks
  // to stick below the sticky header of their parent track(s).
  readonly stickyTop?: number;

  // The ID of the plugin that created this track.
  readonly pluginId?: string;

  // Render a lighter version of the track shell, with no buttons or chips, just
  // the track title.
  readonly lite?: boolean;

  // Called when the track is expanded or collapsed (when the node is clicked).
  onCollapsedChanged?(collapsed: boolean): void;

  // Mouse events within the track content element.
  onTrackContentMouseMove?(pos: Point2D, contentSize: Bounds2D): void;
  onTrackContentMouseOut?(): void;
  onTrackContentClick?(pos: Point2D, contentSize: Bounds2D): boolean;

  // If reorderable, these functions will be called when track shells are
  // dragged and dropped.
  onMoveBefore?(nodeId: string): void;
  onMoveInside?(nodeId: string): void;
  onMoveAfter?(nodeId: string): void;
}

export class TrackShell implements m.ClassComponent<TrackShellAttrs> {
  private mouseDownPos?: Vector2D;
  private selectionOccurred = false;
  private scrollIntoView = false;

  view(vnode: m.CVnode<TrackShellAttrs>) {
    const {attrs} = vnode;

    const {
      collapsible,
      collapsed,
      id,
      summary,
      heightPx,
      ref,
      depth = 0,
      stickyTop = 0,
      lite,
    } = attrs;

    const expanded = collapsible && !collapsed;
    const trackHeight = heightPx;

    return m(
      '.pf-track',
      {
        id,
        style: {
          '--height': trackHeight,
          '--depth': clamp(depth, 0, 16),
          '--sticky-top': Math.max(0, stickyTop),
        },
        ref,
      },
      m(
        '.pf-track__header',
        {
          className: classNames(
            summary && 'pf-track__header--summary',
            expanded && 'pf-track__header--expanded',
            summary && expanded && 'pf-track__header--expanded--summary',
          ),
        },
        this.renderShell(attrs),
        !lite && this.renderContent(attrs),
      ),
      hasChildren(vnode) && m('.pf-track__children', vnode.children),
    );
  }

  oncreate({dom, attrs}: m.VnodeDOM<TrackShellAttrs>) {
    if (attrs.scrollToOnCreate) {
      dom.scrollIntoView({behavior: 'smooth', block: 'nearest'});
    }
  }

  onupdate({dom}: m.VnodeDOM<TrackShellAttrs, this>) {
    if (this.scrollIntoView) {
      dom.scrollIntoView({behavior: 'instant', block: 'nearest'});
      this.scrollIntoView = false;
    }
  }

  private renderShell(attrs: TrackShellAttrs): m.Children {
    const {
      id,
      chips,
      collapsible,
      collapsed,
      reorderable = false,
      onMoveAfter = () => {},
      onMoveBefore = () => {},
      onMoveInside = () => {},
      buttons,
      highlight,
      lite,
      summary,
    } = attrs;

    const block = 'pf-track';
    const blockElement = `${block}__shell`;
    const dragBeforeClassName = `${blockElement}--drag-before`;
    const dragInsideClassName = `${blockElement}--drag-inside`;
    const dragAfterClassName = `${blockElement}--drag-after`;

    function updateDragClassname(target: HTMLElement, className: string) {
      // This is a bit brute-force, but gets the job done without triggering a
      // full mithril redraw every frame while dragging...
      target.classList.remove(dragBeforeClassName);
      target.classList.remove(dragAfterClassName);
      target.classList.remove(dragInsideClassName);
      target.classList.add(className);
    }

    return m(
      `.pf-track__shell`,
      {
        className: classNames(
          collapsible && 'pf-track__shell--clickable',
          highlight && 'pf-track__shell--highlight',
        ),
        onclick: () => {
          collapsible && attrs.onCollapsedChanged?.(!collapsed);
          if (!collapsed) {
            this.scrollIntoView = true;
          }
        },
        draggable: reorderable,
        ondragstart: (e: DragEvent) => {
          id && e.dataTransfer?.setData('text/plain', id);
        },
        ondragover: (e: DragEvent) => {
          if (!reorderable) {
            return;
          }
          const target = e.currentTarget as HTMLElement;
          const position = currentTargetOffset(e);
          if (summary) {
            // For summary tracks, split the track into thirds, so it's
            // possible to insert above, below and into.
            const threshold = target.offsetHeight / 3;
            if (position.y < threshold) {
              // Hovering on the upper third, move before this node.
              updateDragClassname(target, dragBeforeClassName);
            } else if (position.y < threshold * 2) {
              // Hovering in the middle, move inside this node.
              updateDragClassname(target, dragInsideClassName);
            } else {
              // Hovering on the lower third, move after this node.
              updateDragClassname(target, dragAfterClassName);
            }
          } else {
            // For non-summary tracks, split the track in half, as it's only
            // possible to insert before and after.
            const threshold = target.offsetHeight / 2;
            if (position.y < threshold) {
              updateDragClassname(target, dragBeforeClassName);
            } else {
              updateDragClassname(target, dragAfterClassName);
            }
          }
        },
        ondragleave: (e: DragEvent) => {
          if (!reorderable) {
            return;
          }
          const target = e.currentTarget as HTMLElement;
          const related = e.relatedTarget as HTMLElement | null;
          if (related && !target.contains(related)) {
            target.classList.remove(dragAfterClassName);
            target.classList.remove(dragBeforeClassName);
          }
        },
        ondrop: (e: DragEvent) => {
          if (!reorderable) {
            return;
          }
          const id = e.dataTransfer?.getData('text/plain');
          const target = e.currentTarget as HTMLElement;
          const position = currentTargetOffset(e);

          if (id !== undefined) {
            if (summary) {
              // For summary tracks, split the track into thirds, so it's
              // possible to insert above, below and into.
              const threshold = target.offsetHeight / 3;
              if (position.y < threshold) {
                // Dropped on the upper third, move before this node.
                onMoveBefore(id);
              } else if (position.y < threshold * 2) {
                // Dropped in the middle, move inside this node.
                onMoveInside(id);
              } else {
                // Dropped on the lower third, move after this node.
                onMoveAfter(id);
              }
            } else {
              // For non-summary tracks, split the track in half, as it's only
              // possible to insert before and after.
              const threshold = target.offsetHeight / 2;
              if (position.y < threshold) {
                onMoveBefore(id);
              } else {
                onMoveAfter(id);
              }
            }
          }

          // Remove all the modifiers
          target.classList.remove(dragAfterClassName);
          target.classList.remove(dragInsideClassName);
          target.classList.remove(dragBeforeClassName);
        },
      },
      lite
        ? attrs.title
        : m(
            '.pf-track__menubar',
            collapsible
              ? m(Button, {
                  className: 'pf-track__collapse-button',
                  compact: true,
                  icon: collapsed ? Icons.ExpandDown : Icons.ExpandUp,
                })
              : m('.pf-track__title-spacer'),
            m(TrackTitle, {title: attrs.title}),
            chips &&
              m(
                Stack,
                {
                  className: 'pf-track__chips',
                  spacing: 'small',
                  orientation: 'horizontal',
                },
                chips.map((chip) =>
                  m(Chip, {label: chip, compact: true, rounded: true}),
                ),
              ),
            m(
              ButtonBar,
              {
                className: 'pf-track__buttons',
                // Block button clicks from hitting the shell's on click event
                onclick: (e: MouseEvent) => e.stopPropagation(),
              },
              buttons,
              // Always render this one last
              attrs.error && renderCrashButton(attrs.error, attrs.pluginId),
            ),
            attrs.subtitle &&
              !showSubtitleInContent(attrs) &&
              m(
                '.pf-track__subtitle',
                m(MiddleEllipsis, {text: attrs.subtitle}),
              ),
          ),
    );
  }

  private renderContent(attrs: TrackShellAttrs): m.Children {
    const {
      onTrackContentMouseMove,
      onTrackContentMouseOut,
      onTrackContentClick,
      error,
    } = attrs;

    return m(
      '.pf-track__canvas',
      {
        className: classNames(error && 'pf-track__canvas--error'),
        onmousemove: (e: MithrilEvent<MouseEvent>) => {
          e.redraw = false;
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
      },
      attrs.subtitle &&
        showSubtitleInContent(attrs) &&
        m(MiddleEllipsis, {text: attrs.subtitle}),
    );
  }
}

function showSubtitleInContent(attrs: TrackShellAttrs) {
  return attrs.summary && !attrs.collapsed;
}

function getTargetContainerSize(event: MouseEvent): Bounds2D {
  const target = event.target as HTMLElement;
  return target.getBoundingClientRect();
}

function renderCrashButton(error: Error, pluginId: string | undefined) {
  return m(
    Popup,
    {
      trigger: m(Button, {
        icon: Icons.Crashed,
        compact: true,
      }),
    },
    m(
      '.pf-track__crash-popup',
      m('span', 'This track has crashed.'),
      pluginId && m('span', `Owning plugin: ${pluginId}`),
      m(Button, {
        label: 'View & Report Crash',
        intent: Intent.Primary,
        variant: ButtonVariant.Filled,
        className: Popup.DISMISS_POPUP_GROUP_CLASS,
        onclick: () => {
          throw error;
        },
      }),
      // TODO(stevegolton): In the future we should provide a quick way to
      // disable the plugin, or provide a link to the plugin page, but this
      // relies on the plugin page being fully functional.
    ),
  );
}

interface TrackTitleAttrs {
  readonly title: string;
}

class TrackTitle implements m.ClassComponent<TrackTitleAttrs> {
  private readonly trash = new DisposableStack();

  view({attrs}: m.Vnode<TrackTitleAttrs>) {
    return m(
      MiddleEllipsis,
      {
        className: 'pf-track__title',
        text: attrs.title,
      },
      m('.pf-track__title-popup', attrs.title),
    );
  }

  oncreate({dom}: m.VnodeDOM<TrackTitleAttrs>) {
    const title = dom;
    const popup = assertExists(dom.querySelector('.pf-track__title-popup'));

    const resizeObserver = new ResizeObserver(() => {
      // Determine whether to display a title popup based on ellipsization
      if (popup.clientWidth > title.clientWidth) {
        popup.classList.add('pf-track__title-popup--visible');
      } else {
        popup.classList.remove('pf-track__title-popup--visible');
      }
    });

    resizeObserver.observe(title);
    resizeObserver.observe(popup);

    this.trash.defer(() => resizeObserver.disconnect());
  }

  onremove() {
    this.trash.dispose();
  }
}
