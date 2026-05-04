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

// Manually generated with bindings for a limited set of TracePacket
// fields to limit core proto bindings.

use crate::pb_enum;
use crate::pb_msg;
use crate::protos::trace::clock_snapshot::*;
use crate::protos::trace::interned_data::interned_data::*;
use crate::protos::trace::test_event::*;
use crate::protos::trace::track_event::track_descriptor::*;
use crate::protos::trace::track_event::track_event::*;

pb_enum!(TracePacketSequenceFlags {
    SEQ_UNSPECIFIED: 0,
    SEQ_INCREMENTAL_STATE_CLEARED: 1,
    SEQ_NEEDS_INCREMENTAL_STATE: 2,
});

pb_msg!(TracePacketDefaults {
    timestamp_clock_id: u32, primitive, 58,
    track_event_defaults: TrackEventDefaults, msg, 11,
});

pb_msg!(TracePacket {
    timestamp: u64, primitive, 8,
    timestamp_clock_id: u32, primitive, 58,
    trusted_uid: i32, primitive, 3,
    trusted_packet_sequence_id: u32, primitive, 10,
    trusted_pid: i32, primitive, 79,
    first_packet_on_sequence: bool, primitive, 87,
    clock_snapshot: ClockSnapshot, msg, 6,
    track_event: TrackEvent, msg, 11,
    track_descriptor: TrackDescriptor, msg, 60,
    for_testing: TestEvent, msg, 900,
    interned_data: InternedData, msg, 12,
    sequence_flags: u32, primitive, 13,
    trace_packet_defaults: TracePacketDefaults, msg, 59,
});
