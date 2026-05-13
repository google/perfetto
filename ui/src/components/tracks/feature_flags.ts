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

import {featureFlags} from '../../core/feature_flags';

export const CHUNKED_TASK_BACKGROUND_PRIORITY = featureFlags.register({
  id: 'trackBackgroundDataLoading',
  name: 'Load track data in the background',
  description: `When enabled, track data is loaded using background priority
    tasks. This can help keep the UI responsive during heavy data loading
    but may increase the time it takes for tracks to appear.`,
  defaultValue: false,
});
