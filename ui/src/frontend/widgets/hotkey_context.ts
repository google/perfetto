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

import {elementIsEditable} from '../../base/dom_utils';

type Modifier = 'Mod'|'Shift';

export interface HotkeyConfig {
  key: string;
  mods: Modifier[];
  callback: () => void;
  allowInEditable?: boolean;
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

    const inEditable = elementIsEditable(e.target);
    if (this.hotkeys) {
      this.hotkeys.forEach((hotkey) => {
        const {key, mods, callback, allowInEditable = false} = hotkey;
        if (inEditable && !allowInEditable) {
          return;
        }
        if (compareKeys(e, key) && checkMods(e, mods)) {
          e.preventDefault();
          callback();
        }
      });
    }
  };
}

// Return true if |hotkey| matches the event's key (case in-sensitive).
function compareKeys(e: KeyboardEvent, hotkey: string): boolean {
  return e.key.toLowerCase() === hotkey.toLowerCase();
}

// Return true if modifiers specified in |mods| match those in the event.
function checkMods(e: KeyboardEvent, mods: Modifier[]): boolean {
  const mod = (e.ctrlKey || e.metaKey);
  const shift = e.shiftKey;
  const wantedMod = mods.includes('Mod');
  const wantedShift = mods.includes('Shift');
  return mod === wantedMod && shift === wantedShift;
}
