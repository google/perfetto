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

import {z} from 'zod';
import {LocalStorage} from '../../core/local_storage';
import {
  LocalSettingsStorage,
  BIGTRACE_SETTINGS_STORAGE_KEY,
} from './settings_storage';

export const endpointStorage = new LocalSettingsStorage(
  new LocalStorage(BIGTRACE_SETTINGS_STORAGE_KEY),
);

endpointStorage.register({
  id: 'bigtraceEndpoint',
  name: 'BigTrace Endpoint',
  description: 'The URL of the BigTrace backend service.',
  schema: z.string(),
  defaultValue: 'https://brush-googleapis.corp.google.com/v1',
  requiresReload: true,
});
