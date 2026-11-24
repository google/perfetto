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

// Runtime half of Mithril component hot module replacement (dev server only).
//
// The pluginMithrilHmr Vite plugin (ui/vite.config.mjs) rewrites modules whose
// only runtime exports are Mithril components (classes or closure components)
// so that each exported component is passed through hmrWrapComponent().
// Importers therefore hold a stable proxy rather than the component itself.
// When the module is hot-updated, the proxy is repointed at the new component
// and the whole Mithril tree is remounted, so every component instance is
// rebuilt from the new code (constructor, fields, closures and methods all
// consistent). App state that lives outside the component tree (e.g. the
// loaded trace) is preserved.

import {raf} from './raf_scheduler';

type HotContext = NonNullable<ImportMeta['hot']>;
type ComponentClass = abstract new (...args: never[]) => unknown;
type ClosureComponent = (...args: never[]) => unknown;
type ObjectComponent = object;
type HmrComponent = ComponentClass | ClosureComponent | ObjectComponent;

interface HmrEntry {
  latest: HmrComponent;
  proxy: HmrComponent;
}

const HOT_DATA_KEY = 'mithrilHmrEntries';

let remountScheduled = false;

// Several modules may be updated in the same HMR batch; coalesce them into a
// single remount.
function scheduleRemount() {
  if (remountScheduled) return;
  remountScheduled = true;
  setTimeout(() => {
    remountScheduled = false;
    raf.remountAll();
  });
}

export function hmrWrapComponent<T extends HmrComponent>(
  hot: HotContext | undefined,
  name: string,
  component: T,
): T {
  if (!hot) return component;

  const data = hot.data as Record<string, Record<string, HmrEntry>>;
  const entries = (data[HOT_DATA_KEY] ??= {});
  const existing: HmrEntry | undefined = entries[name];
  if (existing !== undefined) {
    // Module re-executed due to a hot update: repoint the proxy at the new
    // component and rebuild the tree.
    existing.latest = component;
    scheduleRemount();
    return existing.proxy as T;
  }

  const entry: HmrEntry = {latest: component, proxy: component};
  // The proxy target must be a callable and constructible function whose
  // `prototype` is writable: proxy invariants forbid the get trap from
  // reporting a different `prototype` if the target's one is read-only (as it
  // is for classes).
  //
  // Mithril picks the component kind from the tag on every instantiation, and
  // all the properties it inspects are read through the get trap, so this
  // follows whatever `latest` is:
  //  - `tag.view` is a function: object component, state = Object.create(tag).
  //  - `tag.prototype.view` is a function: class component, `new tag(vnode)`.
  //  - Otherwise: closure component, `tag(vnode)`.
  //
  // For object components the per-instance state inherits from the proxy, so
  // property reads/writes on the state reach the get/set traps with the state
  // as the receiver. The receiver is forwarded so that e.g. `this.count = 1`
  // lands on the instance, not on the shared component object. Accesses made
  // directly on the proxy (e.g. statics) are redirected to `latest`.
  const target = function () {};
  const recv = (receiver: unknown) =>
    receiver === entry.proxy ? entry.latest : receiver;
  entry.proxy = new Proxy(target, {
    get: (_, key, receiver) => Reflect.get(entry.latest, key, recv(receiver)),
    set: (_, key, value, receiver) =>
      Reflect.set(entry.latest, key, value, recv(receiver)),
    has: (_, key) => Reflect.has(entry.latest, key),
    construct: (_, args, newTarget) => {
      const latest = entry.latest as ComponentClass;
      return Reflect.construct(
        latest,
        args,
        newTarget === entry.proxy ? latest : newTarget,
      );
    },
    apply: (_, thisArg, args) =>
      Reflect.apply(entry.latest as ClosureComponent, thisArg, args),
  }) as unknown as HmrComponent;
  entries[name] = entry;
  return entry.proxy as T;
}
