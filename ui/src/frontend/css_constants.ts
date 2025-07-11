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
export let TRACK_BORDER_COLOR = '#ffc0cb';
export let TOPBAR_HEIGHT = 48;
export let SELECTION_STROKE_COLOR = '#00344596';
export let SELECTION_FILL_COLOR = '#8398e64d';
export let OVERVIEW_TIMELINE_NON_VISIBLE_COLOR = '#c8c8c8cc';
export let DEFAULT_DETAILS_CONTENT_HEIGHT = 308;
export let BACKGROUND_COLOR = '#ffffff';
export let FOREGROUND_COLOR = '#121212';
export let DIVIDER_COLOR = '#999';
export let TRACK_COLLAPSED_BACKGROUND = '#ffffff';
export let TRACK_EXPANDED_BACKGROUND = '#ffffff';
export let FONT_NAME = 'Roboto Condensed';
export let TRACK_LOADING_BACKGROUND = '#eee';
export let TRACK_LOADING_FOREGROUND = '#666';
export let TRACK_LEGEND_BACKGROUND = 'rgba(255, 255, 255, 0.6)';
export let TRACK_LEGEND_FOREGROUND = '#666';
export let TRACK_HISTOGRAM_NEUTRAL_FILL_COLOR = 'rgba(240, 240, 240, 1)';
export let TRACK_HATCH_PATTERN_COLOR = 'rgba(255, 255, 255, 0.3)';
export let NOTES_PANEL_TEXT_COLOR = '#3c4b5d';
export let NOTES_PANEL_NOTE_TEXT_BACKGROUND = 'rgba(255, 255, 255, 0.8)';

export function initCssConstants() {
  TRACK_SHELL_WIDTH = getCssNum('--pf-track-shell-width') ?? TRACK_SHELL_WIDTH;
  SIDEBAR_WIDTH = getCssNum('--pf-sidebar-width') ?? SIDEBAR_WIDTH;
  TRACK_BORDER_COLOR = getCssStr('--pf-track-border-color') ?? TRACK_BORDER_COLOR;
  TOPBAR_HEIGHT = getCssNum('--pf-topbar-height') ?? TOPBAR_HEIGHT;
  SELECTION_STROKE_COLOR =
    getCssStr('--pf-selection-stroke-color') ?? SELECTION_STROKE_COLOR;
  SELECTION_FILL_COLOR =
    getCssStr('--pf-selection-fill-color') ?? SELECTION_FILL_COLOR;
  OVERVIEW_TIMELINE_NON_VISIBLE_COLOR =
    getCssStr('--pf-overview-timeline-non-visible-color') ??
    OVERVIEW_TIMELINE_NON_VISIBLE_COLOR;
  DEFAULT_DETAILS_CONTENT_HEIGHT =
    getCssNum('--pf-details-content-height') ?? DEFAULT_DETAILS_CONTENT_HEIGHT;
  BACKGROUND_COLOR = getCssStr('--pf-viewer-background') ?? BACKGROUND_COLOR;
  FOREGROUND_COLOR = getCssStr('--pf-main-color') ?? FOREGROUND_COLOR;
  DIVIDER_COLOR = getCssStr('--pf-viewer-dividing-line-color') ?? DIVIDER_COLOR;
  TRACK_COLLAPSED_BACKGROUND =
    getCssStr('--pf-track-collapsed-background') ?? TRACK_COLLAPSED_BACKGROUND;
  TRACK_EXPANDED_BACKGROUND =
    getCssStr('--pf-track-expanded-background') ?? TRACK_EXPANDED_BACKGROUND;
  FONT_NAME = getCssStr('--pf-font') ?? FONT_NAME;
  TRACK_LOADING_BACKGROUND = getCssStr('--pf-track-loading-background') ?? TRACK_LOADING_BACKGROUND;
  TRACK_LOADING_FOREGROUND = getCssStr('--pf-track-loading-color') ?? TRACK_LOADING_FOREGROUND;
  TRACK_LEGEND_BACKGROUND = getCssStr('--pf-track-legend-background') ?? TRACK_LEGEND_BACKGROUND;
  TRACK_LEGEND_FOREGROUND = getCssStr('--pf-track-legend-color') ?? TRACK_LEGEND_FOREGROUND;
  TRACK_HISTOGRAM_NEUTRAL_FILL_COLOR = getCssStr('--pf-track-histogram-neutral-color') ?? TRACK_HISTOGRAM_NEUTRAL_FILL_COLOR;
  TRACK_HATCH_PATTERN_COLOR = getCssStr('--pf-track-hatch-pattern-color') ?? TRACK_HATCH_PATTERN_COLOR;
  NOTES_PANEL_TEXT_COLOR = getCssStr('--pf-notes-panel-color') ?? NOTES_PANEL_TEXT_COLOR;
  NOTES_PANEL_NOTE_TEXT_BACKGROUND = getCssStr('--pf-notes-panel-note-text-background') ?? NOTES_PANEL_NOTE_TEXT_BACKGROUND;
}

function getCssStr(prop: string): string | undefined {
  if (typeof window === 'undefined') return undefined;
  const body = window.document.body;
  const value = window.getComputedStyle(body).getPropertyValue(prop);
  // Note: getPropertyValue() returns an empty string if not set
  // https://developer.mozilla.org/en-US/docs/Web/API/CSSStyleDeclaration/getPropertyValue#return_value
  return value === '' ? undefined : value;
}

function getCssNum(prop: string): number | undefined {
  const str = getCssStr(prop);
  if (str === undefined) return undefined;
  const match = str.match(/^\W*(\d+)px(|\!important')$/);
  if (!match) throw Error(`Could not parse CSS property "${str}" as a number`);
  return Number(match[1]);
}
