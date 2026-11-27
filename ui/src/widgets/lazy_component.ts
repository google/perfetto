// Copyright (C) 2026 The Android Open Source Project
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
 * Returns a component that renders the one |load| resolves to, so that
 * component's code is only fetched when it is first shown. It renders
 * nothing until then.
 */
export function lazyComponent<Attrs>(
  load: () => Promise<m.ComponentTypes<Attrs>>,
): m.Component<Attrs> {
  let component: m.ComponentTypes<Attrs> | undefined;
  let loading: Promise<void> | undefined;
  return {
    view(vnode) {
      if (component === undefined) {
        loading ??= load().then((c) => {
          component = c;
          m.redraw();
        });
        return undefined;
      }
      // m() can't check generic attrs; they pass through unchanged.
      const c = component as m.ComponentTypes<m.Attributes>;
      return m(c, vnode.attrs as m.Attributes, vnode.children);
    },
  };
}
