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

#![cfg_attr(
    feature = "intrinsics",
    allow(internal_features),
    feature(core_intrinsics)
)]

use perfetto_sdk::{
    producer::*,
    protos::trace::track_event::{
        source_location::SourceLocationFieldNumber, track_descriptor::TrackDescriptorFieldNumber,
        track_event::TrackEventFieldNumber,
    },
    track_event::*,
    track_event_begin, track_event_categories, track_event_counter, track_event_end,
    track_event_instant, track_event_set_category_callback,
};
use std::error::Error;

track_event_categories! {
    pub mod example_te_ns {
        ( "cat1", "Test category 1", [ "tag1" ] ),
        ( "cat2", "Test category 2", [ "tag2", "tag3" ] ),
    }
}

use example_te_ns as perfetto_te_ns;

fn main() -> Result<(), Box<dyn Error>> {
    let producer_args = ProducerInitArgsBuilder::new().backends(Backends::SYSTEM);
    Producer::init(producer_args.build());
    TrackEvent::init();
    perfetto_te_ns::register()?;

    let user_arg: i64 = 12345;
    track_event_set_category_callback!("cat1", move |inst_id, enabled, global_state_changed| {
        println!(
            "Callback: id: {} on: {}, global_state_changed: {}, user_arg: {}",
            inst_id, enabled, global_state_changed, user_arg
        );
        if enabled {
            track_event_instant!("cat1", "callback", |ctx: &mut EventContext| {
                ctx.add_debug_arg("user_arg", TrackEventDebugArg::Int64(12345));
                ctx.set_flush();
            });
        }
    });
    let my_track =
        TrackEventTrack::register_named_track("mytrack", 0, TrackEventTrack::process_track_uuid())?;
    let my_counter = TrackEventTrack::register_counter_track(
        "mycounter",
        TrackEventTrack::process_track_uuid(),
    )?;
    let mut flow_counter: u64 = 0;
    loop {
        track_event_instant!("cat1", "name1");
        track_event_instant!("cat1", "name2", |ctx: &mut EventContext| {
            ctx.add_debug_arg("dbg_arg", TrackEventDebugArg::Bool(false));
            ctx.add_debug_arg("dbg_arg", TrackEventDebugArg::String("mystring"));
        });
        track_event_begin!("cat1", "name3");
        track_event_end!("cat1");
        flow_counter += 1;
        let flow = TrackEventFlow::process_scoped_flow(flow_counter);
        track_event_begin!("cat1", "name4", |ctx: &mut EventContext| {
            ctx.set_track(&my_track);
            ctx.set_flow(&flow);
        });
        track_event_end!("cat1", |ctx: &mut EventContext| {
            ctx.set_track(&my_track);
        });
        track_event_instant!("cat1", "name5", |ctx: &mut EventContext| {
            ctx.set_timestamp(TrackEventTimestamp::now());
        });
        track_event_instant!("cat1", "name6", |ctx: &mut EventContext| {
            ctx.set_terminating_flow(&flow);
        });
        track_event_counter!("cat1", |ctx: &mut EventContext| {
            ctx.set_track(&my_counter);
            ctx.set_counter(TrackEventCounter::Int64(79));
        });
        track_event_instant!("cat1", "name8", |ctx: &mut EventContext| {
            ctx.set_named_track("dynamictrack", 2, TrackEventTrack::process_track_uuid());
            ctx.set_timestamp(TrackEventTimestamp::now());
        });
        track_event_instant!("cat1", "name9", |ctx: &mut EventContext| {
            ctx.set_proto_fields(&TrackEventProtoFields {
                fields: &[TrackEventProtoField::Nested(
                    TrackEventFieldNumber::SourceLocation as u32,
                    &[
                        TrackEventProtoField::Cstr(
                            SourceLocationFieldNumber::FileName as u32,
                            file!(),
                        ),
                        TrackEventProtoField::VarInt(
                            SourceLocationFieldNumber::LineNumber as u32,
                            line!() as u64,
                        ),
                    ],
                )],
            });
        });
        track_event_counter!("cat1", |ctx: &mut EventContext| {
            ctx.set_proto_track(&TrackEventProtoTrack {
                uuid: TrackEventTrack::counter_track_uuid(
                    "mycounter",
                    TrackEventTrack::process_track_uuid(),
                ),
                fields: &[
                    TrackEventProtoField::VarInt(
                        TrackDescriptorFieldNumber::ParentUuid as u32,
                        TrackEventTrack::process_track_uuid(),
                    ),
                    TrackEventProtoField::Cstr(
                        TrackDescriptorFieldNumber::Name as u32,
                        "mycounter",
                    ),
                    TrackEventProtoField::Bytes(TrackDescriptorFieldNumber::Counter as u32, &[]),
                ],
            });
            ctx.set_counter(TrackEventCounter::Int64(89));
        });
        track_event_counter!("cat1", |ctx: &mut EventContext| {
            ctx.set_track(&my_counter);
            ctx.set_counter(TrackEventCounter::Double(std::f64::consts::PI));
        });
        std::thread::sleep(std::time::Duration::from_secs(1));
    }
}
