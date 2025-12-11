// Copyright (C) 2025 Rivos Inc.
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

// Manually generated with bindings for a limited set of
// DataSourceDescriptor fields to limit core proto bindings.

use crate::pb_msg;
use crate::protos::common::track_event_descriptor::*;

pb_msg!(DataSourceDescriptor {
    name: String, primitive, 1,
    id: u64, primitive, 7,
    will_notify_on_stop: bool, primitive, 2,
    will_notify_on_start: bool, primitive, 3,
    handles_incremental_state_clear: bool, primitive, 4,
    no_flush: bool, primitive, 9,
    track_event_descriptor: TrackEventDescriptor, msg, 6,
});
