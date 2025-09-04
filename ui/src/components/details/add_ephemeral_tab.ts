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
import {uuidv4} from '../../base/uuid';
import {Tab} from '../../public/tab';
import {Trace} from '../../public/trace';

// TODO(primiano): this method probably shouldn't exist at all in favour
// of some helper in the Trace object).
export function addEphemeralTab(
  trace: Trace,
  uriPrefix: string,
  tab: Tab,
): void {
  const uri = `${uriPrefix}#${uuidv4()}`;

  const tabManager = trace.tabs;
  if (tabManager === undefined) return;
  tabManager.registerTab({
    uri,
    content: tab,
    isEphemeral: true,
  });
  tabManager.showTab(uri);
}
