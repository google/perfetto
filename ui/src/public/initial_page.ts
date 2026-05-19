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

/**
 * Lets plugins suggest, during onTraceLoad(), which page the app should
 * navigate to once the trace finishes loading. Highest priority wins; ties
 * resolve to first-registered. If no plugin suggests anything, the app falls
 * back to '/viewer'.
 *
 * Trace-scoped: suggestions are cleared automatically when the trace unloads.
 *
 * Why this isn't part of PageManager: page registration is app-scoped (pages
 * are registered both before any trace loads and from within onTraceLoad),
 * but "which page should we land on for THIS trace" only makes sense in the
 * context of a specific trace. Bolting it onto PageManager would either
 * expose a method on `app.pages` that has no meaningful behaviour, or force
 * PageManager to have a different shape on App vs. Trace. Keeping it as its
 * own small manager hanging off Trace avoids both problems.
 *
 * Suggested priority ranges:
 *   10  - generic alternative landing pages
 *   100 - format-specific landing pages (e.g. a heap profile viewer for a
 *         trace that contains only heap data)
 */
export interface InitialPageManager {
  suggest(route: string, priority: number): Disposable;
}
