// Copyright (C) 2025 The Android Open Source Project
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

import './tab_strip.scss';
import m from 'mithril';
import {classNames} from '../base/classnames';
import {Button} from './button';
import {Icon} from './icon';
import {Icons} from '../base/semantic_icons';
import {PopupMenu} from './menu';
import {PopupPosition} from './popup';
import {assertUnreachable} from '../base/assert';

export interface TabStripAttrs {
  // Additional class name for the container.
  readonly className?: string;
  // Visual style of the tab bar. 'card' (the default) renders classic
  // boxed tab handles on a secondary-background bar; 'underline' renders
  // flat text tabs with a primary underline on the active tab.
  readonly variant?: 'card' | 'underline';
  // Whether the tabs can be reordered by dragging them.
  readonly reorderable?: boolean;
  // Called when a tab is dragged to a new position (only when `reorderable`).
  // `from` is the index of the dragged tab and `to` is the index it should end
  // up at, i.e. remove the tab at `from` then insert it at `to`. Indices only
  // count `TabStrip.Tab` children. Not called if the tab is dropped in place.
  readonly onReorder?: (from: number, to: number) => void;
}

export interface TabStripTabAttrs {
  // Style this tab as the active tab.
  readonly active?: boolean;
  // Style this tab as a disabled tab and prevent interaction.
  readonly disabled?: boolean;
  // If provided, the tab will be rendered as a link with this href.
  readonly href?: string;
  // Additional class name for the tab.
  readonly className?: string;
  // Icon to display on the left side of the tab title.
  readonly leftIcon?: string | m.Children;
  // Icon to display on the right side of the tab title.
  readonly rightIcon?: string | m.Children;
  // Whether to show a close button on the tab.
  readonly closeButton?: boolean;
  // Called when the tab's close button is clicked.
  readonly onClose?: () => void;
  // Optional menu items to show in a dropdown menu on the tab.
  // When provided, a menu button appears on hover.
  readonly menuItems?: m.Children;
  // If provided, the tab can be renamed inline by double-clicking it. Called
  // with the new (trimmed, non-empty) name when the rename is committed
  // (Enter or blur). Pressing Escape cancels without calling this.
  readonly onRename?: (newName: string) => void;
  // Called when the tab is clicked. Not called for the click that ends a
  // drag-to-reorder.
  readonly onClick?: (e: MouseEvent) => void;
}

class Tab implements m.ClassComponent<TabStripTabAttrs> {
  // Inline rename state. `renameValue` is only meaningful while `renaming`.
  private renaming = false;
  private renameValue = '';

