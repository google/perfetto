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
 * Visual Marker Framework for Timeline Tracks.
 *
 * Provides fixed-screen-size visual markers (badges, icons, pins) over timeline events
 * to ensure high-visibility warnings (e.g. Jank, Cadence Skips) remain prominent
 * regardless of zoom level.
 */

import type m from 'mithril';
import type {ColorScheme} from '../../base/color_scheme';
import type {time, duration} from '../../base/time';

/** Supported visual shape styles for track markers. */
export type MarkerShape = 'badge' | 'pin' | 'diamond' | 'icon';

/**
 * Visual configuration and rendering attributes for a track marker.
 */
export interface VisualMarkerStyle {
  /** Size in screen pixels (e.g. 16px). Kept constant regardless of track zoom level. */
  readonly sizePx: number;
  /** Primary geometric shape for rendering the marker. */
  readonly shape?: MarkerShape;
  /** Color scheme used for background fill and text rendering. */
  readonly colorScheme: ColorScheme;
  /** Optional stroke border color around the marker badge. */
  readonly strokeColor?: string;
  /** Optional text color override. */
  readonly textColor?: string;
  /** Optional Unicode emoji or character icon displayed inside the badge. */
  readonly icon?: string;
  /** Optional custom canvas draw override callback. */
  render?(
    ctx: CanvasRenderingContext2D,
    x: number,
    y: number,
    sizePx: number,
    colorScheme: ColorScheme,
  ): void;
}

/**
 * Represents an individual fixed-size visual marker anchored at a specific timestamp.
 */
export interface VisualMarker<T = unknown> {
  /** Unique ID for keying and selection lookup. */
  readonly id: number;
  /** Primary timestamp anchor on the track timeline. */
  readonly ts: time;
  /** Optional slice duration. */
  readonly dur?: duration;
  /** Track depth / row index where the marker is rendered. */
  readonly depth: number;
  /** Categorical type identifier used for clustering similar markers. */
  readonly typeKey: string;
  /** Visual styling configuration. */
  readonly style: VisualMarkerStyle;
  /** Render priority weight used to pick representative marker when clustered. */
  readonly priority: number;
  /** Underlying row dataset entity. */
  readonly row: T;
  /** Tooltip element displayed when hovering over the marker. */
  readonly tooltip?: m.Children;
}

/**
 * A cluster of overlapping markers grouped together when zoomed out.
 */
export interface MarkerCluster<T = unknown> {
  /** Weighted center timestamp of the cluster. */
  readonly centerTs: time;
  /** Calculated screen X position. */
  readonly screenX: number;
  /** Depth row index. */
  readonly depth: number;
  /** Total count of aggregated markers within this screen region. */
  readonly count: number;
  /** Marker with highest priority chosen to represent the cluster visually. */
  readonly representativeMarker: VisualMarker<T>;
  /** Complete list of markers contained in this cluster. */
  readonly markers: ReadonlyArray<VisualMarker<T>>;
}
