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
// - When closed, children are rendered inside a div where display = none
// Use this component when we want to conditionally render certain children,
// but we want to maintain their state.
export const Gate = {
  view({attrs, children}: m.VnodeDOM<GateAttrs>) {
    return m(
      '',
      {
        style: {display: attrs.open ? 'contents' : 'none'},
      },
      children,
    );
  },
};