  view({attrs, children}: m.CVnode<TabStripTabAttrs>): m.Children {
    const {
      active,
      className,
      leftIcon,
      rightIcon,
      closeButton,
      onClose,
      menuItems,
      onRename,
      disabled,
      href,
      onClick,
    } = attrs;

    const renderIcon = (
      icon: string | m.Children | undefined,
      iconClassName: string,
    ) => {
      if (icon === undefined) {
        return undefined;
      }
      if (typeof icon === 'string') {
        return m(Icon, {icon, className: iconClassName});
      }
      return m('.pf-tab-strip__tab-icon', {className: iconClassName}, icon);
    };

    const tag = href ? 'a' : 'button';

    const commitRename = () => {
      if (!this.renaming) return;
      this.renaming = false;
      const newName = this.renameValue.trim();
      if (newName) {
        onRename?.(newName);
      }
    };

    const cancelRename = () => {
      this.renaming = false;
    };

    return m(
      tag + '.pf-tab-strip__tab',
      {
        'tabIndex': disabled ? -1 : 0,
        'className': classNames(
          className,
          active && 'pf-tab-strip__tab--active',
          disabled && 'pf-tab-strip__tab--disabled',
        ),
        'ondblclick': (e: PointerEvent) => {
          if (onRename && !this.renaming) {
            // Seed the input with the currently rendered title text.
            const titleEl = (e.currentTarget as HTMLElement).querySelector(
              '.pf-tab-strip__tab-title',
            );
            this.renameValue = titleEl?.textContent ?? '';
            this.renaming = true;
          }
        },
        'onauxclick': onClose,
        'onclick': onClick,
        // A disabled link drops its href so it can't be followed, and a
        // disabled button uses the native disabled attribute.
        'href': disabled ? undefined : href,
        'disabled': tag === 'button' ? disabled : undefined,
        'aria-disabled': disabled ? 'true' : undefined,
      },
      [
        renderIcon(leftIcon, 'pf-tab-strip__tab-icon--left'),
        this.renaming
          ? m('input.pf-tab-strip__tab-rename-input', {
              value: this.renameValue,
              oncreate: (vnode: m.VnodeDOM) => {
                const el = vnode.dom as HTMLInputElement;
                el.focus();
                el.select();
              },
              oninput: (e: InputEvent) => {
                const target = e.target as HTMLInputElement;
                this.renameValue = target.value;
              },
              onkeydown: (e: KeyboardEvent) => {
                if (e.key === 'Enter') {
                  commitRename();
                  e.preventDefault();
                } else if (e.key === 'Escape') {
                  cancelRename();
                  e.preventDefault();
                }
                e.stopPropagation();
              },
              onblur: commitRename,
              onclick: (e: Event) => e.stopPropagation(),
            })
          : m('span.pf-tab-strip__tab-title', children),
        renderIcon(rightIcon, 'pf-tab-strip__tab-icon--right'),
        menuItems !== undefined &&
          m(
            PopupMenu,
            {
              trigger: m(Button, {
                rounded: true,
                icon: Icons.ContextMenuAlt,
                className: 'pf-tab-strip__tab-btn pf-tab-strip__tab-menu-btn',
              }),
              position: PopupPosition.Bottom,
            },
            menuItems,
          ),
        closeButton &&
          m(Button, {
            rounded: true,
            icon: Icons.Close,
            className: 'pf-tab-strip__tab-btn',
            onclick: (e: Event) => {
              e.stopPropagation();
              onClose?.();
            },
          }),
      ],
    );
  }
}

/**
 * A horizontal tab bar. Tabs are passed as children using the
 * `TabStrip.Tab` sub-component:
 *
 * ```ts
 * m(
 *   TabStrip,
 *   m(TabStrip.Tab, {active: true, onClick: () => {}}, 'Content'),
 *   m(TabStrip.Tab, {onClick: () => {}}, 'Other'),
 * );
 * ```
 */
export class TabStrip implements m.ClassComponent<TabStripAttrs> {
  static readonly Tab = Tab;

  // Latest attrs, for use in the DOM event listeners.
  private attrs: TabStripAttrs = {};
  // The `.pf-tab-strip__tabs` element, which owns the reorder listeners.
  private tabsEl?: HTMLElement;
  // Set from pointerdown on a tab until pointerup/cancel.
  private drag?: DragState;
  // Pending timer for the drop animation, during which new drags are ignored.
  private settleTimer?: ReturnType<typeof setTimeout>;

  oncreate({dom}: m.VnodeDOM<TabStripAttrs>) {
    this.tabsEl =
      dom.querySelector<HTMLElement>('.pf-tab-strip__tabs') ?? undefined;
    this.tabsEl?.addEventListener('pointerdown', this.onPointerDown);
    this.tabsEl?.addEventListener('dragstart', this.onNativeDragStart);
  }

  onremove() {
    this.tabsEl?.removeEventListener('pointerdown', this.onPointerDown);
    this.tabsEl?.removeEventListener('dragstart', this.onNativeDragStart);
    this.removeClickSuppressor();
    this.removeWindowListeners();
    clearTimeout(this.settleTimer);
  }

  view({attrs, children}: m.CVnode<TabStripAttrs>): m.Children {
    this.attrs = attrs;
    const {className, variant = 'card', reorderable} = attrs;
    return m(
      '.pf-tab-strip',
      {
        className: classNames(
          className,
          variantToClassName(variant),
          reorderable && 'pf-tab-strip--reorderable',
        ),
      },
      m('.pf-tab-strip__tabs', children),
    );
  }

