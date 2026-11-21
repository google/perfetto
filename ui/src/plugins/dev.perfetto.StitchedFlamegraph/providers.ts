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

import {FLOWS_PROVIDER} from './flows';
import type {StitchedMetricProvider} from './metrics';

// Every source of stitched flamegraph metrics, in the order their metrics
// are listed in the measure picker.
export const STITCHED_PROVIDERS: ReadonlyArray<StitchedMetricProvider> = [
  FLOWS_PROVIDER,
];
