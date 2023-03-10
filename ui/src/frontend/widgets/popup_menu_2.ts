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

import * as m from 'mithril';

import {Menu} from './menu';
import {Popup, PopupPosition} from './popup';

interface PopupMenu2Attrs {
  // The trigger is mithril component which is used to toggle the popup when
  // clicked, and provides the anchor on the page which the popup shall hover
  // next to, and to which the popup's arrow shall point. The popup shall move
  // around the page with this component, as if attached to it.
  // This trigger can be any mithril component, but it is typically a Button,
  // an Icon, or some other interactive component.
  // Beware this element will have its `onclick`, `ref`, and `active` attributes
  // overwritten.
  trigger: m.Vnode<any, any>;
  // Close the popup menu when any of the menu items are clicked.
  // Defaults to false.
  closeOnItemClick?: boolean;
  // Which side of the trigger to place to popup.
  // Defaults to "Auto".
  popupPosition?: PopupPosition;
}

// A combination of a Popup and a Menu component.
// The menu contents are passed in as children, and are typically MenuItems or
// MenuDividers, but really they can be any Mithril component.
export class PopupMenu2 implements m.ClassComponent<PopupMenu2Attrs> {
  view({attrs, children}: m.CVnode<PopupMenu2Attrs>) {
    const {
      trigger,
      popupPosition,
      closeOnItemClick,
    } = attrs;

    return m(
        Popup,
        {
          trigger,
          position: popupPosition,
          closeOnContentClick: closeOnItemClick,
        },
        m(Menu, children));
  }
};
