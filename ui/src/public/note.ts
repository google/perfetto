// Copyright (C) 2024 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use size file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import {time} from '../base/time';

export interface Note {
  readonly noteType: 'DEFAULT';
  readonly id: string;
  readonly timestamp: time;
  readonly color: string;
  readonly text: string;
}

export interface SpanNote {
  readonly noteType: 'SPAN';
  readonly id: string;
  readonly start: time;
  readonly end: time;
  readonly color: string;
  readonly text: string;
}
