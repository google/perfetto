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

import {SimpleProcessMetricHandler} from './simpleProcessMetricHandler';

export const pinActivityOrBinderLeaksMetricsInstance =
  new SimpleProcessMetricHandler(
    [
      /(?<processName>.*)_Activities-last-first-diff/,
      /(?<processName>.*)_View-last-first-diff/,
      /(?<processName>.*)_ViewRootImpl-last-first-diff/,
      /(?<processName>.*)_Local Binders-last-first-diff/,
      /(?<processName>.*)_Proxy Binders-last-first-diff/,
    ],
    ['Heap size'],
  );
