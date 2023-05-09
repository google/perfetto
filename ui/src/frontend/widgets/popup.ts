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
import type {StrictModifiers} from '@popperjs/core';
import m from 'mithril';
import {globals} from '../globals';
import {MountOptions, Portal, PortalAttrs} from './portal';
import {classNames} from '../classnames';
import {findRef, isOrContains, toHTMLElement} from './utils';
import {assertExists} from '../../base/logging';

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
  trigger: m.Vnode<any, any>;
  // Close when the escape key is pressed
  // Defaults to true.
  closeOnEscape?: boolean;
  // Close on mouse down somewhere other than the popup or trigger.
  // Defaults to true.
  closeOnOutsideClick?: boolean;
  // Controls whether the popup is open or not.
  // If omitted, the popup operates in uncontrolled mode.
  isOpen?: boolean;
  // Called when the popup isOpen state should be changed in controlled mode.
  onChange?: OnChangeCallback;
  // Space delimited class names applied to the popup div.
  className?: string;
  // Whether to show a little arrow pointing to our trigger element.
  // Defaults to true.
  showArrow?: boolean;
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

  private static readonly TRIGGER_REF = 'trigger';
  private static readonly POPUP_REF = 'popup';
  static readonly DISMISS_POPUP_CLASS = 'pf-dismiss-popup';

  view({attrs, children}: m.CVnode<PopupAttrs>): m.Children {
    const {
      trigger,
      isOpen = this.isOpen,
      onChange = () => {},
      closeOnEscape = true,
      closeOnOutsideClick = true,
    } = attrs;

    this.isOpen = isOpen;
    this.onChange = onChange;
    this.closeOnEscape = closeOnEscape;
    this.closeOnOutsideClick = closeOnOutsideClick;

    return [
      this.renderTrigger(trigger),
      isOpen && this.renderPopup(attrs, children),
    ];
  }

  private renderTrigger(trigger: m.Vnode<any, any>): m.Children {
    trigger.attrs = {
      ...trigger.attrs,
      ref: Popup.TRIGGER_REF,
      onclick: () => {
        this.togglePopup();
      },
      active: this.isOpen,
    };
    return trigger;
  }

  private renderPopup(attrs: PopupAttrs, children: any): m.Children {
    const {
      className,
      showArrow = true,
    } = attrs;

    const portalAttrs: PortalAttrs = {
      className: 'pf-popup-portal',
      onBeforeContentMount: (dom: Element): MountOptions => {
        // Check to see if dom is a descendant of a popup
        // If so, get the popup's "container" and put it in there instead
        // This handles the case where popups are placed inside the other popups
        // we nest outselves in their containers instead of document body which
        // means we become part of their hitbox for mouse events.
        const closestPopup = dom.closest(`[ref=${Popup.POPUP_REF}]`);
        return {container: closestPopup ?? undefined};
      },
      onContentMount: (dom: HTMLElement) => {
        this.popupElement =
            toHTMLElement(assertExists(findRef(dom, Popup.POPUP_REF)));
        this.createOrUpdatePopper(attrs);
        document.addEventListener('mousedown', this.handleDocMouseDown);
        document.addEventListener('keydown', this.handleDocKeyPress);
        dom.addEventListener('click', this.handleContentClick);
      },
      onContentUpdate: () => {
        // The content inside the portal has updated, so we call popper to
        // recompute the popup's position, in case it has changed size.
        this.popper && this.popper.update();
      },
      onContentUnmount: (dom: HTMLElement) => {
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
        m('.pf-popup',
          {
            class: classNames(className),
            ref: Popup.POPUP_REF,
          },
          showArrow && m('.pf-popup-arrow[data-popper-arrow]'),
          m('.pf-popup-content', children)),
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

  private createOrUpdatePopper(attrs: PopupAttrs) {
    const {
      position = PopupPosition.Auto,
      showArrow = true,
    } = attrs;

    const options: Partial<OptionsGeneric<StrictModifiers>> = {
      placement: position,
      modifiers: [
        // Move the popup away from the target allowing room for the arrow
        {
          name: 'offset',
          options: {offset: [0, showArrow ? 8 : 0]},
        },
        // Don't let the popup touch the edge of the viewport
        {name: 'preventOverflow', options: {padding: 8}},
        // Don't let the arrow reach the end of the popup, which looks odd when
        // the popup has rounded corners
        {name: 'arrow', options: {padding: 8}},
      ],
    };

    if (this.popper) {
      this.popper.setOptions(options);
    } else {
      if (this.popupElement && this.triggerElement) {
        this.popper = createPopper<StrictModifiers>(
            this.triggerElement, this.popupElement, options);
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
    if (this.closeOnEscape && e.key === 'Escape') {
      this.closePopup();
    }
  };

  private handleContentClick = (e: Event) => {
    // Close the popup if the clicked element:
    // - Is a direct descendant of this popup
    // - Has the magic class
    const target = e.target as HTMLElement;
    const childPopup = this.popupElement?.querySelector('.pf-popup');
    if (childPopup) {
      if (childPopup.contains(target)) {
        return;
      }
    }
    if (target.closest(`.${Popup.DISMISS_POPUP_CLASS}`)) {
      this.closePopup();
    }
  };

  private closePopup() {
    if (this.isOpen) {
      this.isOpen = false;
      this.onChange(this.isOpen);
      globals.rafScheduler.scheduleFullRedraw();
    }
  }

  private togglePopup() {
    this.isOpen = !this.isOpen;
    this.onChange(this.isOpen);
    globals.rafScheduler.scheduleFullRedraw();
  }
}
