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
import {getPlatform, Hotkey, Key, parseHotkey, Platform} from '../base/hotkeys';
import {Icon} from './icon';
import {classForSpacing, HTMLAttrs} from './common';
import {classNames} from '../base/classnames';

export interface HotkeyGlyphsAttrs {
  readonly hotkey: Hotkey;
  readonly spoof?: Platform;
  readonly spacing?: 'medium' | 'large';
}

// Renders a hotkey as a series of little keycaps.
export class HotkeyGlyphs implements m.ClassComponent<HotkeyGlyphsAttrs> {
  view({attrs}: m.Vnode<HotkeyGlyphsAttrs>) {
    const {hotkey, spoof, spacing} = attrs;

    const platform = spoof || getPlatform();
    const result = parseHotkey(hotkey);
    if (result) {
      const {key, modifier} = result;
      const hasMod = modifier.includes('Mod');
      const hasCtrl = modifier.includes('Ctrl');
      const hasAlt = modifier.includes('Alt');
      const hasShift = modifier.includes('Shift');

      return m(
        'span.pf-hotkey',
        hasMod && m(Keycap, {spacing}, glyphForMod(platform)),
        hasCtrl && m(Keycap, {spacing}, glyphForCtrl(platform)),
        hasAlt && m(Keycap, {spacing}, glyphForAlt(platform)),
        hasShift && m(Keycap, {spacing}, glyphForShift()),
        m(Keycap, {spacing}, glyphForKey(key, platform)),
      );
    } else {
      return m(Keycap, {spacing}, '???');
    }
  }
}

export interface KeycapGlyphsAttrs {
  readonly keyValue: Key;
  readonly spoof?: Platform;
  readonly spacing?: 'medium' | 'large';
}

// Renders a single keycap.
export class KeycapGlyph implements m.ClassComponent<KeycapGlyphsAttrs> {
  view({attrs}: m.Vnode<KeycapGlyphsAttrs>) {
    const {keyValue, spoof, spacing} = attrs;
    const platform = spoof || getPlatform();
    return m(Keycap, {spacing}, glyphForKey(keyValue, platform));
  }
}

export interface KeycapAttrs extends HTMLAttrs {
  readonly spacing?: 'medium' | 'large';
}

export class Keycap implements m.ClassComponent<KeycapAttrs> {
  view({attrs, children}: m.Vnode<KeycapAttrs>) {
    const {spacing = 'medium'} = attrs;
    return m(
      'span.pf-keycap',
      {className: classNames(classForSpacing(spacing)), ...attrs},
      children,
    );
  }
}

function glyphForKey(key: Key, platform: Platform): m.Children {
  if (key === 'Enter') {
    return m(Icon, {icon: 'keyboard_return'});
  } else if (key === 'ArrowUp') {
    return m(Icon, {icon: 'arrow_upward'});
  } else if (key === 'ArrowDown') {
    return m(Icon, {icon: 'arrow_downward'});
  } else if (key === 'Space') {
    return m(Icon, {icon: 'space_bar'});
  } else if (key === 'Escape') {
    if (platform === 'Mac') {
      return 'esc';
    } else {
      return 'Esc';
    }
  } else {
    return key;
  }
}

function glyphForMod(platform: Platform): m.Children {
  if (platform === 'Mac') {
    return m(Icon, {icon: 'keyboard_command_key'});
  } else {
    return 'Ctrl';
  }
}

function glyphForShift(): m.Children {
  return m(Icon, {icon: 'shift'});
}

function glyphForCtrl(platform: Platform): m.Children {
  if (platform === 'Mac') {
    return m(Icon, {icon: 'keyboard_control_key'});
  } else {
    return 'Ctrl';
  }
}

function glyphForAlt(platform: Platform): m.Children {
  if (platform === 'Mac') {
    return m(Icon, {icon: 'keyboard_option_key'});
  } else {
    return 'Alt';
  }
}
