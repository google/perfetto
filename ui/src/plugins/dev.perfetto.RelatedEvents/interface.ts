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

export interface NavTarget {
  id: number;
  trackUri: string;
  ts: time;
  dur: duration;
  depth: number;
}

export interface RelatedEvent {
  id: number; // Unique ID for this event within the dataset
  ts: time;
  dur: duration;
  trackUri: string;
  type: string; // Type of event
  depth?: number; // Optional depth within the track
  customArgs?: Record<string, unknown>; // Support for additional custom arguments
}

export interface Relation {
  sourceId: number; // ID of the source RelatedEvent
  targetId: number; // ID of the target RelatedEvent
  type: string; // e.g., 'parent_child', 'flow', 'dependency', etc.
  customArgs?: Record<string, unknown>; // Args specific to this relation
}

export interface RelatedEventData {
  events: RelatedEvent[];
  relations: Relation[];
  overlayEvents?: RelatedEvent[];
  overlayRelations?: Relation[];
}

export interface EventSource {
  getRelatedEventData(eventId: number): Promise<RelatedEventData>;
}

export interface ColumnDefinition {
  key: string;
  title: m.Children;
  widthPx?: number;
  minWidthPx?: number;
  render: (event: RelatedEvent, trace: Trace) => m.Children;
}

export interface RelatedEventsTabConfig {
  tabTitle: string;
  columns: ColumnDefinition[];
  getChainName?: (chain: RelatedEvent[]) => string;
}
