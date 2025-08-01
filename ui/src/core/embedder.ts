// Copyright (C) 2025 The Android Open Source Project
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

import {RouteArgs} from '../public/route_schema';
import {Route} from './router';
import {SerializedAppState} from './state_serialization_schema';

/**
 * An optional intercept of various routing APIs to customize how Perfetto UI
 * responds to and initiates URL navigation to present different parts of the app.
 */
export interface RoutingHooks {
  /** Optional override of window URL hash change handling in the `Router`. */
  onHashChange?(oldRoute: Route, newRoute: Route): Route;
  /** Optional override of navigating to a new window URL in the `Router`. */
  navigate?(uri: string): void;
  /** Optional override of opening a trace on URL navigation. */
  openTraceFromRoute?(route: Route): boolean;
  /** Optional override of state tracking what is the current route to which the UI has navigated. */
  readonly currentRoute?: Route;
}

/**
 * An optional strategy for installation of Perfetto UI's content security
 * policy by whatever means is appropriate for the host application. It is
 * likely that the embedder already has a `<meta>` tag defining the policy,
 * so this is an opportunity to update that tag, create it if it does not
 * already exist, or apply the policy in some other application-specific manner.
 * Additionally, the embedder is free to select from and augment the given
 * `policy` rules.
 *
 * @param policy a content security policy to install
 */
export interface ContentSecurityPolicyInstaller {
  (policy: Readonly<Record<string, string[]>>): void;
}

/**
 * An optional handler for post messages not recognized by Perfetto UI.
 */
export interface PostMessageHandler {
  /**
   * Attempt to handle the given data received in a post message.
   * @returns `true` if successfully handled;
   *    `false` to indicate that the message is unrecognized or unsupported
   */
  (messageData: MessageEvent['data']): boolean;
}

/**
 * Enumeration of responses to the query of what to do with a trace found pre-loaded
 * into the trace processor when starting Perfetto UI.
 */
export type PreloadedTraceOption = 'use_trace' | 'reset_rpc' | 'use_wasm';
/**
 * Signature of a function querying what to do with a trace found pre-loaded
 * into the trace processor when starting Perfetto UI.
 *
 * @param loadedTraceName a user-presentable label for the trace that was loaded
 */
export interface PreloadedTraceQuery {
  (loadedTraceName: string): Promise<PreloadedTraceOption>;
}

/**
 * Optional overrides/intercepts of basic Perfetto UI behaviours
 * for integration with host (embedding) applications. If the context
 * does not exist, then there is no host application.
 */
export interface EmbedderContext {
  /**
   * Optional flag to suppress the built-in error handling.
   * If `true`, the host application avers that it will handle uncaught
   * exceptions and promise rejections.
   */
  readonly suppressErrorHandling?: boolean;

  /**
   * Optional flag to suppress rendering of the main UI.
   * If `true`, then the host application is responsible for instantiating
   * some portion of the Perfetto UI when and as required.
   */
  readonly suppressMainUi?: boolean;

  /**
   * Optional prefix to prepend on the URLs used as keys for cache storage
   * in the `CacheManager`.
   */
  readonly cachePrefix?: string;

  /**
   * Optional hooks to intercept/override routing functions, as described
   * in the `RoutingHooks` interface.
   */
  readonly routingHooks?: RoutingHooks;

  /**
   * Optional initial route arguments to inject as defaults before route args
   * parsed from the window URL are overlaid.
   */
  readonly initialRouteArgs?: RouteArgs;

  /**
   * Optional content-security policy installation strategy.
   */
  readonly setContentSecurityPolicy?: ContentSecurityPolicyInstaller;

  /**
   * Optional handler for post messages not recognized by Perfetto UI.
   */
  readonly postMessageHandler?: PostMessageHandler;

  /**
   * Optional strategy for how to deal with a trace that is found pre-loaded into the
   * trace processor at start-up.
   */
  readonly shouldUsePreloadedTrace?: PreloadedTraceQuery;

  /**
   * Optional serialized application state to seed the loading of a trace. Useful for
   * applications that save UI and plug-in state to restore when reloading a trace
   * that had been loaded in an earlier session.
   */
  readonly appState?: SerializedAppState;
}

export declare const embedderContext: Readonly<EmbedderContext> | undefined;

let _embedderContext: typeof embedderContext;
Object.defineProperty(exports, 'embedderContext', {
  get: () => _embedderContext,
  enumerable: true,
});

/**
 * Set the embedder context for customization of the UI application behaviour
 * within the embedding application. This may only be set once.
 */
export function setEmbedderContext(embedderContext: EmbedderContext): void {
  if (_embedderContext) {
    throw new Error('embedder context is already set');
  }
  _embedderContext = embedderContext;
}
