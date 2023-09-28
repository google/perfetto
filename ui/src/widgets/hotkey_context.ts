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

export interface HotkeyConfig {
  hotkey: Hotkey;
  callback: () => void;
}

export interface HotkeyContextAttrs {
  hotkeys: HotkeyConfig[];
}

export class HotkeyContext implements m.ClassComponent<HotkeyContextAttrs> {
  private hotkeys?: HotkeyConfig[];

  view(vnode: m.Vnode<HotkeyContextAttrs>): m.Children {
    return vnode.children;
  }

  oncreate(vnode: m.VnodeDOM<HotkeyContextAttrs>) {
    document.addEventListener('keydown', this.onKeyDown);
    this.hotkeys = vnode.attrs.hotkeys;
  }

  onupdate(vnode: m.VnodeDOM<HotkeyContextAttrs>) {
    this.hotkeys = vnode.attrs.hotkeys;
  }

  onremove(_vnode: m.VnodeDOM<HotkeyContextAttrs>) {
    document.removeEventListener('keydown', this.onKeyDown);
    this.hotkeys = undefined;
  }

  private onKeyDown = (e: KeyboardEvent) => {
    // Find out whether the event has already been handled further up the chain.
    if (e.defaultPrevented) return;

    this.hotkeys?.forEach(({callback, hotkey}) => {
      if (checkHotkey(hotkey, e)) {
        e.preventDefault();
        callback();
      }
    });
  };
}
