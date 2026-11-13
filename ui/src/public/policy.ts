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
 * Which additional features are on for the current user, with one field per
 * feature. Unlike feature flags, users can't change these.
 *
 * TODO(b/585531330): serve these from a policy server. Until then, every field
 * is derived from whether the user is internal (see
 * AppImpl.setIsInternalUser).
 */
export interface Policy {
  // Offer to share the current trace via a permalink.
  readonly enableTraceSharing: boolean;

  // Offer to upload the trace when filing a bug from the error dialog.
  readonly enableErrorTraceUpload: boolean;

  // Point bug links at the internal bug tracker instead of GitHub.
  readonly useInternalBugLinks: boolean;

  // Show the Perfetto SQL agent button and banner on the query page.
  readonly enableSqlAgent: boolean;

  // Allow the hiring banner in the sidebar.
  readonly showHiringBanner: boolean;
}
