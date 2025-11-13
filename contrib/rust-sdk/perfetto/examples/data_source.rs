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

use perfetto_sdk::{
    data_source::*,
    pb_decoder::*,
    producer::*,
    protos::{
        config::{data_source_config::*, test_config::*},
        trace::{
            test_event::*,
            trace_packet::TracePacket,
            track_event::debug_annotation::{DebugAnnotation, NestedValue, NestedValueNestedType},
        },
    },
};
use std::{
    error::Error,
    sync::{Arc, Mutex},
};

#[derive(Debug, Default)]
struct DummyFields {
    field_uint32: Option<u32>,
    field_int32: Option<i32>,
    field_uint64: Option<u64>,
    field_int64: Option<i64>,
    field_fixed64: Option<u64>,
    field_sfixed64: Option<i64>,
    field_fixed32: Option<u32>,
    field_sfixed32: Option<i32>,
    field_double: Option<f64>,
    field_float: Option<f32>,
    field_sint64: Option<i64>,
    field_sint32: Option<i32>,
    field_string: Option<String>,
    field_bytes: Vec<u8>,
}

impl DummyFields {
    fn decode(&mut self, data: &[u8]) -> &mut Self {
        use PbDecoderField::*;
        const UINT32_ID: u32 = DummyFieldsFieldNumber::FieldUint32 as u32;
        const INT32_ID: u32 = DummyFieldsFieldNumber::FieldInt32 as u32;
        const UINT64_ID: u32 = DummyFieldsFieldNumber::FieldUint64 as u32;
        const INT64_ID: u32 = DummyFieldsFieldNumber::FieldInt64 as u32;
        const FIXED64_ID: u32 = DummyFieldsFieldNumber::FieldFixed64 as u32;
        const SFIXED64_ID: u32 = DummyFieldsFieldNumber::FieldSfixed64 as u32;
        const FIXED32_ID: u32 = DummyFieldsFieldNumber::FieldFixed32 as u32;
        const SFIXED32_ID: u32 = DummyFieldsFieldNumber::FieldSfixed32 as u32;
        const DOUBLE_ID: u32 = DummyFieldsFieldNumber::FieldDouble as u32;
        const FLOAT_ID: u32 = DummyFieldsFieldNumber::FieldFloat as u32;
        const SINT64_ID: u32 = DummyFieldsFieldNumber::FieldSint64 as u32;
        const SINT32_ID: u32 = DummyFieldsFieldNumber::FieldSint32 as u32;
        const STRING_ID: u32 = DummyFieldsFieldNumber::FieldString as u32;
        const BYTES_ID: u32 = DummyFieldsFieldNumber::FieldBytes as u32;
        for item in PbDecoder::new(data) {
            match item.as_ref().unwrap_or_else(|e| panic!("Error: {}", e)) {
                (UINT32_ID, Varint(v)) => self.field_uint32 = Some(*v as u32),
                (INT32_ID, Varint(v)) => self.field_int32 = Some(*v as i32),
                (UINT64_ID, Varint(v)) => self.field_uint64 = Some(*v),
                (INT64_ID, Varint(v)) => self.field_int64 = Some(*v as i64),
                (FIXED64_ID, Fixed64(v)) => self.field_fixed64 = Some(*v),
                (SFIXED64_ID, Fixed64(v)) => self.field_sfixed64 = Some(*v as i64),
                (FIXED32_ID, Fixed32(v)) => self.field_fixed32 = Some(*v),
                (SFIXED32_ID, Fixed32(v)) => self.field_sfixed32 = Some(*v as i32),
                (DOUBLE_ID, Fixed64(v)) => self.field_double = Some(f64::from_bits(*v)),
                (FLOAT_ID, Fixed32(v)) => self.field_float = Some(f32::from_bits(*v)),
                (SINT64_ID, Varint(v)) => self.field_sint64 = Some(*v as i64),
                (SINT32_ID, Varint(v)) => self.field_sint32 = Some(*v as i32),
                (STRING_ID, Delimited(v)) => {
                    self.field_string = Some(String::from_utf8(v.to_vec()).unwrap())
                }
                (BYTES_ID, Delimited(v)) => self.field_bytes = v.to_vec(),
                _ => println!("WARNING: unknown DummyFields field: {:?}", item),
            }
        }
        self
    }
}

#[derive(Debug, Default)]
struct TestConfig {
    message_count: Option<u32>,
    max_messages_per_second: Option<u32>,
    seed: Option<u32>,
    message_size: Option<u32>,
    send_batch_on_register: Option<bool>,
    dummy_fields: Option<DummyFields>,
}

