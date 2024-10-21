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

import {ErrorDetails} from '../base/logging';
import {getCurrentChannel} from '../common/channels';
import {VERSION} from '../gen/perfetto_version';
import {globals} from './globals';
import {Router} from '../core/router';

type TraceCategories = 'Trace Actions' | 'Record Trace' | 'User Actions';
const ANALYTICS_ID = 'G-BD89KT2P3C';
const PAGE_TITLE = 'no-page-title';

function isValidUrl(s: string) {
  let url;
  try {
    url = new URL(s);
  } catch (_) {
    return false;
  }
  return url.protocol === 'http:' || url.protocol === 'https:';
}

function getReferrerOverride(): string | undefined {
  const route = Router.parseUrl(window.location.href);
  const referrer = route.args.referrer;
  if (referrer) {
    return referrer;
  } else {
    return undefined;
  }
}

// Get the referrer from either:
// - If present: the referrer argument if present
// - document.referrer
function getReferrer(): string {
  const referrer = getReferrerOverride();
  if (referrer) {
    if (isValidUrl(referrer)) {
      return referrer;
    } else {
      // Unclear if GA discards non-URL referrers. Lets try faking
      // a URL to test.
      const name = referrer.replaceAll('_', '-');
      return `https://${name}.example.com/converted_non_url_referrer`;
    }
  } else {
    return document.referrer.split('?')[0];
  }
}

export function initAnalytics() {
  // Only initialize logging on the official site and on localhost (to catch
  // analytics bugs when testing locally).
  // Skip analytics is the fragment has "testing=1", this is used by UI tests.
  // Skip analytics in embeddedMode since iFrames do not have the same access to
  // local storage.
  if (
    (window.location.origin.startsWith('http://localhost:') ||
      window.location.origin.endsWith('.perfetto.dev')) &&
    !globals.testing &&
    !globals.embeddedMode
  ) {
    return new AnalyticsImpl();
  }
  return new NullAnalytics();
}

const gtagGlobals = window as {} as {
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  dataLayer: any[];
  gtag: (command: string, event: string | Date, args?: {}) => void;
};

export interface Analytics {
  initialize(): void;
  updatePath(_: string): void;
  logEvent(category: TraceCategories | null, event: string): void;
  logError(err: ErrorDetails): void;
  isEnabled(): boolean;
}

class NullAnalytics implements Analytics {
  initialize() {}
  updatePath(_: string) {}
  logEvent(_category: TraceCategories | null, _event: string) {}
  logError(_err: ErrorDetails) {}
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
    // eslint-disable-next-line @typescript-eslint/strict-boolean-expressions
    gtagGlobals.dataLayer = gtagGlobals.dataLayer || [];

    // eslint-disable-next-line @typescript-eslint/no-explicit-any
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
      `isInternalUser=${globals.isInternalUser}`,
    );
    // GA's recommendation for SPAs is to disable automatic page views and
    // manually send page_view events. See:
    // https://developers.google.com/analytics/devguides/collection/gtagjs/pages#manual_pageviews
    gtagGlobals.gtag('config', ANALYTICS_ID, {
      allow_google_signals: false,
      anonymize_ip: true,
      page_location: route,
      // Referrer as a URL including query string override.
      page_referrer: getReferrer(),
      send_page_view: false,
      page_title: PAGE_TITLE,
      perfetto_is_internal_user: globals.isInternalUser ? '1' : '0',
      perfetto_version: VERSION,
      // Release channel (canary, stable, autopush)
      perfetto_channel: getCurrentChannel(),
      // Referrer *if overridden* via the query string else empty string.
      perfetto_referrer_override: getReferrerOverride() ?? '',
    });
    this.updatePath(route);
  }

  updatePath(path: string) {
    gtagGlobals.gtag('event', 'page_view', {
      page_path: path,
      page_title: PAGE_TITLE,
    });
  }

  logEvent(category: TraceCategories | null, event: string) {
    gtagGlobals.gtag('event', event, {event_category: category});
  }

  logError(err: ErrorDetails) {
    let stack = '';
    for (const entry of err.stack) {
      const shortLocation = entry.location.replace('frontend_bundle.js', '$');
      stack += `${entry.name}(${shortLocation}),`;
    }
    // Strip trailing ',' (works also for empty strings without extra checks).
    stack = stack.substring(0, stack.length - 1);

    gtagGlobals.gtag('event', 'exception', {
      description: err.message,
      error_type: err.errType,

      // As per GA4 all field are restrictred to 100 chars.
      // page_title is the only one restricted to 1000 chars and we use that for
      // the full crash report.
      page_location: `http://crash?/${encodeURI(stack)}`,
    });
  }

  isEnabled(): boolean {
    return true;
  }
}
