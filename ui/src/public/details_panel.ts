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

import m from 'mithril';
import {Selection, TrackEventSelection} from './selection';
import {z} from 'zod';

export interface DetailsPanel {
  render(selection: Selection): m.Children;
  isLoading?(): boolean;
}

export interface TrackEventDetailsPanelSerializeArgs<T> {
  // The Zod schema which will be used the parse the state in a serialized
  // permalink JSON object.
  readonly schema: z.ZodType<T>;

  // The serializable state of the details panel. The usage of this field is
  // as follows
  //  1) default initialize this field in the constructor.
  //  2) if the trace is being restored from a permalink, the UI will use
  //     `schema` to parse the serialized state and will write the result into
  //     `state`. If parsing failed or the trace is not being restored,
  //     `state` will not be touched.
  //  3) if a permalink is requested, the UI will read the value of `state`
  //     and stash it in the permalink serialzed state.
  //
  // This flow has the following consequences:
  //  1) Details panels *must* respect changes to this object between their
  //     constructor and the first call to `load()`. This is the point where
  //     the core will "inject" the permalink deserialized object
  //     if available.
  //  2) The `state` object *must* be serializable: that is, it should be a
  //     pure Javascript object.
  state: T;
}

export interface TrackEventDetailsPanel {
  // Optional: Do any loading required to render the details panel in here and
  // the core will:
  // - Ensure that no more than one concurrent loads are enqueued at any given
  //   time in order to keep the UI snappy.
  // - Hold off switching to this tab for up to around 50ms while this loading
  //   is going, to avoid flickering when loading is fast.
  load?(id: TrackEventSelection): Promise<void>;

  // Called every render cycle to render the details panel. Note: This function
  // is called regardless of whether |load| has completed yet.
  render(): m.Children;

  // Optional interface to implement by details panels which want to support
  // saving/restoring state from a permalink.
  readonly serialization?: TrackEventDetailsPanelSerializeArgs<unknown>;
}
