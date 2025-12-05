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

import m from 'mithril';
import markdownit from 'markdown-it';
import {assetSrc} from '../../../base/assets';

// Create a markdown renderer instance with safe HTML rendering
const md = markdownit({
  html: false, // Disable HTML tags in source
  linkify: true, // Auto-convert URLs to links
  typographer: true, // Enable smart quotes and other typographic replacements
});

// Cache for loaded markdown content
const markdownCache = new Map<string, string>();

// Track loading states
const loadingStates = new Map<string, 'loading' | 'loaded' | 'error'>();

/**
 * Loads and renders a markdown documentation file for a node type.
 * @param nodeType The node type identifier (e.g., 'modify_columns', 'filter', etc.)
 * @returns Mithril virtual DOM containing the rendered markdown
 */
export function loadNodeDoc(nodeType: string): m.Children {
  // Check cache first
  const cached = markdownCache.get(nodeType);
  if (cached !== undefined) {
    return m('.pf-node-info', m.trust(md.render(cached)));
  }

  // Check loading state
  const state = loadingStates.get(nodeType);
  if (state === 'loading') {
    return m(
      '.pf-node-info',
      m('.pf-node-info-loading', 'Loading documentation...'),
    );
  }

  if (state === 'error') {
    return m(
      '.pf-node-info',
      m(
        '.pf-node-info-error',
        'Documentation not available for this node type.',
      ),
    );
  }

  // Start loading
  loadingStates.set(nodeType, 'loading');

  // Fetch the markdown file from assets
  const assetPath = assetSrc(`assets/explore_page/node_info/${nodeType}.md`);

  fetch(assetPath)
    .then((response) => {
      if (!response.ok) {
        throw new Error(`HTTP ${response.status}: ${response.statusText}`);
      }
      return response.text();
    })
    .then((content) => {
      markdownCache.set(nodeType, content);
      loadingStates.set(nodeType, 'loaded');
      m.redraw();
    })
    .catch((error) => {
      console.warn(
        `Failed to load documentation for node type '${nodeType}':`,
        error,
      );
      loadingStates.set(nodeType, 'error');
      markdownCache.set(nodeType, ''); // Cache empty to avoid repeated attempts
      m.redraw();
    });

  // Return loading state
  return m(
    '.pf-node-info',
    m('.pf-node-info-loading', 'Loading documentation...'),
  );
}

/**
 * Preload documentation for a node type.
 * Useful for preloading docs for commonly used nodes.
 * @param nodeType The node type identifier
 */
export function preloadNodeDoc(nodeType: string): void {
  if (!markdownCache.has(nodeType) && !loadingStates.has(nodeType)) {
    loadNodeDoc(nodeType);
  }
}

/**
 * Clear the documentation cache.
 * Useful for development/testing.
 */
export function clearNodeDocCache(): void {
  markdownCache.clear();
  loadingStates.clear();
}
