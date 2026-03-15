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

import m from 'mithril';
import {duration, time} from '../../base/time';
import {Trace} from '../../public/trace';

// TODO(ivankc) Consider more generic graphs structures

// Represents a navigation target, typically an event on a track
// used for jumping to a specific time range and track.
export interface NavTarget {
  id: number;
  trackUri: string;
  ts: time;
  dur: duration;
  depth: number;
}

// Represents a single event in a relationship chain.
export interface RelatedEvent {
  id: number;
  ts: time;
  dur: duration;
  trackUri: string;
  type: string;
  depth?: number;
  customArgs?: unknown;
}

// Defines a directed relationship between two RelatedEvents.
export interface Relation {
  sourceId: number;
  targetId: number;
  type: string; // e.g., 'parent_child', 'flow', 'dependency', etc.
  customArgs?: unknown; // Args specific to this relation (e.g., color)
}

// Container for events and their relationships.
// This is the primary data structure used by the visualization components.
export interface RelatedEventData {
  events: ReadonlyArray<RelatedEvent>;
  relations: ReadonlyArray<Relation>;
}

// Defines a column for displaying RelatedEvent properties in a table (e.g., in a tab).
export interface ColumnDefinition {
  key: string;
  title: m.Children;
  widthPx?: number;
  minWidthPx?: number;
  render: (event: RelatedEvent, trace: Trace) => m.Children;
}
