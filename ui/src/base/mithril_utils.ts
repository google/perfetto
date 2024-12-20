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

/**
 * Utility function to pre-bind some mithril attrs of a component, and leave
 * the others unbound and passed at run-time.
 * Example use case: the Page API Passes to the registered page a PageAttrs,
 * which is {subpage:string}. Imagine you write a MyPage component that takes
 * some extra input attrs (e.g. the App object) and you want to bind them
 * onActivate(). The results looks like this:
 *
 * interface MyPageAttrs extends PageAttrs { app: App; }
 *
 * class MyPage extends m.classComponent<MyPageAttrs> {... view() {...} }
 *
 * onActivate(app: App) {
 *   pages.register(... bindMithrilApps(MyPage, {app: app});
 * }
 *
 * The return value of bindMithrilApps is a mithril component that takes in
 * input only a {subpage: string} and passes down to MyPage the combination
 * of pre-bound and runtime attrs, that is {subpage, app}.
 */
export function bindMithrilAttrs<BaseAttrs, Attrs>(
  component: m.ComponentTypes<Attrs>,
  boundArgs: Omit<Attrs, keyof BaseAttrs>,
): m.Component<BaseAttrs> {
  return {
    view(vnode: m.Vnode<BaseAttrs>) {
      const attrs = {...vnode.attrs, ...boundArgs} as Attrs;
      const emptyAttrs: m.CommonAttributes<Attrs, {}> = {}; // Keep tsc happy.
      return m<Attrs, {}>(component, {...attrs, ...emptyAttrs});
    },
  };
}

export type MithrilEvent<T extends Event = Event> = T & {redraw: boolean};
