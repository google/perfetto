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
// - github://owner/repo/ref -> https://raw.githubusercontent.com/owner/repo/ref
// - https://... -> unchanged (already canonical)
//
// Note: http:// URLs are rejected. Use https:// instead.
export function resolveServerUrl(input: string): string {
  const trimmed = input.trim();

  // GitHub alias: github://owner/repo/ref
  if (trimmed.startsWith('github://')) {
    const path = trimmed.substring('github://'.length);
    if (!path) {
      throw new Error('Invalid GitHub URL: missing owner/repo/ref');
    }
    // Path format: owner/repo/ref
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
  throw new Error('Invalid server URL: must start with https:// or github://');
}

// Converts a canonical URL into a display-friendly format by removing
// the protocol prefix.
export function makeDisplayUrl(url: string) {
  return url.replace(/^github:\/\//, '').replace(/^https?:\/\//, '');
}
