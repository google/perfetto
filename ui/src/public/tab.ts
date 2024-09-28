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
import {DetailsPanel} from './details_panel';

export interface TabManager {
  registerTab(tab: TabDescriptor): void;
  registerDetailsPanel(detailsPanel: DetailsPanel): Disposable;
  showTab(uri: string): void;
  hideTab(uri: string): void;
  addDefaultTab(uri: string): void;
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
