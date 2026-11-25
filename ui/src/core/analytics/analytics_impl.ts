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

import type {ErrorDetails} from '../../base/logging';
import {getCurrentChannel} from '../channels';
import {getPerfettoEnv} from './perfetto_env';
import {VERSION} from '../../virtual/version';
import {Router} from '../router';
import {assertUnreachable} from '../../base/assert';
import type {Brand} from '../../base/brand';
import {uuidv4} from '../../base/uuid';
import type {Analytics, BasicAnalyticsEvent} from '../../public/analytics';
import type {Policy} from '../../public/policy';

const PAGE_TITLE = 'no-page-title';

function isValidUrl(s: string) {
  let url;
  try {
    url = new URL(s);
  } catch {
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
    return document.referrer.split('?')[0]!;
  }
}

// An event that only core can send, with AnalyticsInternal.logAdvancedEvent().
// These are only sent when Policy.useAdvancedAnalytics is on. Each kind of
// event has a unique `kind`, which is how code tells them apart.
export type AdvancedAnalyticsEvent = TraceLoadedEvent | PluginLoadedEvent;

// A random ID for one trace load, shared by all of its events.
export type TraceLoadId = Brand<string, 'TraceLoadId'>;

// The size of a trace file, rounded to a coarse range. 'unknown' when the size
// isn't known in advance, e.g. for traces opened from a URL.
export type TraceSizeBucket =
  '<10MB' | '10-100MB' | '100MB-1GB' | '>1GB' | 'unknown';

// A trace finished loading.
export interface TraceLoadedEvent {
  readonly kind: 'trace_loaded';
  readonly traceLoadId: TraceLoadId;
  // The trace's types, sorted and joined with '+', e.g. 'json+proto'.
  readonly traceFormat: string;
  readonly traceSizeBucket: TraceSizeBucket;
  readonly loadMs: number;
}

// One plugin's part in a trace load.
export interface PluginLoadedEvent {
  readonly kind: 'plugin_loaded';
  readonly traceLoadId: TraceLoadId;
  readonly plugin: string;
  readonly loadMs: number;
  // Whether the plugin registered at least one track for the trace.
  readonly hasTracks: boolean;
}

// Interface exposed only to core (for the initialize method).
export interface AnalyticsInternal extends Analytics {
  addDimension(
    arg: {key: string; value: string} | Promise<{key: string; value: string}>,
  ): void;
  // Records whether the user is internal on every event. Must be called before
  // initialize(), which waits for the answer before it starts sending.
  setIsInternalUser(isInternalUser: Promise<boolean>): void;
  // Sends an advanced event, but only if the policy allows advanced analytics
  // at the time of the call. Plugins can't reach this method.
  logAdvancedEvent(event: AdvancedAnalyticsEvent): void;
  initialize(): Promise<void>;
}

export function initAnalytics(
  testingMode: boolean,
  embeddedMode: boolean,
  enable: boolean,
  analyticsId: string | undefined,
  getPolicy: () => Policy,
): AnalyticsInternal {
  // Skip analytics if the fragment has "testing=1", this is used by UI tests.
  // Skip analytics in embeddedMode since iFrames do not have the same access to
  // local storage.
  // Skip analytics if the user has disabled analytics.
  // Skip analytics if the embedder does not provide an analytics ID.
  if (analyticsId !== undefined && !testingMode && !embeddedMode && enable) {
    return new AnalyticsImpl(analyticsId, getPolicy);
  }
  return new NullAnalytics();
}

const gtagGlobals = window as {} as {
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  dataLayer: any[];
  gtag: (command: string, event: string | Date, args?: {}) => void;
};

class NullAnalytics implements AnalyticsInternal {
  addDimension(
    _arg: {key: string; value: string} | Promise<{key: string; value: string}>,
  ) {}
  setIsInternalUser(_isInternalUser: Promise<boolean>) {}
  logAdvancedEvent(_event: AdvancedAnalyticsEvent) {}
  async initialize() {}
  logEvent(_event: BasicAnalyticsEvent) {}
  logError(_err: ErrorDetails) {}
  isEnabled(): boolean {
    return false;
  }
}

class AnalyticsImpl implements AnalyticsInternal {
  private initialized_ = false;
  private readonly analyticsId: string;
  private readonly getPolicy: () => Policy;
  // A random ID for this visit: it's created when the UI loads and lasts until
  // the tab is closed or reloaded. Why not use GA's session ID? GA puts all of
  // a browser's tabs on the same site into one session, but we want to report
  // analytics per visit to one tab, that's why.
  private readonly visitId = uuidv4();
  private readonly dimensions: Array<Promise<{key: string; value: string}>> =
    [];

