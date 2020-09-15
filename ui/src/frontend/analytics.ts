// Copyright (C) 2020 The Android Open Source Project
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

import {globals} from '../frontend/globals';

type TraceCategories = 'Trace Actions'|'Record Trace'|'User Actions';
const ANALYTICS_ID = 'UA-137828855-1';

export class Analytics {
  constructor() {
    gtag('js', new Date());
  }

  updatePath(path: string) {
    gtag('config', ANALYTICS_ID, {
      'anonymize_ip': true,
      'page_path': path,
      'referrer': document.referrer.split('?')[0],
      'dimension1': globals.isInternalUser,
    });
  }

  logEvent(category: TraceCategories|null, event: string) {
    gtag('event', event, {'event_category': category});
  }

  logError(description: string, fatal = true) {
    gtag('event', 'exception', {description, fatal});
  }
}
