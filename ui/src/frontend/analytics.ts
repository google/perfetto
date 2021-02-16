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
import * as version from '../gen/perfetto_version';

type TraceCategories = 'Trace Actions'|'Record Trace'|'User Actions';
const ANALYTICS_ID = 'UA-137828855-1';

export function initAnalytics() {
  // Only initialize logging on prod or staging
  if (window.location.origin.startsWith('http://localhost:') ||
      window.location.origin.endsWith('.perfetto.dev') ||
      window.location.origin.endsWith('staging-dot-perfetto-ui.appspot.com')) {
    return new AnalyticsImpl();
  }
  return new NullAnalytics();
}

const gtagGlobals = window as {} as {
  // tslint:disable-next-line no-any
  dataLayer: any[];
  gtag: (command: string, event: string|Date, args?: {}) => void;
};

export interface Analytics {
  initialize(): void;
  updatePath(_: string): void;
  logEvent(_x: TraceCategories|null, _y: string): void;
  logError(_x: string, _y?: boolean): void;
}

export class NullAnalytics implements Analytics {
  initialize() {}
  updatePath(_: string) {}
  logEvent(_x: TraceCategories|null, _y: string) {}
  logError(_x: string) {}
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

    // tslint:disable-next-line no-any
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
    const route = globals.state.route || '/';
    console.log(
        `GA initialized. route=${route}`,
        `isInternalUser=${globals.isInternalUser}`);
    // GA's reccomendation for SPAs is to disable automatic page views and
    // manually send page_view events. See:
    // https://developers.google.com/analytics/devguides/collection/gtagjs/pages#manual_pageviews
    gtagGlobals.gtag('config', ANALYTICS_ID, {
      anonymize_ip: true,
      page_path: route,
      referrer: document.referrer.split('?')[0],
      send_page_view: false,
      dimension1: globals.isInternalUser ? '1' : '0',
      dimension2: version.VERSION,
      dimension3: globals.channel,
    });
    this.updatePath(route);
  }

  updatePath(path: string) {
    gtagGlobals.gtag('event', 'page_view', {page_path: path});
  }

  logEvent(category: TraceCategories|null, event: string) {
    gtagGlobals.gtag('event', event, {event_category: category});
  }

  logError(description: string, fatal = true) {
    gtagGlobals.gtag('event', 'exception', {description, fatal});
  }
}