  private readonly onPointerDown = (e: PointerEvent) => {
    // Any click suppressor left over from a previous drag (e.g. the pointer
    // was released outside the strip so no click fired) must not swallow
    // this new, genuine click.
    this.removeClickSuppressor();

    if (!this.attrs.reorderable || this.drag || this.settleTimer) return;
    if (e.button !== 0) return;

    const target = e.target as Element;
    const tab = target.closest<HTMLElement>('.pf-tab-strip__tab');
    if (tab === null) return;
    // Don't start a drag from interactive content inside the tab, such as the
    // rename input or the menu/close buttons.
    const interactive = target.closest('input, button, a');
    if (interactive !== null && interactive !== tab) return;

    const tabs = this.getTabElements();
    const fromIndex = tabs.indexOf(tab);
    if (fromIndex === -1) return;

    this.drag = {
      pointerId: e.pointerId,
      startX: e.clientX,
      fromIndex,
      toIndex: fromIndex,
      tabs,
    };
    window.addEventListener('pointermove', this.onPointerMove);
    window.addEventListener('pointerup', this.onPointerUp);
    window.addEventListener('pointercancel', this.onPointerCancel);
    window.addEventListener('keydown', this.onKeyDown, true);
  };

  // Link tabs are natively draggable, which would hijack the pointer events.
  private readonly onNativeDragStart = (e: DragEvent) => {
    if (this.attrs.reorderable) {
      e.preventDefault();
    }
  };

  private readonly onPointerMove = (e: PointerEvent) => {
    const drag = this.drag;
    if (drag === undefined || e.pointerId !== drag.pointerId) return;
    const dx = e.clientX - drag.startX;
    if (drag.rects === undefined) {
      // Treat it as a click until the pointer has moved far enough.
      if (Math.abs(dx) < DRAG_THRESHOLD_PX) return;
      this.startDrag(drag);
    }
    this.updateDrag(drag, dx);
  };

  private readonly onPointerUp = (e: PointerEvent) => {
    const drag = this.drag;
    if (drag === undefined || e.pointerId !== drag.pointerId) return;
    this.finishDrag(drag, drag.toIndex);
  };

  private readonly onPointerCancel = (e: PointerEvent) => {
    const drag = this.drag;
    if (drag === undefined || e.pointerId !== drag.pointerId) return;
    this.finishDrag(drag, drag.fromIndex);
  };

  private readonly onKeyDown = (e: KeyboardEvent) => {
    const drag = this.drag;
    if (drag === undefined || e.key !== 'Escape') return;
    e.preventDefault();
    e.stopPropagation();
    this.finishDrag(drag, drag.fromIndex);
  };

  // The click that follows a drag must not activate the tab or follow a link.
  private readonly suppressClick = (e: MouseEvent) => {
    e.preventDefault();
    e.stopPropagation();
    this.removeClickSuppressor();
  };

  private removeClickSuppressor() {
    this.tabsEl?.removeEventListener('click', this.suppressClick, true);
  }

  private removeWindowListeners() {
    window.removeEventListener('pointermove', this.onPointerMove);
    window.removeEventListener('pointerup', this.onPointerUp);
    window.removeEventListener('pointercancel', this.onPointerCancel);
    window.removeEventListener('keydown', this.onKeyDown, true);
  }

  // The tab elements belonging to this strip (not any nested strips), in DOM
  // order, which matches the order of the TabStrip.Tab children.
  private getTabElements(): HTMLElement[] {
    const container = this.tabsEl;
    if (container === undefined) return [];
    return Array.from(
      container.querySelectorAll<HTMLElement>('.pf-tab-strip__tab'),
    ).filter((el) => el.closest('.pf-tab-strip__tabs') === container);
  }

  private startDrag(drag: DragState) {
    // Snapshot the layout once, so the maths isn't affected by the transforms
    // applied during the drag.
    drag.rects = drag.tabs.map((el) => el.getBoundingClientRect());
    this.tabsEl?.classList.add('pf-tab-strip__tabs--reordering');
    const dragged = drag.tabs[drag.fromIndex];
    // The dragged tab tracks the pointer exactly and sits above the others.
    dragged.style.transition = 'none';
    dragged.style.zIndex = '2';
  }

