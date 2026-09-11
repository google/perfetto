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

import {SingleFieldStorage} from './single_field_storage';

// Id of the preset the last new query started from, so the launcher can
// preselect it. Empty when the last query was configured by hand.
export const lastPresetIdState = new SingleFieldStorage<string>(
  'bigtraceLastPreset',
  'id',
  (raw) => (typeof raw === 'string' ? raw : ''),
  '',
);
