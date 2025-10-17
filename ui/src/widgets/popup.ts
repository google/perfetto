// Copyright (C) 2023 The Android Open Source Project
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

import {createPopper, Instance, OptionsGeneric} from '@popperjs/core';
import type {Modifier} from '@popperjs/core';
import m from 'mithril';
import {MountOptions, Portal, PortalAttrs} from './portal';
import {classNames} from '../base/classnames';
import {findRef, isOrContains, toHTMLElement} from '../base/dom_utils';
import {assertExists} from '../base/logging';
import {ExtendedModifiers} from './popper_utils';

// Note: We could just use the Placement type from popper.js instead, which is a
// union of string literals corresponding to the values in this enum, but having
// the emun makes it possible to enumerate the possible options, which is a
// feature used in the widgets page.
export enum PopupPosition {
  Auto = 'auto',
  AutoStart = 'auto-start',
  AutoEnd = 'auto-end',
  Top = 'top',
  TopStart = 'top-start',
  TopEnd = 'top-end',
  Bottom = 'bottom',
  BottomStart = 'bottom-start',
  BottomEnd = 'bottom-end',
  Right = 'right',
  RightStart = 'right-start',
  RightEnd = 'right-end',
  Left = 'left',
  LeftStart = 'left-start',
  LeftEnd = 'left-end',
}

type OnChangeCallback = (shouldOpen: boolean) => void;

export interface PopupAttrs {
  // Which side of the trigger to place to popup.
  // Defaults to "Auto"
  position?: PopupPosition;
  // The element used to open and close the popup, and the target which the near
  // which the popup should hover.
  // Beware this element will have its `onclick`, `ref`, and `active` attributes
  // overwritten.
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  trigger: m.Vnode<any, any>;
  // Close when the escape key is pressed
  // Defaults to true.
  closeOnEscape?: boolean;
  // Close on mouse down somewhere other than the popup or trigger.
  // Defaults to true.
  closeOnOutsideClick?: boolean;
  // Controls whether the popup is open or not.
  // When provided, the popup operates in controlled mode and will not
  // automatically toggle on trigger clicks. The parent component must
  // handle opening the popup (e.g., via the trigger's onclick handler).
  // When omitted, the popup operates in uncontrolled mode and
  // automatically toggles when the trigger is clicked.
  isOpen?: boolean;
  // Called when the popup isOpen state should be changed in controlled mode.
  onChange?: OnChangeCallback;
  // Space delimited class names applied to the popup div.
  className?: string;
  // Whether to show a little arrow pointing to our trigger element.
  // Defaults to true.
  showArrow?: boolean;
  // Whether this popup should form a new popup group.
  // When nesting popups, grouping controls how popups are closed.
  // When closing popups via the Escape key, each group is closed one by one,
  // starting at the topmost group in the stack.
  // When using a magic button to close groups (see DISMISS_POPUP_GROUP_CLASS),
  // only the group in which the button lives and it's children will be closed.
  // Defaults to true.
  createNewGroup?: boolean;
  // Called when the popup mounts, passing the popup's dom element.
  onPopupMount?: (dom: HTMLElement) => void;
  // Called when the popup unmounts, padding the popup's dom element.
  onPopupUnMount?: (dom: HTMLElement) => void;
  // Popup matches the width of the trigger element. Default = false.
  matchWidth?: boolean;
  // Distance in px between the popup and its trigger. Default = 0.
  offset?: number;
  // Cross-axial popup offset in px. Defaults to 0.
  // When position is *-end or *-start, this setting specifies where start and
  // end is as an offset from the edge of the popup.
  // Positive values move the positioning away from the edge towards the center
  // of the popup.
  // If position is not *-end or *-start, this setting has no effect.
  edgeOffset?: number;
  // If true, the popup will not have a maximum width and will instead fit its
  // content. This is useful for popups that have a lot of buttons or other
  // content that should not be constrained by a maximum width.
  // Defaults to false.
  fitContent?: boolean;
  // If true, the popup will appear when the trigger is on right clicked rather
  // than when it's left clicked.
  isContextMenu?: boolean;
  // If true, position the popup at the cursor location rather than the trigger
  // element. When omitted with isContextMenu=true, defaults to true. When omitted
  // with isContextMenu=false, defaults to false.
  positionAtCursor?: boolean;
}

