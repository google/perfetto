// Copyright (C) 2021 The Android Open Source Project
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

import {duration, time} from '../base/time';
import {Color} from '../common/colorizer';

export interface Slice {
  // These properties are updated only once per query result when the Slice
  // object is created and don't change afterwards.
  readonly id: number;
  readonly startNsQ: time;
  readonly endNsQ: time;
  readonly durNsQ: duration;
  readonly ts: time;
  readonly dur: duration;
  readonly depth: number;
  readonly flags: number;

  // These can be changed by the Impl.
  title: string;
  subTitle: string;
  baseColor: Color;
  color: Color;
}
