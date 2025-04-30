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
 * A persistent status bar component typically rendered at the bottom of the UI.
 */
export function renderStatusBar(trace: Trace | undefined): m.Children {
  return m(
    '.pf-statusbar',
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
