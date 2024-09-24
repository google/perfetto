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
import {uuidv4} from '../base/uuid';
import {BottomTab} from '../public/lib/bottom_tab';
import {globals} from '../frontend/globals';
import {Tab} from '../public/tab';
import {BottomTabToTabAdapter} from '../public/utils';

export function addEphemeralTab(uriPrefix: string, tab: Tab): void {
  const uri = `${uriPrefix}#${uuidv4()}`;

  globals.tabManager.registerTab({
    uri,
    content: tab,
    isEphemeral: true,
  });
  globals.tabManager.showTab(uri);
}

export function addBottomTab(tab: BottomTab, uriPrefix: string): void {
  const uri = `${uriPrefix}#${tab.uuid}`;

  globals.tabManager.registerTab({
    uri,
    content: new BottomTabToTabAdapter(tab),
    isEphemeral: true,
  });
  globals.tabManager.showTab(uri);
}
