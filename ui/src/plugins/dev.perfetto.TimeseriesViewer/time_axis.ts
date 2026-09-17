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

import {Duration, type duration, type time} from '../../base/time';

// A human-readable label for a time expressed as an offset from a reference
// (e.g. the zoom-box duration, or the trace start). Using an offset rather than
// a raw boot timestamp makes magnitudes easy to read.
export function timeTickLabel(t: time, reference: time): string {
  const off = (t - reference) as duration;
  if (off === 0n) return '0';
  return Duration.humanise(off);
}
