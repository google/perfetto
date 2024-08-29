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

// This file contains constants for some command IDs that are used directly
// from frontend code (e.g. the details panel that has buttons for critical
// path). They exist to deal with all cases where some feature cannot be done
// just with the existing API (e.g. the command palette), and a more direct
// coupling between frontend and commands is necessary.
// Adding entries to this file usually is the symptom of a missing API in the
// plugin surface (e.g. the ability to customize context menus).
// These constants exist just to make the dependency evident at code
// search time, rather than copy-pasting the string in two places.

export const CRITICAL_PATH_CMD = 'perfetto.CriticalPath';
export const CRITICAL_PATH_LITE_CMD = 'perfetto.CriticalPathLite';
