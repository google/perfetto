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

// Check if a mithril component vnode has children
export function hasChildren<T>({children}: m.Vnode<T>): boolean {
  return (
    Array.isArray(children) &&
    children.length > 0 &&
    children.some((value) => value)
  );
}

// A component which simply passes through it's children.
// Can be used for having something to attach lifecycle hooks to without having
// to add an extra HTML element to the DOM.
export const Passthrough = {
  view({children}: m.VnodeDOM) {
    return children;
  },
};

export interface GateAttrs {
  open: boolean;
}

// The gate component is a wrapper which can either be open or closed.
// - When open, children are rendered inside a div where display = contents.
// - When closed, children are rendered inside a div where display = none, and
//   children's view functions are not called.
//
// Use this component when we want to conditionally render certain children, but
// we want to retain their state, such as page and tab views.
export class Gate implements m.ClassComponent<GateAttrs> {
  private previousChildren: m.Children;
  private wasOpen?: boolean;

  view({attrs, children}: m.Vnode<GateAttrs>) {
    return m(
      '',
      {
        style: {display: attrs.open ? 'contents' : 'none'},
      },
      this.renderChildren(attrs.open, children),
    );
  }

  private renderChildren(open: boolean, children: m.Children) {
    // If the gate is open, pass the latest children through, otherwise pass the
    // cached children through. When Mithril sees the same children as in the
    // previous render cycle, it doesn't re-render those children. This is a
    // performance optimization, as children that are not visible typically
    // don't need to be re-rendered.
    //
    // Note: Render the children once more after the gate has been closed, which
    // allows out-of-tree elements like popups to close properly, as the
    // display: none doesn't apply to them.
    if (open || this.wasOpen) {
      this.previousChildren = children;
    }
    this.wasOpen = open;
    return this.previousChildren;
  }
}

export type MithrilEvent<T extends Event = Event> = T & {redraw: boolean};

/**
 * Converts a string input in a <span>, extracts URLs and converts them into
 * clickable links.
 * @param text the input string, e.g., "See https://example.org for details".
 * @returns a Mithril vnode, e.g.
 *    <span>See <a href="https://example.org">example.org<a> for more details.
 */
export function linkify(text: string): m.Children {
  const urlPattern = /(https?:\/\/[^\s]+)|(go\/[^\s]+)/g;
  const parts = text.split(urlPattern);
  return m(
    'span',
    parts.map((part) => {
      if (/^(https?:\/\/[^\s]+)$/.test(part)) {
        return m('a', {href: part, target: '_blank'}, part.split('://')[1]);
      } else if (/^(go\/[^\s]+)$/.test(part)) {
        return m(
          'a',
          {
            href: `http://${part}`,
            target: '_blank',
          },
          part,
        );
      } else {
        return part;
      }
    }),
  );
}
