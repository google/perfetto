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

import {createPopper, Instance, OptionsGeneric} from '@popperjs/core';
import m from 'mithril';
import {MountOptions, Portal, PortalAttrs} from './portal';
import {classNames} from '../base/classnames';
import {findRef, toHTMLElement} from '../base/dom_utils';
import {assertExists} from '../base/logging';
import {PopupPosition} from './popup';
import {ExtendedModifiers} from './popper_utils';

export interface TooltipAttrs {
  // Which side of the trigger to place to tooltip.
  // Defaults to "Auto"
  position?: PopupPosition;
  // The element used to open and close the tooltip, and to which the tooltip
  // will be anchored. Beware this element will have its `onmouseenter`,
  // `onmouseleave`, `ref` attributes overwritten.
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  trigger: m.Vnode<any, any>;
  // Space delimited class names applied to the tooltip div.
  className?: string;
  // Whether to show a little arrow pointing to our trigger element.
  // Defaults to true.
  showArrow?: boolean;
  // Called when the tooltip mounts, passing the tooltip's dom element.
  onTooltipMount?: (dom: HTMLElement) => void;
  // Called when the tooltip unmounts, padding the tooltip's dom element.
  onTooltipUnMount?: (dom: HTMLElement) => void;
  // Distance in px between the tooltip and its trigger. Default = 0.
  offset?: number;
  // Cross-axial tooltip offset in px. Defaults to 0.
  // When position is *-end or *-start, this setting specifies where start and
  // end is as an offset from the edge of the tooltip.
  // Positive values move the positioning away from the edge towards the center
  // of the tooltip.
  // If position is not *-end or *-start, this setting has no effect.
  edgeOffset?: number;
  // If true, the tooltip will not have a maximum width and will instead fit its
  // content. This is useful for tooltips that have a lot of buttons or other
  // content that should not be constrained by a maximum width.
  // Defaults to false.
  fitContent?: boolean;
}

// A tooltip is a portal whose position is dynamically updated so that it floats
// next to a trigger element. It is also styled with a nice backdrop, and
// a little arrow pointing at the trigger element.
// Useful for displaying things like tooltips.
export class Tooltip implements m.ClassComponent<TooltipAttrs> {
  private isOpen: boolean = false;
  private triggerElement?: Element;
  private tooltipElement?: HTMLElement;
  private popper?: Instance;

  private static readonly TRIGGER_REF = 'trigger';
  private static readonly TOOLTIP_REF = 'tooltip';

  view({attrs, children}: m.CVnode<TooltipAttrs>): m.Children {
    const {trigger} = attrs;

    return [
      this.renderTrigger(trigger),
      this.isOpen && this.renderToolip(attrs, children),
    ];
  }

  private renderTrigger(
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    trigger: m.Vnode<any, any>,
  ): m.Children {
    trigger.attrs = {
      ...trigger.attrs,
      ref: Tooltip.TRIGGER_REF,
      onmouseenter: () => {
        this.isOpen = true;
      },
      onmouseleave: () => {
        this.isOpen = false;
      },
    };
    return trigger;
  }

  private renderToolip(attrs: TooltipAttrs, children: m.Children): m.Children {
    const {
      className,
      showArrow = true,
      onTooltipMount = () => {},
      onTooltipUnMount = () => {},
      fitContent,
    } = attrs;

    const portalAttrs: PortalAttrs = {
      className: 'pf-tooltip-portal',
      onBeforeContentMount: (dom: Element): MountOptions => {
        // Check to see if dom is a descendant of a popup or modal
        // If so, get the popup's "container" and put it in there instead
        // This handles the case where popups are placed inside the other popups
        // we nest outselves in their containers instead of document body which
        // means we become part of their hitbox for mouse events.
        const closestPopup = dom.closest(`[ref=${Tooltip.TOOLTIP_REF}]`);
        if (closestPopup) {
          return {container: closestPopup};
        }
        const closestModal = dom.closest('.pf-overlay-container');
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
          assertExists(findRef(dom, Tooltip.TOOLTIP_REF)),
        );
        this.tooltipElement = popupElement;
        this.createOrUpdatePopper(attrs);
        onTooltipMount(popupElement);
      },
      onContentUpdate: () => {
        this.popper?.update();
      },
      onContentUnmount: () => {
        if (this.tooltipElement) {
          onTooltipUnMount(this.tooltipElement);
        }
        this.popper?.destroy();
        this.popper = undefined;
        this.tooltipElement = undefined;
      },
    };

    return m(
      Portal,
      portalAttrs,
      m(
        '.pf-popup', // Re-use popup styles
        {
          class: classNames(className, fitContent && 'pf-popup--fit-content'),
          ref: Tooltip.TOOLTIP_REF,
        },
        showArrow && m('.pf-popup-arrow[data-popper-arrow]'),
        m('.pf-popup-content', children),
      ),
    );
  }

  oncreate({dom}: m.VnodeDOM<TooltipAttrs, this>) {
    this.triggerElement = assertExists(findRef(dom, Tooltip.TRIGGER_REF));
  }

  onupdate({attrs}: m.VnodeDOM<TooltipAttrs, this>) {
    this.createOrUpdatePopper(attrs);
  }

  onremove(_: m.VnodeDOM<TooltipAttrs, this>) {
    this.triggerElement = undefined;
  }

  private createOrUpdatePopper(attrs: TooltipAttrs) {
    const {
      position = PopupPosition.Auto,
      showArrow = true,
      offset = 0,
      edgeOffset = 0,
    } = attrs;

    const options: Partial<OptionsGeneric<ExtendedModifiers>> = {
      placement: position,
      modifiers: [
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
        {name: 'preventOverflow', options: {padding: 8}},
        {name: 'arrow', options: {padding: 2}},
      ],
    };

    if (this.popper) {
      this.popper.setOptions(options);
    } else {
      if (this.tooltipElement && this.triggerElement) {
        this.popper = createPopper<ExtendedModifiers>(
          this.triggerElement,
          this.tooltipElement,
          options,
        );
      }
    }
  }
}
