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

import {ErrorDetails} from '../base/logging';

export type TraceCategories = 'Trace Actions' | 'Record Trace' | 'User Actions';

/**
 * Logs analytics events and errors.
 *
 * Use this to track user actions, trace operations, and error conditions.
 * Events are categorized (e.g., 'Trace Actions', 'User Actions') and can
 * be used to understand how users interact with your plugin.
 */
export interface Analytics {
  /**
   * Logs a generic analytics event.
   *
   * @param category The category of the event (e.g., 'Trace Actions').
   * @param event The name of the event (e.g., 'Save trace').
   */
  logEvent(category: TraceCategories | null, event: string): void;

  /**
   * Logs an error event.
   *
   * @param err The error details to log.
   */
  logError(err: ErrorDetails): void;

  /**
   * Checks if analytics is enabled.
   *
   * @returns `true` if analytics is enabled, `false` otherwise.
   */
  isEnabled(): boolean;
}
