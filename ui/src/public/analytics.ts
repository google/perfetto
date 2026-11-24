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

import type {ErrorDetails} from '../base/logging';

// An event that any plugin can send with Analytics.logEvent(). Each kind of
// event has a unique `kind`, which is how code tells them apart.
export type BasicAnalyticsEvent = PageViewEvent | ActionEvent;

// The user opened a page, e.g. '/viewer' ('' for the home page).
export interface PageViewEvent {
  readonly kind: 'page_view';
  readonly page: string;
}

// The user did something, e.g. 'Download trace'.
export interface ActionEvent {
  readonly kind: 'action';
  // Groups the action in reports. Can be left out if none of these fit.
  readonly category?: 'Trace Actions' | 'Record Trace' | 'User Actions';
  readonly action: string;
}

export interface Analytics {
  logEvent(event: BasicAnalyticsEvent): void;
  logError(err: ErrorDetails): void;
  isEnabled(): boolean;
}
