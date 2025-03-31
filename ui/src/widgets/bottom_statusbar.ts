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

/**
 * Attributes for the StatusBar component.
 */
export interface StatusbarAttrs {
  // Unique key to identify this specific status bar instance,
  // useful for closing the correct one if multiple are shown in sequence.
  key?: string;
  // Content to be displayed within the status bar.
  // Can be direct Mithril children or a function returning children.
  content?: m.Children | (() => m.Children);
}

/**
 * A persistent status bar component typically rendered at the bottom of the UI.
 * It replaces the previous status bar content when shown.
 */
export class StatusBar implements m.ClassComponent<StatusbarAttrs> {
  view(vnode: m.Vnode<StatusbarAttrs>) {
    return m(
      '.perfetto-statusbar',
      vnode.children,
    );
  }
}

let currentStatusbar: StatusbarAttrs | undefined = undefined;

/**
 * Renders the current status bar component if one is active.
 * @returns An array containing the StatusBar Vnode if active, otherwise empty.
 */
export function maybeRenderStatusbar(): m.Vnode<StatusbarAttrs>[] {
  if (currentStatusbar === undefined) {
    return [];
  }

  let children: m.Children;
  if (currentStatusbar.content === undefined) {
    children = null;
  } else if (typeof currentStatusbar.content === 'function') {
    children = currentStatusbar.content();
  } else {
    children = currentStatusbar.content;
  }

  return [m(StatusBar, currentStatusbar, children)];
}

/**
 * Displays or updates the application's status bar.
 * If a status bar is already visible, its content and attributes are replaced.
 * @param userAttrs Attributes defining the status bar's content and key.
 */
export function showStatusbar(userAttrs: StatusbarAttrs): void {
  currentStatusbar = userAttrs;
  m.redraw();
}

/**
 * Requests a redraw if the status bar is currently active.
 * Useful if the status bar content (generated by a function) needs refreshing
 * due to external state changes.
 */
export function redrawStatusbar(): void {
  if (currentStatusbar !== undefined) {
    m.redraw();
  }
}

/**
 * Closes the currently displayed status bar.
 * @param key Optional. If provided, only closes the status bar if its key matches.
 * This prevents accidentally closing a different status bar instance
 * that might have been shown subsequently. If undefined, closes
 * whichever status bar is currently open.
 */
export function closeStatusbar(key?: string): void {
  if (currentStatusbar === undefined) {
    return;
  }

  if (key !== undefined && currentStatusbar.key !== key) {
    return;
  }

  currentStatusbar = undefined;
  m.redraw();
}