  private updateDrag(drag: DragState, rawDx: number) {
    const rects = drag.rects!;
    const from = drag.fromIndex;
    const fromRect = rects[from];
    const last = rects.length - 1;

    // Keep the dragged tab within the extent of the tabs.
    const dx = Math.min(
      Math.max(rawDx, rects[0].left - fromRect.left),
      rects[last].right - fromRect.right,
    );

    // The dragged tab moves to the slot of the furthest tab whose midpoint its
    // centre has passed.
    const centre = fromRect.left + fromRect.width / 2 + dx;
    let to = from;
    for (let i = from + 1; i <= last; i++) {
      if (centre > midX(rects[i])) to = i;
    }
    for (let i = from - 1; i >= 0; i--) {
      if (centre < midX(rects[i])) to = i;
    }
    drag.toIndex = to;

    this.applyOffsets(drag, to, dx);
  }

  // Translates the dragged tab by `draggedDx` and shifts the tabs between
  // `from` and `to` over by one slot to make room for it.
  private applyOffsets(drag: DragState, to: number, draggedDx: number) {
    const rects = drag.rects!;
    const from = drag.fromIndex;
    const fromRect = rects[from];
    const last = rects.length - 1;
    // Distance a neighbour moves when the dragged tab (and its gap) is removed
    // from in front of / behind it.
    const shiftLeft = from < last ? rects[from + 1].left - fromRect.left : 0;
    const shiftRight = from > 0 ? fromRect.right - rects[from - 1].right : 0;

    drag.tabs.forEach((el, i) => {
      let offset = 0;
      if (i === from) {
        offset = draggedDx;
      } else if (i > from && i <= to) {
        offset = -shiftLeft;
      } else if (i < from && i >= to) {
        offset = shiftRight;
      }
      el.style.transform = offset !== 0 ? `translateX(${offset}px)` : '';
    });
  }

  // Ends the drag, animating the tabs into their final slots for `to` (pass
  // `fromIndex` to cancel), then reports the reorder.
  private finishDrag(drag: DragState, to: number) {
    this.removeWindowListeners();
    this.drag = undefined;
    if (drag.rects === undefined) {
      // The pointer never moved far enough: this was a plain click.
      return;
    }

    // The click that follows the pointerup must not activate the tab.
    this.tabsEl?.addEventListener('click', this.suppressClick, true);

    const rects = drag.rects;
    const from = drag.fromIndex;
    let slotDx = 0;
    if (to > from) {
      slotDx = rects[to].right - rects[from].right;
    } else if (to < from) {
      slotDx = rects[to].left - rects[from].left;
    }

    // Let the dragged tab animate into its slot along with the others.
    drag.tabs[from].style.transition = '';
    this.applyOffsets(drag, to, slotDx);

    this.settleTimer = setTimeout(() => {
      this.settleTimer = undefined;
      // Remove the transitions before clearing the transforms, so the tabs
      // jump straight to their untransformed positions. Reporting the reorder
      // and redrawing synchronously means the DOM is reordered in the same
      // frame, so the tabs appear to stay where they settled.
      this.tabsEl?.classList.remove('pf-tab-strip__tabs--reordering');
      for (const el of drag.tabs) {
        el.style.transform = '';
        el.style.transition = '';
        el.style.zIndex = '';
      }
      if (to !== from) {
        this.attrs.onReorder?.(from, to);
      }
      m.redraw.sync();
    }, SETTLE_DURATION_MS);
  }
}

// How far (in px) the pointer must move before a press on a tab becomes a
// drag rather than a click.
const DRAG_THRESHOLD_PX = 4;

// Duration of the drop animation. Must match the transform transition in
// tab_strip.scss.
const SETTLE_DURATION_MS = 150;

interface DragState {
  readonly pointerId: number;
  // Pointer X position at pointerdown.
  readonly startX: number;
  // Index of the dragged tab.
  readonly fromIndex: number;
  // Index the dragged tab would move to if dropped now.
  toIndex: number;
  // This strip's tab elements, in order.
  readonly tabs: HTMLElement[];
  // Layout of the tabs, captured when the drag starts. Undefined until the
  // pointer has moved past the drag threshold.
  rects?: DOMRect[];
}

function midX(rect: DOMRect): number {
  return rect.left + rect.width / 2;
}

function variantToClassName(variant: 'card' | 'underline'): string {
  switch (variant) {
    case 'card':
      return 'pf-tab-strip--card';
    case 'underline':
      return 'pf-tab-strip--underline';
    default:
      assertUnreachable(variant);
  }
}