// A popup is a portal whose position is dynamically updated so that it floats
// next to a trigger element. It is also styled with a nice backdrop, and
// a little arrow pointing at the trigger element.
// Useful for displaying things like popup menus.
export class Popup implements m.ClassComponent<PopupAttrs> {
  private isOpen: boolean = false;
  private triggerElement?: Element;
  private popupElement?: HTMLElement;
  private popper?: Instance;
  private onChange: OnChangeCallback = () => {};
  private closeOnEscape?: boolean;
  private closeOnOutsideClick?: boolean;
  private relativeMouseX?: number; // Relative to trigger element
  private relativeMouseY?: number; // Relative to trigger element

  private static readonly TRIGGER_REF = 'trigger';
  private static readonly POPUP_REF = 'popup';
  static readonly POPUP_GROUP_CLASS = 'pf-popup-group';

  // Any element with this class will close its containing popup group on click
  static readonly DISMISS_POPUP_GROUP_CLASS = 'pf-dismiss-popup-group';

  view({attrs, children}: m.CVnode<PopupAttrs>): m.Children {
    const {
      trigger,
      isOpen = this.isOpen,
      onChange = () => {},
      closeOnEscape = true,
      closeOnOutsideClick = true,
      isContextMenu = false,
    } = attrs;

    // Detect if we're in controlled mode (parent provides isOpen prop)
    const isControlled = attrs.isOpen !== undefined;

    this.isOpen = isOpen;
    this.onChange = onChange;
    this.closeOnEscape = closeOnEscape;
    this.closeOnOutsideClick = closeOnOutsideClick;

    return [
      this.renderTrigger(trigger, isOpen, isControlled, isContextMenu),
      isOpen && this.renderPopup(attrs, children),
    ];
  }

  private renderTrigger(
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    trigger: m.Vnode<any, any>,
    isOpen: boolean,
    isControlled: boolean,
    isContextMenu: boolean,
  ): m.Children {
    const baseAttrs = {
      ...trigger.attrs,
      ref: Popup.TRIGGER_REF,
      active: isOpen,
    };

    // In controlled mode, don't override the trigger's onclick.
    // Let the parent handle opening/closing via the isOpen prop.
    if (!isControlled) {
      if (isContextMenu) {
        // For context menus, right-click opens at cursor, left-click closes
        baseAttrs.oncontextmenu = (e: MouseEvent) => {
          if (this.triggerElement) {
            const rect = this.triggerElement.getBoundingClientRect();
            this.relativeMouseX = e.clientX - rect.left;
            this.relativeMouseY = e.clientY - rect.top;
          }
          // Always open on context menu (re-open at new position if already open)
          if (!this.isOpen) {
            this.isOpen = true;
            this.onChange(true);
          } else {
            // Already open - need to recreate popper at new position
            if (this.popper) {
              this.popper.destroy();
              this.popper = undefined;
            }
            // Force recreation on next update
            m.redraw();
          }
          e.preventDefault();
        };
        // Left click closes context menu if open
        baseAttrs.onclick = (e: MouseEvent) => {
          if (this.isOpen) {
            this.closePopup();
            e.preventDefault();
          }
        };
      } else {
        baseAttrs.onclick = (e: MouseEvent) => {
          if (this.triggerElement) {
            const rect = this.triggerElement.getBoundingClientRect();
            this.relativeMouseX = e.clientX - rect.left;
            this.relativeMouseY = e.clientY - rect.top;
          }
          this.togglePopup();
          e.preventDefault();
        };
      }
    }

    trigger.attrs = baseAttrs;
    return trigger;
  }

