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

import {CurrentSelectionSection, TabDescriptor} from '../public';

export class TabManager {
  private _registry = new Map<string, TabDescriptor>();
  private _currentSelectionSectionReg = new Set<CurrentSelectionSection>();

  registerTab(desc: TabDescriptor): void {
    this._registry.set(desc.uri, desc);
  }

  unregisterTab(uri: string): void {
    this._registry.delete(uri);
  }

  registerCurrentSelectionSection(section: CurrentSelectionSection): void {
    this._currentSelectionSectionReg.add(section);
  }

  unregisterCurrentSelectionSection(section: CurrentSelectionSection): void {
    this._currentSelectionSectionReg.delete(section);
  }

  resolveTab(uri: string): TabDescriptor|undefined {
    return this._registry.get(uri);
  }

  get tabs(): TabDescriptor[] {
    return Array.from(this._registry.values());
  }

  get currentSelectionSections(): CurrentSelectionSection[] {
    return Array.from(this._currentSelectionSectionReg);
  }
}
