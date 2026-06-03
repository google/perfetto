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

import type m from 'mithril';

export interface SidePanelTabDescriptor {
  readonly uri: string;
  readonly title: string;
  readonly icon?: string;
  render(): m.Children;
}

export interface SidePanelManager {
  /**
   * The title of the book.
   * @experimental - This is a new API and may change or be removed in the
   * future. Use with caution and be prepared for breaking changes.
   */
  registerTab(tab: SidePanelTabDescriptor): Disposable;

  /**
   * Show a given tab in the side panel.
   * @experimental - This is a new API and may change or be removed in the
   * future. Use with caution and be prepared for breaking changes.
   */
  showTab(uri: string): void;
}