  private renderPopup(attrs: PopupAttrs, children: m.Children): m.Children {
    const {
      className,
      showArrow = true,
      createNewGroup = true,
      onPopupMount = () => {},
      onPopupUnMount = () => {},
      matchWidth,
      fitContent,
    } = attrs;

    const portalAttrs: PortalAttrs = {
      className: 'pf-popup-portal',
      onBeforeContentMount: (dom: Element): MountOptions => {
        // Check to see if dom is a descendant of a popup or modal
        // If so, get the popup's "container" and put it in there instead
        // This handles the case where popups are placed inside the other popups
        // we nest outselves in their containers instead of document body which
        // means we become part of their hitbox for mouse events.
        const closestPopup = dom.closest(`[ref=${Popup.POPUP_REF}]`);
        if (closestPopup) {
          return {container: closestPopup};
        }
        const closestModal = dom.closest('.pf-modal-dialog');
        if (closestModal) {
          return {container: closestModal};
        }
        const closestContainer = dom.closest('.pf-overlay-container');
        if (closestContainer) {
          return {container: closestContainer};
        }
        return {container: undefined};
      },
      onContentMount: (dom: HTMLElement) => {
        const popupElement = toHTMLElement(
          assertExists(findRef(dom, Popup.POPUP_REF)),
        );
        this.popupElement = popupElement;
        this.createOrUpdatePopper(attrs);
        document.addEventListener('mousedown', this.handleDocMouseDown);
        document.addEventListener('keydown', this.handleDocKeyPress);
        dom.addEventListener('click', this.handleContentClick);
        onPopupMount(popupElement);
      },
      onContentUpdate: () => {
        // The content inside the portal has updated, so we call popper to
        // recompute the popup's position, in case it has changed size.
        this.popper && this.popper.update();
      },
      onContentUnmount: (dom: HTMLElement) => {
        if (this.popupElement) {
          onPopupUnMount(this.popupElement);
        }
        dom.removeEventListener('click', this.handleContentClick);
        document.removeEventListener('keydown', this.handleDocKeyPress);
        document.removeEventListener('mousedown', this.handleDocMouseDown);
        this.popper && this.popper.destroy();
        this.popper = undefined;
        this.popupElement = undefined;
      },
    };

    return m(
      Portal,
      portalAttrs,
      m(
        '.pf-popup',
        {
          class: classNames(
            className,
            createNewGroup && Popup.POPUP_GROUP_CLASS,
            matchWidth && 'pf-popup--match-width',
            fitContent && 'pf-popup--fit-content',
          ),
          ref: Popup.POPUP_REF,
        },
        showArrow && m('.pf-popup-arrow[data-popper-arrow]'),
        m('.pf-popup-content', children),
      ),
    );
  }

  oncreate({dom}: m.VnodeDOM<PopupAttrs, this>) {
    this.triggerElement = assertExists(findRef(dom, Popup.TRIGGER_REF));
  }

  onupdate({attrs}: m.VnodeDOM<PopupAttrs, this>) {
    // We might have some new popper options, or the trigger might have changed
    // size, so we call popper to recompute the popup's position.
    this.createOrUpdatePopper(attrs);
  }

  onremove(_: m.VnodeDOM<PopupAttrs, this>) {
    this.triggerElement = undefined;
  }

  // Creates a virtual element positioned at relative coordinates within the trigger
  private createRelativeVirtualElement(
    trigger: Element,
    relativeX: number,
    relativeY: number,
  ) {
    return {
      getBoundingClientRect: () => {
        const triggerRect = trigger.getBoundingClientRect();
        const absoluteX = triggerRect.left + relativeX;
        const absoluteY = triggerRect.top + relativeY;

        return {
          width: 0,
          height: 0,
          top: absoluteY,
          right: absoluteX,
          bottom: absoluteY,
          left: absoluteX,
          x: absoluteX,
          y: absoluteY,
          toJSON: () => {},
        };
      },
    };
  }

