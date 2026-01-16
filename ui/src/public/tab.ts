// Copyright (C) 2024 The Android Open Source Project
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
import z from 'zod';

export interface GenericTabInstance {
  // Id of the tab renderer instance
  readonly tabId: string;

  // Unique ID for this tab instance
  readonly id: string;

  // This tab's configuration
  readonly config: unknown;
}

export interface GenericTab<T> {
  readonly id: string;
  readonly render: (id: string, config: T) => m.Children;
  readonly schema: z.ZodType<T>;
}

export interface TabManager {
  registerTab(tab: TabDescriptor): void;
  showTab(uri: string): void;
  hideTab(uri: string): void;
  addDefaultTab(uri: string): void;
  registerGenericTab<T>(tab: GenericTab<T>): void;
  openGenericTab(config: GenericTabInstance): void;
}

export interface Tab {
  render(): m.Children;
  getTitle(): string;
}

export interface TabDescriptor {
  uri: string; // TODO(stevegolton): Maybe optional for ephemeral tabs.
  content: Tab;
  isEphemeral?: boolean; // Defaults false
  onHide?(): void;
  onShow?(): void;
}
