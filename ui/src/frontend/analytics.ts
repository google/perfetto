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

import {getCurrentChannel} from '../common/channels';
import {VERSION} from '../gen/perfetto_version';

import {globals} from './globals';

type TraceCategories = 'Trace Actions'|'Record Trace'|'User Actions';
const ANALYTICS_ID = 'G-BD89KT2P3C';
const PAGE_TITLE = 'no-page-title';

export function initAnalytics() {
  // Only initialize logging on the official site and on localhost (to catch
  // analytics bugs when testing locally).
  // Skip analytics is the fragment has "testing=1", this is used by UI tests.
  // Skip analytics in embeddedMode since iFrames do not have the same access to
  // local storage.
  if ((window.location.origin.startsWith('http://localhost:') ||
       window.location.origin.endsWith('.perfetto.dev')) &&
      !globals.testing && !globals.embeddedMode) {
    return new AnalyticsImpl();
  }
  return new NullAnalytics();
}

const gtagGlobals = window as {} as {
  dataLayer: any[];
  gtag: (command: string, event: string|Date, args?: {}) => void;
};

export interface Analytics {
  initialize(): void;
  updatePath(_: string): void;
  logEvent(_x: TraceCategories|null, _y: string): void;
  logError(_x: string, _y?: boolean): void;
  isEnabled(): boolean;
}

export class NullAnalytics implements Analytics {
  initialize() {}
  updatePath(_: string) {}
  logEvent(_x: TraceCategories|null, _y: string) {}
  logError(_x: string) {}
  isEnabled(): boolean {
    return false;
  }
}

class AnalyticsImpl implements Analytics {
  private initialized_ = false;

  constructor() {
    // The code below is taken from the official Google Analytics docs [1] and
    // adapted to TypeScript. We have it here rather than as an inline script
    // in index.html (as suggested by GA's docs) because inline scripts don't
    // play nicely with the CSP policy, at least in Firefox (Firefox doesn't
    // support all CSP 3 features we use).
    // [1] https://developers.google.com/analytics/devguides/collection/gtagjs .
    gtagGlobals.dataLayer = gtagGlobals.dataLayer || [];

    function gtagFunction(..._: any[]) {
      // This needs to be a function and not a lambda. |arguments| behaves
      // slightly differently in a lambda and breaks GA.
      gtagGlobals.dataLayer.push(arguments);
    }
    gtagGlobals.gtag = gtagFunction;
    gtagGlobals.gtag('js', new Date());
  }

  // This is callled only after the script that sets isInternalUser loads.
  // It is fine to call updatePath() and log*() functions before initialize().
  // The gtag() function internally enqueues all requests into |dataLayer|.
  initialize() {
    if (this.initialized_) return;
    this.initialized_ = true;
    const script = document.createElement('script');
    script.src = 'https://www.googletagmanager.com/gtag/js?id=' + ANALYTICS_ID;
    script.defer = true;
    document.head.appendChild(script);
    const route = window.location.href;
    console.log(
        `GA initialized. route=${route}`,
        `isInternalUser=${globals.isInternalUser}`);
    // GA's reccomendation for SPAs is to disable automatic page views and
    // manually send page_view events. See:
    // https://developers.google.com/analytics/devguides/collection/gtagjs/pages#manual_pageviews
    gtagGlobals.gtag('config', ANALYTICS_ID, {
      allow_google_signals: false,
      anonymize_ip: true,
      page_location: route,
      referrer: document.referrer.split('?')[0],
      send_page_view: false,
      page_title: PAGE_TITLE,
      perfetto_is_internal_user: globals.isInternalUser ? '1' : '0',
      perfetto_version: VERSION,
      perfetto_channel: getCurrentChannel(),
    });
    this.updatePath(route);
  }

  updatePath(path: string) {
    gtagGlobals.gtag(
        'event', 'page_view', {page_path: path, page_title: PAGE_TITLE});
  }

  logEvent(category: TraceCategories|null, event: string) {
    gtagGlobals.gtag('event', event, {event_category: category});
  }

  logError(description: string, fatal = true) {
    gtagGlobals.gtag('event', 'exception', {description, fatal});
  }

  isEnabled(): boolean {
    return true;
  }
}
