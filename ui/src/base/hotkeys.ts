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

// This module provides hotkey detection using type-safe human-readable strings.
//
// The basic premise is this: Let's say you have a KeyboardEvent |event|, and
// you wanted to check whether it contains the hotkey 'Ctrl+O', you can execute
// the following function:
//
//   checkHotkey('Shift+O', event);
//
// ...which will evaluate to true if 'Shift+O' is discovered in the event.
//
// This will only trigger when O is pressed while the Shift key is held, not O
// on it's own, and not if other modifiers such as Alt or Ctrl were also held.
//
// Modifiers include 'Shift', 'Ctrl', 'Alt', and 'Mod':
// - 'Shift' and 'Ctrl' are fairly self explanatory.
// - 'Alt' is 'option' on Macs.
// - 'Mod' is a special modifier which means 'Ctrl' on PC and 'Cmd' on Mac.
// Modifiers may be combined in various ways - check the |Modifier| type.
//
// By default hotkeys will not register when the event target is inside an
// editable element, such as <textarea> and some <input>s.
// Prefixing a hotkey with a bang '!' relaxes is requirement, meaning the hotkey
// will register inside editable fields.

// E.g. '!Mod+Shift+P' will register when pressed when a text box has focus but
// 'Mod+Shift+P' (no bang) will not.
// Warning: Be careful using this with single key hotkeys, e.g. '!P' is usually
// never what you want!
//
// Some single-key hotkeys like '?' and '!' normally cannot be activated in
// without also pressing shift key, so the shift requirement is relaxed for
// these keys.

import {elementIsEditable} from './dom_utils';

type Alphabet =
  | 'A'
  | 'B'
  | 'C'
  | 'D'
  | 'E'
  | 'F'
  | 'G'
  | 'H'
  | 'I'
  | 'J'
  | 'K'
  | 'L'
  | 'M'
  | 'N'
  | 'O'
  | 'P'
  | 'Q'
  | 'R'
  | 'S'
  | 'T'
  | 'U'
  | 'V'
  | 'W'
  | 'X'
  | 'Y'
  | 'Z';
type Number = '0' | '1' | '2' | '3' | '4' | '5' | '6' | '7' | '8' | '9';
type Special =
  | 'Enter'
  | 'Escape'
  | 'Delete'
  | '/'
  | '?'
  | '!'
  | 'Space'
  | 'ArrowUp'
  | 'ArrowDown'
  | 'ArrowLeft'
  | 'ArrowRight'
  | '['
  | ']';
export type Key = Alphabet | Number | Special;
export type Modifier =
  | ''
  | 'Mod+'
  | 'Shift+'
  | 'Ctrl+'
  | 'Alt+'
  | 'Mod+Shift+'
  | 'Mod+Alt+'
  | 'Mod+Shift+Alt+'
  | 'Ctrl+Shift+'
  | 'Ctrl+Alt'
  | 'Ctrl+Shift+Alt';
type AllowInEditable = '!' | '';
export type Hotkey = `${AllowInEditable}${Modifier}${Key}`;

// The following list of keys cannot be pressed wither with or without the
// presence of the Shift modifier on most keyboard layouts. Thus we should
// ignore shift in these cases.
const shiftExceptions = [
  '0',
  '1',
  '2',
  '3',
  '4',
  '5',
  '6',
  '7',
  '8',
  '9',
  '/',
  '?',
  '!',
  '[',
  ']',
];

const macModifierStrings: ReadonlyMap<Modifier, string> = new Map<
  Modifier,
  string
>([
  ['', ''],
  ['Mod+', '⌘'],
  ['Shift+', '⇧'],
  ['Ctrl+', '⌃'],
  ['Alt+', '⌥'],
  ['Mod+Shift+', '⌘⇧'],
  ['Mod+Alt+', '⌘⌥'],
  ['Mod+Shift+Alt+', '⌘⇧⌥'],
  ['Ctrl+Shift+', '⌃⇧'],
  ['Ctrl+Alt', '⌃⌥'],
  ['Ctrl+Shift+Alt', '⌃⇧⌥'],
]);

const pcModifierStrings: ReadonlyMap<Modifier, string> = new Map<
  Modifier,
  string
>([
  ['', ''],
  ['Mod+', 'Ctrl+'],
  ['Mod+Shift+', 'Ctrl+Shift+'],
  ['Mod+Alt+', 'Ctrl+Alt+'],
  ['Mod+Shift+Alt+', 'Ctrl+Shift+Alt+'],
]);

