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

import type {HeaderItem, HeaderManager} from '../public/header';

/** Keeps the header items, which ui_main.ts renders above the topbar. */
export class HeaderManagerImpl implements HeaderManager {
  // Keyed by registration, so that the UI can key the rendered items.
  private readonly registry = new Map<number, HeaderItem>();
  private nextKey = 0;

  registerItem(item: HeaderItem): Disposable {
    const key = this.nextKey++;
    this.registry.set(key, item);
    return {
      [Symbol.dispose]: () => {
        this.registry.delete(key);
      },
    };
  }

  // The registered items by key, in registration order.
  get items(): ReadonlyMap<number, HeaderItem> {
    return this.registry;
  }
}
