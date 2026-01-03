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

/**
 * Resolves extension server URLs from shorthand aliases to canonical HTTPS URLs.
 *
 * Supports:
 * - github://owner/repo/ref[/optional/path] -> https://raw.githubusercontent.com/owner/repo/ref[/optional/path]
 * - gs://bucket/path -> https://storage.googleapis.com/bucket/path
 * - s3://bucket/path -> https://bucket.s3.amazonaws.com/path
 * - https://... -> unchanged (already canonical)
 *
 * @param input User-provided server URL (with or without alias)
 * @returns Canonical HTTPS URL
 */
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

  // GCS alias: gs://bucket/path
  if (trimmed.startsWith('gs://')) {
    const path = trimmed.substring('gs://'.length);
    if (!path) {
      throw new Error('Invalid GCS URL: missing bucket/path');
    }
    return `https://storage.googleapis.com/${path}`;
  }

  // S3 alias: s3://bucket/path
  if (trimmed.startsWith('s3://')) {
    const path = trimmed.substring('s3://'.length);
    if (!path) {
      throw new Error('Invalid S3 URL: missing bucket/path');
    }
    // Extract bucket name (first path component)
    const slashIndex = path.indexOf('/');
    if (slashIndex === -1) {
      throw new Error('Invalid S3 URL: missing path after bucket');
    }
    const bucket = path.substring(0, slashIndex);
    const objectPath = path.substring(slashIndex + 1);
    return `https://${bucket}.s3.amazonaws.com/${objectPath}`;
  }

  // HTTPS URLs pass through unchanged
  if (trimmed.startsWith('https://')) {
    return trimmed;
  }

  // HTTP URLs are upgraded to HTTPS
  if (trimmed.startsWith('http://')) {
    return trimmed.replace('http://', 'https://');
  }

  // Unknown format
  throw new Error(
    `Invalid server URL: must start with https://, github://, gs://, or s3://`,
  );
}

/**
 * Normalizes a canonical HTTPS URL to a stable server key.
 *
 * Server keys are used for:
 * - Deduplication (same server added twice)
 * - Deterministic ordering
 * - Macro namespacing
 * - Credential lookups
 *
 * Algorithm:
 * 1. Lowercase the URL
 * 2. Strip the https:// prefix
 * 3. Remove any query string or fragment
 * 4. Replace non-alphanumeric ASCII (e.g., / . _ :) with -
 * 5. Collapse repeated - and trim leading/trailing -
 *
 * Examples:
 * - https://perfetto.acme.com -> perfetto-acme-com
 * - https://corp.example.com:8443/modules -> corp-example-com-8443-modules
 * - https://raw.githubusercontent.com/acme/perfetto-ext/main ->
 *   raw-githubusercontent-com-acme-perfetto-ext-main
 *
 * @param url Canonical HTTPS URL
 * @returns Normalized server key
 */
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