// Represents a deconstructed hotkey.
export interface HotkeyParts {
  // The name of the primary key of this hotkey.
  key: Key;

  // All the modifiers as one chunk. E.g. 'Mod+Shift+'.
  modifier: Modifier;

  // Whether this hotkey should register when the event target is inside an
  // editable field.
  allowInEditable: boolean;
}

// Deconstruct a hotkey from its string representation into its constituent
// parts.
export function parseHotkey(hotkey: Hotkey): HotkeyParts | undefined {
  const regex = /^(!?)((?:Mod\+|Shift\+|Alt\+|Ctrl\+)*)(.*)$/;
  const result = hotkey.match(regex);

  if (!result) {
    return undefined;
  }

  return {
    allowInEditable: result[1] === '!',
    modifier: result[2] as Modifier,
    key: result[3] as Key,
  };
}

// Print the hotkey in a human readable format.
export function formatHotkey(
  hotkey: Hotkey,
  spoof?: Platform,
): string | undefined {
  const parsed = parseHotkey(hotkey);
  return parsed && formatHotkeyParts(parsed, spoof);
}

function formatHotkeyParts(
  {modifier, key}: HotkeyParts,
  spoof?: Platform,
): string {
  return `${formatModifier(modifier, spoof)}${key}`;
}

function formatModifier(modifier: Modifier, spoof?: Platform): string {
  const platform = spoof || getPlatform();
  const strings = platform === 'Mac' ? macModifierStrings : pcModifierStrings;
  return strings.get(modifier) ?? modifier;
}

// Like |KeyboardEvent| but all fields apart from |key| are optional.
export type KeyboardEventLike = Pick<KeyboardEvent, 'key'> &
  Partial<KeyboardEvent>;

// Check whether |hotkey| is present in the keyboard event |event|.
export function checkHotkey(
  hotkey: Hotkey,
  event: KeyboardEventLike,
  spoofPlatform?: Platform,
): boolean {
  const result = parseHotkey(hotkey);
  if (!result) {
    return false;
  }

  const {key, allowInEditable} = result;
  const {target = null} = event;

  const inEditable = elementIsEditable(target);
  if (inEditable && !allowInEditable) {
    return false;
  }
  return compareKeys(event, key) && checkMods(event, result, spoofPlatform);
}

// Return true if |key| matches the event's key.
function compareKeys(e: KeyboardEventLike, key: Key): boolean {
  return e.key.toLowerCase() === key.toLowerCase();
}

// Return true if modifiers specified in |mods| match those in the event.
function checkMods(
  event: KeyboardEventLike,
  hotkey: HotkeyParts,
  spoofPlatform?: Platform,
): boolean {
  const platform = spoofPlatform ?? getPlatform();

  const {key, modifier} = hotkey;

  const {
    ctrlKey = false,
    altKey = false,
    shiftKey = false,
    metaKey = false,
  } = event;

  const wantShift = modifier.includes('Shift');
  const wantAlt = modifier.includes('Alt');
  const wantCtrl =
    platform === 'Mac'
      ? modifier.includes('Ctrl')
      : modifier.includes('Ctrl') || modifier.includes('Mod');
  const wantMeta = platform === 'Mac' && modifier.includes('Mod');

  // For certain keys we relax the shift requirement, as they usually cannot be
  // pressed without the shift key on English keyboards.
  const shiftOk =
    shiftExceptions.includes(key as string) || shiftKey === wantShift;

  return (
    metaKey === wantMeta &&
    Boolean(shiftOk) &&
    altKey === wantAlt &&
    ctrlKey === wantCtrl
  );
}

export type Platform = 'Mac' | 'PC';

// Get the current platform (PC or Mac).
export function getPlatform(): Platform {
  return window.navigator.platform.indexOf('Mac') !== -1 ? 'Mac' : 'PC';
}

// Returns a cross-platform check for whether the event has "Mod" key pressed
// (e.g. as a part of Mod-Click UX pattern).
// On Mac, Mod-click is actually Command-click and on PC it's Control-click,
// so this function handles this for all platforms.
export function hasModKey(event: {
  readonly metaKey: boolean;
  readonly ctrlKey: boolean;
}): boolean {
  if (getPlatform() === 'Mac') {
    return event.metaKey;
  } else {
    return event.ctrlKey;
  }
}

export function modKey(): {metaKey?: boolean; ctrlKey?: boolean} {
  if (getPlatform() === 'Mac') {
    return {metaKey: true};
  } else {
    return {ctrlKey: true};
  }
}
