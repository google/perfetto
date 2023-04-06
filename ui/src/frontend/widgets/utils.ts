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

// Check whether a DOM element contains another, or whether they're the same
export function isOrContains(container: Element, target: Element): boolean {
  return container === target || container.contains(target);
}

// Find a DOM element with a given "ref" attribute
export function findRef(root: Element, ref: string): Element|null {
  const query = `[ref=${ref}]`;
  if (root.matches(query)) {
    return root;
  } else {
    return root.querySelector(query);
  }
}

// Safely case an Element to an HTMLElement.
// Throws if the element is not an HTMLElement.
export function toHTMLElement(el: Element): HTMLElement {
  if (!(el instanceof HTMLElement)) {
    throw new Error('Element is not an HTLMElement');
  }
  return el as HTMLElement;
}

// Check if a mithril component vnode has children
export function hasChildren({children}: m.Vnode<any>): boolean {
  return Array.isArray(children) && children.length > 0;
}
