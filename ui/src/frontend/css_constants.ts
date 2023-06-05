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
export let SIDEBAR_WIDTH = 100;
export let TOPBAR_HEIGHT = 48;
export let SELECTION_STROKE_COLOR = '#00344596';
export let SELECTION_FILL_COLOR = '#8398e64d';
export let OVERVIEW_TIMELINE_NON_VISIBLE_COLOR = '#c8c8c8cc';
export let DEFAULT_DETAILS_CONTENT_HEIGHT = 280;
export const SELECTED_LOG_ROWS_COLOR = '#D2EFE0';

export function initCssConstants() {
  TRACK_SHELL_WIDTH = getCssNum('--track-shell-width') || TRACK_SHELL_WIDTH;
  SIDEBAR_WIDTH = getCssNum('--sidebar-width') || SIDEBAR_WIDTH;
  TOPBAR_HEIGHT = getCssNum('--topbar-height') || TOPBAR_HEIGHT;
  SELECTION_STROKE_COLOR =
      getCssStr('--selection-stroke-color') || SELECTION_STROKE_COLOR;
  SELECTION_FILL_COLOR =
      getCssStr('--selection-fill-color') || SELECTION_FILL_COLOR;
  OVERVIEW_TIMELINE_NON_VISIBLE_COLOR =
      getCssStr('--overview-timeline-non-visible-color') ||
      OVERVIEW_TIMELINE_NON_VISIBLE_COLOR;
  DEFAULT_DETAILS_CONTENT_HEIGHT =
      getCssNum('--details-content-height') || DEFAULT_DETAILS_CONTENT_HEIGHT;
}

export function getCssStr(prop: string): string {
  if (typeof window === 'undefined') return '';
  const doc = window.document.documentElement;
  return window.getComputedStyle(doc).getPropertyValue(prop).trim();
}

function getCssNum(prop: string): number|undefined {
  const str = getCssStr(prop);
  if (str === undefined) return undefined;
  const match = str.match(/^\W*(\d+)px(|\!important')$/);
  if (!match) throw Error(`Could not parse CSS property "${str}" as a number`);
  return Number(match[1]);
}
