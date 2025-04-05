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

import m from 'mithril';
import {Trace} from '../public/trace';
import {Button, ButtonVariant} from '../widgets/button';
import {Popup, PopupPosition} from '../widgets/popup';

/**
 * Attributes for the StatusBar component.
 */
export interface StatusbarAttrs {
  // Unique key to identify this specific status bar instance,
  // useful for closing the correct one if multiple are shown in sequence.
  key?: string;
  // Content to be displayed within the status bar.
  // Can be direct Mithril children or a function returning children.
  content?: () => m.Children;
}

/**
 * A persistent status bar component typically rendered at the bottom of the UI.
 * It replaces the previous status bar content when shown.
 */
export class StatusBar implements m.ClassComponent<StatusbarAttrs> {
  view(vnode: m.Vnode<StatusbarAttrs>) {
    return m('.pf-statusbar', vnode.children);
  }
}

/**
 * Renders the current status bar component if one is active.
 * @returns An array containing the StatusBar Vnode if active, otherwise empty.
 */
export function renderStatusBar(trace: Trace | undefined): m.Children {
  return m(
    StatusBar,
    trace?.statusbar.statusBarItems.map((item) => {
      const {icon, label, intent, onclick} = item.renderItem();
      const popupContent = item.popupContent?.();
      const itemContent = m(Button, {
        label,
        icon,
        intent,
        onclick,
        variant: ButtonVariant.Filled,
      });
      if (Boolean(popupContent)) {
        return m(
          Popup,
          {
            position: PopupPosition.Top,
            trigger: itemContent,
          },
          popupContent,
        );
      } else {
        return itemContent;
      }
    }),
  );
}