  private createOrUpdatePopper(attrs: PopupAttrs) {
    const {
      position = PopupPosition.Auto,
      showArrow = true,
      matchWidth = false,
      offset = 0,
      edgeOffset = 0,
      positionAtCursor: explicitPositionAtCursor,
      isContextMenu = false,
    } = attrs;

    // Smart default: context menus default to cursor positioning
    const positionAtCursor = explicitPositionAtCursor ?? isContextMenu;

    let matchWidthModifier: Modifier<'sameWidth', {}>[];
    if (matchWidth) {
      matchWidthModifier = [
        {
          name: 'sameWidth',
          enabled: true,
          phase: 'beforeWrite',
          requires: ['computeStyles'],
          fn: ({state}) => {
            state.styles.popper.width = `${state.rects.reference.width}px`;
          },
          effect: ({state}) => {
            const trigger = state.elements.reference as HTMLElement;
            state.elements.popper.style.width = `${trigger.offsetWidth}px`;
          },
        },
      ];
    } else {
      matchWidthModifier = [];
    }

    // Custom modifier to hide popup when trigger is not visible. This can be
    // due to the trigger or one of its ancestors having display:none.
    const hideOnInvisible: Modifier<'hideOnInvisible', {}> = {
      name: 'hideOnInvisible',
      enabled: true,
      phase: 'main',
      fn({state}) {
        const reference = state.elements.reference;
        if (!(reference instanceof HTMLElement)) {
          return;
        }

        // Check if checkVisibility is supported
        if (typeof reference.checkVisibility === 'function') {
          const isVisible = reference.checkVisibility();

          if (!isVisible) {
            // Hide the popper by setting display to none
            state.elements.popper.style.display = 'none';
          } else {
            // Show the popper
            state.elements.popper.style.display = '';
          }
        } else {
          // Fallback for browsers that don't support checkVisibility()
          // Use intersection observer or other visibility checks
          const rect = reference.getBoundingClientRect();
          const isVisible =
            rect.top >= 0 &&
            rect.left >= 0 &&
            rect.bottom <=
              (window.innerHeight || document.documentElement.clientHeight) &&
            rect.right <=
              (window.innerWidth || document.documentElement.clientWidth) &&
            window.getComputedStyle(reference).visibility !== 'hidden' &&
            window.getComputedStyle(reference).display !== 'none';

          state.elements.popper.style.display = isVisible ? '' : 'none';
        }
      },
    };

    const options: Partial<OptionsGeneric<ExtendedModifiers>> = {
      placement: position,
      modifiers: [
        // Move the popup away from the target allowing room for the arrow
        {
          name: 'offset',
          options: {
            offset: ({placement}) => {
              let skid = 0;
              if (placement.includes('-end')) {
                skid = edgeOffset;
              } else if (placement.includes('-start')) {
                skid = -edgeOffset;
              }
              return [skid, showArrow ? offset + 8 : offset];
            },
          },
        },
        // Don't let the popup touch the edge of the viewport
        {name: 'preventOverflow', options: {padding: 8}},
        // Don't let the arrow reach the end of the popup, which looks odd when
        // the popup has rounded corners
        {name: 'arrow', options: {padding: 2}},
        hideOnInvisible,
        ...matchWidthModifier,
      ],
    };

    if (this.popper) {
      this.popper.setOptions(options);
    } else {
      if (this.popupElement && this.triggerElement) {
        // Use virtual element at relative cursor position if available
        const referenceElement =
          positionAtCursor &&
          this.relativeMouseX !== undefined &&
          this.relativeMouseY !== undefined
            ? this.createRelativeVirtualElement(
                this.triggerElement,
                this.relativeMouseX,
                this.relativeMouseY,
              )
            : this.triggerElement;

        this.popper = createPopper<ExtendedModifiers>(
          referenceElement,
          this.popupElement,
          options,
        );
      }
    }
  }

  private eventInPopupOrTrigger(e: Event): boolean {
    const target = e.target as HTMLElement;
    const onTrigger = isOrContains(assertExists(this.triggerElement), target);
    const onPopup = isOrContains(assertExists(this.popupElement), target);
    return onTrigger || onPopup;
  }

  private handleDocMouseDown = (e: Event) => {
    if (this.closeOnOutsideClick && !this.eventInPopupOrTrigger(e)) {
      this.closePopup();
    }
  };

  private handleDocKeyPress = (e: KeyboardEvent) => {
    // Close on escape keypress if we are in the toplevel group
    const nextGroupElement = this.popupElement?.querySelector(
      `.${Popup.POPUP_GROUP_CLASS}`,
    );
    if (!nextGroupElement) {
      if (this.closeOnEscape && e.key === 'Escape') {
        this.closePopup();
      }
    }
  };

  private handleContentClick = (e: Event) => {
    // Close the popup if the clicked element:
    // - Is in the same group as this class
    // - Has the magic class
    const target = e.target as HTMLElement;
    const childPopup = this.popupElement?.querySelector(
      `.${Popup.POPUP_GROUP_CLASS}`,
    );
    if (childPopup) {
      if (childPopup.contains(target)) {
        return;
      }
    }
    if (target.closest(`.${Popup.DISMISS_POPUP_GROUP_CLASS}`)) {
      this.closePopup();
    }
  };

  private closePopup() {
    this.isOpen = false;
    this.onChange(false);
    m.redraw();
  }

  private togglePopup() {
    this.isOpen = !this.isOpen;
    this.onChange(this.isOpen);
  }
}
