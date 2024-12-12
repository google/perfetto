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

export interface FtraceFilter {
  // We use an exclude list rather than include list for filtering events, as we
  // want to include all events by default but we won't know what names are
  // present initially.
  excludeList: string[];
}

export interface FtracePluginState {
  version: number;
  filter: FtraceFilter;
}

export interface FtraceStat {
  name: string;
  count: number;
}
