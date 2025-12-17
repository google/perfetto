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

export function childrenValid(children: m.Children): boolean {
  if (children === null || children === undefined) return false;
  if (Array.isArray(children)) {
    return children.length > 0 && children.some(childrenValid);
  }
  return true;
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

// Check if a mithril children is empty (null, undefined, or an empty array). If
// it is any of these, mithril will not render anything. Useful for when we want
// to optionally avoid rendering a wrapper for some children for instance.
export function isEmptyVnodes(children: m.Children): boolean {
  if (children === null || children === undefined) return true;
  if (Array.isArray(children)) {
    return children.length === 0 || children.every(isEmptyVnodes);
  }
  return false;
}
