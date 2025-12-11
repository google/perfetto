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
// Also we cannot have global constructors because when the javascript is
// loaded, the CSS might not be ready yet.
export let TRACK_SHELL_WIDTH = 100;
export let DEFAULT_DETAILS_CONTENT_HEIGHT = 308;

export let FONT_COMPACT = '"Roboto Condensed", sans-serif';

export let COLOR_BORDER = 'hotpink';
export let COLOR_BORDER_SECONDARY = 'hotpink';
export let COLOR_BACKGROUND_SECONDARY = 'hotpink';
export let COLOR_ACCENT = 'hotpink';
export let COLOR_BACKGROUND = 'hotpink';
export let COLOR_TEXT = 'hotpink';
export let COLOR_TEXT_MUTED = 'hotpink';
export let COLOR_NEUTRAL = 'hotpink';
export let COLOR_HIGHLIGHT = 'hotpink';
export let COLOR_TIMELINE_OVERLAY = 'hotpink';

export function initCssConstants(element?: Element) {
  function getCssStr(prop: string): string | undefined {
    if (typeof window === 'undefined') return undefined;
    const searchElement = element ?? window.document.body;
    const value = window.getComputedStyle(searchElement).getPropertyValue(prop);
    // Note: getPropertyValue() returns an empty string if not set
    // https://developer.mozilla.org/en-US/docs/Web/API/CSSStyleDeclaration/getPropertyValue#return_value
    return value === '' ? undefined : value;
  }

  function getCssNum(prop: string): number | undefined {
    const str = getCssStr(prop);
    if (str === undefined) return undefined;
    const match = str.match(/^\W*(\d+)px(|\!important')$/);
    if (!match) {
      throw Error(`Could not parse CSS property "${str}" as a number`);
    }
    return Number(match[1]);
  }

  TRACK_SHELL_WIDTH = getCssNum('--track-shell-width') ?? TRACK_SHELL_WIDTH;
  COLOR_BORDER = getCssStr('--pf-color-border') ?? COLOR_BORDER;
  COLOR_BORDER_SECONDARY =
    getCssStr('--pf-color-border-secondary') ?? COLOR_BORDER_SECONDARY;
  COLOR_BACKGROUND_SECONDARY =
    getCssStr('--pf-color-background-secondary') ?? COLOR_BACKGROUND_SECONDARY;
  COLOR_ACCENT = getCssStr('--pf-color-accent') ?? COLOR_ACCENT;
  DEFAULT_DETAILS_CONTENT_HEIGHT =
    getCssNum('--details-content-height') ?? DEFAULT_DETAILS_CONTENT_HEIGHT;
  COLOR_BACKGROUND = getCssStr('--pf-color-background') ?? COLOR_BACKGROUND;
  COLOR_TEXT = getCssStr('--pf-color-text') ?? COLOR_TEXT;
  FONT_COMPACT = getCssStr('--pf-font-compact') ?? FONT_COMPACT;
  COLOR_TEXT_MUTED = getCssStr('--pf-color-text-muted') ?? COLOR_TEXT_MUTED;
  COLOR_NEUTRAL = getCssStr('--pf-color-neutral') ?? COLOR_NEUTRAL;
  COLOR_HIGHLIGHT = getCssStr('--pf-color-highlight') ?? COLOR_HIGHLIGHT;
  COLOR_TIMELINE_OVERLAY =
    getCssStr('--pf-color-timeline-overlay') ?? COLOR_TIMELINE_OVERLAY;
}
