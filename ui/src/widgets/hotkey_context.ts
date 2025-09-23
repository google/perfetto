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

import m from 'mithril';
import {checkHotkey, Hotkey} from '../base/hotkeys';
import {toHTMLElement} from '../base/dom_utils';
import {classNames} from '../base/classnames';

export interface HotkeyConfig {
  readonly hotkey: Hotkey;
  readonly callback: () => void;
}

export interface HotkeyContextAttrs {
  // An array of hotkeys to listen for.
  readonly hotkeys: HotkeyConfig[];

  // If true, the context will fill the height of its parent container.
  // This is useful for contexts that are used as a full-screen overlay.
  readonly fillHeight?: boolean;

  // If true, the context will be focused on creation.
  // Defaults to false.
  readonly autoFocus?: boolean;

  // If true, a focus ring will be shown when the context is focused.
  // Defaults to false.
  readonly showFocusRing?: boolean;
}

export class HotkeyContext implements m.ClassComponent<HotkeyContextAttrs> {
  private hotkeys?: HotkeyConfig[];

  view(vnode: m.Vnode<HotkeyContextAttrs>): m.Children {
    return m(
      '.pf-hotkey-context',
      {
        // The tabindex is necessary to make the context focusable.
        // This is needed to capture key events.
        // The -1 value means it won't be focusable by tabbing, but can be
        // focused programmatically.
        tabindex: -1,
        className: classNames(
          vnode.attrs.fillHeight && 'pf-hotkey-context--fill-height',
          vnode.attrs.showFocusRing && 'pf-hotkey-context--show-focus-ring',
        ),
      },
      vnode.children,
    );
  }

  oncreate(vnode: m.VnodeDOM<HotkeyContextAttrs>) {
    vnode.dom.addEventListener('keydown', this.onKeyDown);
    this.hotkeys = vnode.attrs.hotkeys;
    if (vnode.attrs.autoFocus) {
      toHTMLElement(vnode.dom).focus();
    }
  }

  onupdate(vnode: m.VnodeDOM<HotkeyContextAttrs>) {
    this.hotkeys = vnode.attrs.hotkeys;
  }

  onremove(vnode: m.VnodeDOM<HotkeyContextAttrs>) {
    vnode.dom.removeEventListener('keydown', this.onKeyDown);
    this.hotkeys = undefined;
  }

  // Due to a bug in chrome, we get onKeyDown events fired where the payload is
  // not a KeyboardEvent when selecting an item from an autocomplete suggestion.
  // See https://issues.chromium.org/issues/41425904
  // Thus, we can't assume we get an KeyboardEvent and must check manually.
  private onKeyDown = (e: Event) => {
    // Find out whether the event has already been handled further up the chain.
    if (e.defaultPrevented) return;

    if (e instanceof KeyboardEvent) {
      this.hotkeys?.forEach(({callback, hotkey}) => {
        if (checkHotkey(hotkey, e)) {
          e.preventDefault();
          callback();
          m.redraw();
        }
      });
    }
  };
}
