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

// Resolves extension server URLs from shorthand aliases to canonical HTTPS URLs.
//
// Supports:
// - github://owner/repo/ref[/optional/path] -> https://raw.githubusercontent.com/...
// - https://... -> unchanged (already canonical)
//
// Note: http:// URLs are rejected. Use https:// instead.
export function resolveServerUrl(input: string): string {
  const trimmed = input.trim();

  // GitHub alias: github://owner/repo/ref[/optional/path]
  if (trimmed.startsWith('github://')) {
    const path = trimmed.substring('github://'.length);
    if (!path) {
      throw new Error('Invalid GitHub URL: missing owner/repo/ref');
    }
    // Path format: owner/repo/ref[/optional/path]
    return `https://raw.githubusercontent.com/${path}`;
  }

  // HTTPS URLs pass through unchanged
  if (trimmed.startsWith('https://')) {
    return trimmed;
  }

  // HTTP URLs are rejected - browsers don't allow mixed-content fetch,
  // and silently upgrading can hide bugs if server behaves differently.
  if (trimmed.startsWith('http://')) {
    throw new Error(
      'Invalid server URL: http:// is not supported, use https:// instead',
    );
  }

  // Unknown format
  throw new Error(
    'Invalid server URL: must start with https:// or github://',
  );
}

// Normalizes a canonical HTTPS URL to a stable server key.
//
// Server keys are used for:
// - Deduplication (same server added twice)
// - Deterministic ordering
// - Macro namespacing
// - Credential lookups
//
// Algorithm:
// 1. Lowercase the URL
// 2. Strip the https:// prefix
// 3. Remove any query string or fragment
// 4. Replace non-alphanumeric ASCII (e.g., / . _ :) with -
// 5. Collapse repeated - and trim leading/trailing -
//
// Examples:
// - https://perfetto.acme.com -> perfetto-acme-com
// - https://corp.example.com:8443/modules -> corp-example-com-8443-modules
// - https://raw.githubusercontent.com/acme/perfetto-ext/main ->
//   raw-githubusercontent-com-acme-perfetto-ext-main
export function normalizeServerKey(url: string): string {
  if (!url.startsWith('https://')) {
    throw new Error('Server key normalization requires canonical HTTPS URL');
  }

  // Strip https:// prefix
  let key = url.substring('https://'.length);

  // Remove query string and fragment
  const queryIndex = key.indexOf('?');
  if (queryIndex !== -1) {
    key = key.substring(0, queryIndex);
  }
  const fragmentIndex = key.indexOf('#');
  if (fragmentIndex !== -1) {
    key = key.substring(0, fragmentIndex);
  }

  // Lowercase
  key = key.toLowerCase();

  // Replace non-alphanumeric characters with -
  // Keep alphanumeric (a-z, 0-9) and replace everything else
  key = key.replace(/[^a-z0-9]+/g, '-');

  // Collapse repeated - and trim leading/trailing -
  key = key.replace(/-+/g, '-');
  key = key.replace(/^-+/, '');
  key = key.replace(/-+$/, '');

  if (!key) {
    throw new Error('Server key normalization resulted in empty key');
  }

  return key;
}