  constructor(analyticsId: string, getPolicy: () => Policy) {
    this.analyticsId = analyticsId;
    this.getPolicy = getPolicy;
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

  addDimension(
    arg: {key: string; value: string} | Promise<{key: string; value: string}>,
  ) {
    if (this.initialized_) {
      throw new Error('Cannot add analytics dimensions after initialization');
    }
    this.dimensions.push(Promise.resolve(arg));
  }

  setIsInternalUser(isInternalUser: Promise<boolean>) {
    this.addDimension(
      isInternalUser.then((v) => ({
        key: 'perfetto_is_internal_user',
        value: v ? '1' : '0',
      })),
    );
  }

  // It is fine to call updatePath() and log*() functions before initialize().
  // The gtag() function internally enqueues all requests into |dataLayer|.
  async initialize() {
    if (this.initialized_) return;
    this.initialized_ = true;
    const script = document.createElement('script');
    script.src =
      'https://www.googletagmanager.com/gtag/js?id=' + this.analyticsId;
    script.defer = true;
    document.head.appendChild(script);
    const route = window.location.href;

    // Await all dimension promises and build a map of extra dimensions.
    const resolvedDimensions = await Promise.all(this.dimensions);
    const extraDimensions: Record<string, string> = {};
    for (const {key, value} of resolvedDimensions) {
      extraDimensions[key] = value;
    }

    console.log(`GA initialized. route=${route}`);
    // GA's recommendation for SPAs is to disable automatic page views and
    // manually send page_view events. See:
    // https://developers.google.com/analytics/devguides/collection/gtagjs/pages#manual_pageviews
    gtagGlobals.gtag('config', this.analyticsId, {
      allow_google_signals: false,
      anonymize_ip: true,
      page_location: route,
      // Referrer as a URL including query string override.
      page_referrer: getReferrer(),
      send_page_view: false,
      page_title: PAGE_TITLE,
      perfetto_version: VERSION,
      // Release channel (canary, stable, autopush)
      perfetto_channel: getCurrentChannel(),
      // Referrer *if overridden* via the query string else empty string.
      perfetto_referrer_override: getReferrerOverride() ?? '',
      // Where the UI is served from (prod, localhost, pr_preview or other).
      perfetto_env: getPerfettoEnv(new URL(window.location.href)),
      visit_id: this.visitId,
      ...extraDimensions,
    });

    gtagGlobals.gtag('event', 'page_view', {
      page_path: route,
      page_title: PAGE_TITLE,
    });
  }

  logEvent(event: BasicAnalyticsEvent) {
    switch (event.kind) {
      case 'page_view':
        // Page views are sent as an event named after the page, rather than
        // as GA's own page_view event, so that existing reports keep working.
        gtagGlobals.gtag('event', event.page, {event_category: 'User Actions'});
        break;
      case 'action':
        gtagGlobals.gtag('event', event.action, {
          event_category: event.category ?? null,
        });
        break;
      default:
        assertUnreachable(event);
    }
  }

  logAdvancedEvent(event: AdvancedAnalyticsEvent) {
    // Only send advanced events if the policy allows them. The policy only
    // allows them for internal users (Googlers). See RFC 0053.
    // The policy is read on every call, as it can change when this visit's
    // internal-user answer arrives.
    if (!this.getPolicy().useAdvancedAnalytics) return;
    gtagGlobals.gtag('event', event.kind, toGoogleAnalyticsParams(event));
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

// Converts an advanced event to GA parameters, in GA's snake_case naming.
function toGoogleAnalyticsParams(
  event: AdvancedAnalyticsEvent,
): Record<string, string | number> {
  switch (event.kind) {
    case 'trace_loaded':
      return {
        trace_load_id: event.traceLoadId,
        trace_format: event.traceFormat,
        trace_size_bucket: event.traceSizeBucket,
        load_ms: event.loadMs,
      };
    case 'plugin_loaded':
      return {
        trace_load_id: event.traceLoadId,
        plugin: event.plugin,
        load_ms: event.loadMs,
        has_tracks: event.hasTracks ? 1 : 0,
      };
    default:
      assertUnreachable(event);
  }
}