impl TestConfig {
    fn decode(&mut self, data: &[u8]) -> &mut Self {
        use PbDecoderField::*;
        const MESSAGE_COUNT_ID: u32 = TestConfigFieldNumber::MessageCount as u32;
        const MAX_MESSAGES_PER_SECOND_ID: u32 = TestConfigFieldNumber::MaxMessagesPerSecond as u32;
        const SEED_ID: u32 = TestConfigFieldNumber::Seed as u32;
        const MESSAGE_SIZE_ID: u32 = TestConfigFieldNumber::MessageSize as u32;
        const SEND_BATCH_ON_REGISTER_ID: u32 = TestConfigFieldNumber::SendBatchOnRegister as u32;
        const DUMMY_FIELDS_ID: u32 = TestConfigFieldNumber::DummyFields as u32;
        for item in PbDecoder::new(data) {
            match item.as_ref().unwrap_or_else(|e| panic!("Error: {}", e)) {
                (MESSAGE_COUNT_ID, Varint(v)) => self.message_count = Some(*v as u32),
                (MAX_MESSAGES_PER_SECOND_ID, Varint(v)) => {
                    self.max_messages_per_second = Some(*v as u32)
                }
                (SEED_ID, Varint(v)) => self.seed = Some(*v as u32),
                (MESSAGE_SIZE_ID, Varint(v)) => self.message_size = Some(*v as u32),
                (SEND_BATCH_ON_REGISTER_ID, Varint(v)) => {
                    self.send_batch_on_register = Some(*v != 0)
                }
                (DUMMY_FIELDS_ID, Delimited(v)) => {
                    let mut dummy_fields = DummyFields::default();
                    dummy_fields.decode(v);
                    self.dummy_fields = Some(dummy_fields);
                }
                _ => println!("WARNING: unknown TestConfig field: {:?}", item),
            }
        }
        self
    }
}

fn main() -> Result<(), Box<dyn Error>> {
    const FOR_TESTING_ID: u32 = DataSourceConfigFieldNumber::ForTesting as u32;
    let producer_args = ProducerInitArgsBuilder::new().backends(Backends::SYSTEM);
    Producer::init(producer_args.build());
    let mut data_source = DataSource::new();
    let setup_data = 1234;
    let test_configs: Arc<Mutex<[Option<TestConfig>; 8]>> =
        Arc::new(Mutex::new([None, None, None, None, None, None, None, None]));
    let test_configs_for_on_setup = Arc::clone(&test_configs);
    let stop_guards: Arc<Mutex<[Option<StopGuard>; 8]>> =
        Arc::new(Mutex::new([None, None, None, None, None, None, None, None]));
    let stop_guards_for_on_stop = Arc::clone(&stop_guards);
    let data_source_args = DataSourceArgsBuilder::new()
        .on_setup(move |inst_id, config, _| {
            let mut test_configs = test_configs_for_on_setup.lock().unwrap();
            let mut test_config = TestConfig::default();
            for item in PbDecoder::new(config) {
                if let (FOR_TESTING_ID, PbDecoderField::Delimited(value)) =
                    item.unwrap_or_else(|e| panic!("Error: {}", e))
                {
                    test_config.decode(value);
                }
            }
            test_configs[inst_id as usize] = Some(test_config);
            println!("OnSetup id: {} data: {}", inst_id, setup_data);
        })
        .on_start(move |inst_id, _| {
            println!(
                "OnStart id: {} {:?}",
                inst_id,
                test_configs.lock().unwrap()[inst_id as usize]
            );
        })
        .on_stop(move |inst_id, args| {
            let mut stop_guards = stop_guards_for_on_stop.lock().unwrap();
            stop_guards[inst_id as usize] = Some(args.postpone());
            println!("OnStop id: {}", inst_id);
        });
    data_source.register("com.example.custom_data_source", data_source_args.build())?;
    loop {
        data_source.trace(|ctx: &mut TraceContext| {
            let inst_id = ctx.instance_index();
            ctx.with_incremental_state(|ctx: &mut TraceContext, state| {
                if state.was_cleared {
                    ctx.add_packet(|packet: &mut TracePacket| {
                        packet
                            .set_timestamp(10)
                            .set_for_testing(|for_testing: &mut TestEvent| {
                                for_testing.set_str(format!(
                                    "Incremental state was cleared for inst_id: {}",
                                    inst_id
                                ));
                            });
                    });
                    state.was_cleared = false;
                }
            });
            ctx.add_packet(|packet: &mut TracePacket| {
                packet
                    .set_timestamp(42)
                    .set_for_testing(|for_testing: &mut TestEvent| {
                        for_testing.set_str("This is a long string");
                        for_testing.set_counter(10);
                        for_testing.set_payload(|payload: &mut TestPayload| {
                            payload.set_debug_annotations(
                                |debug_annotation: &mut DebugAnnotation| {
                                    debug_annotation.set_name("This is a payload debug annotation");
                                    debug_annotation.set_nested_value(
                                        |nested_value: &mut NestedValue| {
                                            nested_value.set_nested_type(
                                                NestedValueNestedType::Unspecified,
                                            );
                                            nested_value.set_string_value(
                                                "This is a nested debug annotation value",
                                            );
                                        },
                                    );
                                },
                            );
                            for _ in 0..10 {
                                payload.set_str("nested");
                            }
                        });
                    });
            });
            if let Some(stop_guard) = stop_guards.lock().unwrap()[inst_id as usize].take() {
                ctx.add_packet(|packet: &mut TracePacket| {
                    packet
                        .set_timestamp(10)
                        .set_for_testing(|for_testing: &mut TestEvent| {
                            for_testing
                                .set_str(format!("Asynchronous stop for inst_id: {}", inst_id));
                        });
                });
                ctx.flush(|| {});
                // Signal that the data source stop operation is complete. The explicit drop
                // call is just for documentation purposes as the guard would go out of scope
                // here and the behavior would be the same.
                drop(stop_guard);
            }
        });
        std::thread::sleep(std::time::Duration::from_secs(1));
    }
}
