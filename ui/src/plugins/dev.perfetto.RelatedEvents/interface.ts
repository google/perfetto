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
import z from 'zod';

export const durationSchema = z.custom<duration>(
  (val) => typeof val === 'bigint',
);
export const timeSchema = z.custom<time>((val) => typeof val === 'bigint');

// Represents a navigation target, typically an event on a track
// used for jumping to a specific time range and track.
export interface NavTarget {
  id: number;
  trackUri: string;
  ts: time;
  dur: duration;
  depth: number;
}

export const NavTargetSchema = z.object({
  id: z.number(),
  trackUri: z.string(),
  ts: timeSchema,
  dur: durationSchema,
  depth: z.number(),
}) satisfies z.ZodType<NavTarget>;

// Represents a single event in a relationship chain.
export interface RelatedEvent {
  id: number; // Unique ID for this event within the dataset
  ts: time;
  dur: duration;
  trackUri: string; // URI of the track this event belongs to.
  type: string; // Type of event
  depth?: number; // Optional depth within the track
  customArgs?: Record<string, unknown>; // Support for additional custom arguments
}

// Defines a directed relationship between two RelatedEvents.
export interface Relation {
  sourceId: number; // ID of the source RelatedEvent
  targetId: number; // ID of the target RelatedEvent
  type: string; // e.g., 'parent_child', 'flow', 'dependency', etc.
  customArgs?: Record<string, unknown>; // Args specific to this relation (e.g., color)
}

// Container for events and their relationships.
// This is the primary data structure used by the visualization components.
export interface RelatedEventData {
  events: RelatedEvent[]; // All events involved in the relationships.
  relations: Relation[]; // The relationships between the events.
  // Optional events and relations specifically for the overlay arrows.
  overlayEvents?: RelatedEvent[];
  overlayRelations?: Relation[];
}

// Interface to be implemented by data providers.
// This allows different parts of the UI to supply RelatedEventData
// based on a selected event ID.
export interface EventSource {
  getRelatedEventData(eventId: number): Promise<RelatedEventData>;
}

// Defines a column for displaying RelatedEvent properties in a table (e.g., in a tab).
export interface ColumnDefinition {
  key: string;
  title: m.Children;
  widthPx?: number;
  minWidthPx?: number;
  render: (event: RelatedEvent, trace: Trace) => m.Children;
}

// Configuration for a tab that displays details about related events.
export interface RelatedEventsTabConfig {
  tabTitle: string;
  columns: ColumnDefinition[]; // Columns to display in the tab table.
  getChainName?: (chain: RelatedEvent[]) => string; // Function to name a chain of events.
}
