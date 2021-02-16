// Copyright (C) 2019 The Android Open Source Project
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

// This code can be used in unittests where we can't read CSS variables.
// Also we cannot have global constructors beacause when the javascript is
// loaded, the CSS might not be ready yet.
export let TRACK_SHELL_WIDTH = 100;
export let TRACK_BORDER_COLOR = '#ffc0cb';
export let TOPBAR_HEIGHT = 48;

export function initCssConstants() {
  TRACK_SHELL_WIDTH = getCssNum('--track-shell-width') || TRACK_SHELL_WIDTH;
  TRACK_BORDER_COLOR = getCssStr('--track-border-color') || TRACK_BORDER_COLOR;
  TOPBAR_HEIGHT = getCssNum('--topbar-height') || TOPBAR_HEIGHT;
}

function getCssStr(prop: string): string|undefined {
  if (typeof window === 'undefined') return undefined;
  const body = window.document.body;
  return window.getComputedStyle(body).getPropertyValue(prop);
}

function getCssNum(prop: string): number|undefined {
  const str = getCssStr(prop);
  if (str === undefined) return undefined;
  const match = str.match(/^\W*(\d+)px$/);
  if (!match) throw Error(`Could not parse CSS property "${str}" as a number`);
  return Number(match[1]);
}
