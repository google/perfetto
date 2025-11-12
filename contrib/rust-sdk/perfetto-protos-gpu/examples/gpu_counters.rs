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
    data_source::*, pb_decoder::*, producer::*, protos::trace::trace_packet::TracePacket,
};

use perfetto_sdk_protos_gpu::protos::{
    common::gpu_counter_descriptor::*, config::data_source_config::*,
    config::gpu::gpu_counter_config::*, trace::gpu::gpu_counter_event::*,
    trace::trace_packet::prelude::*,
};

use std::{
    error::Error,
    sync::{Arc, Mutex},
    time::{Duration, Instant},
};

#[derive(Debug, Default)]
struct GpuCounterConfig {
    counter_period_ns: Option<u64>,
    counter_ids: Vec<u32>,
    instrumented_sampling: Option<bool>,
    fix_gpu_clock: Option<bool>,
}

impl GpuCounterConfig {
    fn decode(&mut self, data: &[u8]) -> &mut Self {
        use PbDecoderField::*;
        const COUNTER_PERIOD_NS_ID: u32 = GpuCounterConfigFieldNumber::CounterPeriodNs as u32;
        const COUNTER_IDS_ID: u32 = GpuCounterConfigFieldNumber::CounterIds as u32;
        const INSTRUMENTED_SAMPLING_ID: u32 =
            GpuCounterConfigFieldNumber::InstrumentedSampling as u32;
        const FIX_GPU_CLOCK_ID: u32 = GpuCounterConfigFieldNumber::FixGpuClock as u32;
        for item in PbDecoder::new(data) {
            match item.as_ref().unwrap_or_else(|e| panic!("Error: {}", e)) {
                (COUNTER_PERIOD_NS_ID, Varint(v)) => self.counter_period_ns = Some(*v),
                (COUNTER_IDS_ID, Varint(v)) => self.counter_ids.push(*v as u32),
                (INSTRUMENTED_SAMPLING_ID, Varint(v)) => self.instrumented_sampling = Some(*v != 0),
                (FIX_GPU_CLOCK_ID, Varint(v)) => self.fix_gpu_clock = Some(*v != 0),
                _ => println!("WARNING: unknown GpuCounterConfig field: {:?}", item),
            }
        }
        self
    }
}

fn main() -> Result<(), Box<dyn Error>> {
    const GPU_COUNTER_CONFIG_ID: u32 = DataSourceConfigExtFieldNumber::GpuCounterConfig as u32;
    let producer_args = ProducerInitArgsBuilder::new().backends(Backends::SYSTEM);
    Producer::init(producer_args.build());
    let mut data_source = DataSource::new();
    let gpu_counter_config = Arc::new(Mutex::new(GpuCounterConfig::default()));
    let gpu_counter_config_for_setup = Arc::clone(&gpu_counter_config);
    let data_source_args = DataSourceArgsBuilder::new().on_setup(move |inst_id, config, _args| {
        let mut gpu_counter_config = gpu_counter_config_for_setup.lock().unwrap();
        for item in PbDecoder::new(config) {
            if let (GPU_COUNTER_CONFIG_ID, PbDecoderField::Delimited(value)) =
                item.unwrap_or_else(|e| panic!("Error: {}", e))
            {
                gpu_counter_config.decode(value);
            }
        }
        println!(
            "OnSetup id: {} gpu_conter_config: {:?}",
            inst_id, gpu_counter_config
        );
    });
    let start_time = Instant::now();
    data_source.register("gpu.counters.rig", data_source_args.build())?;
    loop {
        data_source.trace(|ctx: &mut TraceContext| {
            // Fixed set of counters: sin, cos, tan.
            const COUNTER_IDS: [u32; 3] = [1, 2, 3];
            let elapsed_secs = start_time.elapsed().as_secs_f64();
            ctx.with_incremental_state(|ctx: &mut TraceContext, state| {
                let was_cleared = std::mem::replace(&mut state.was_cleared, false);
                ctx.add_packet(|packet: &mut TracePacket| {
                    packet.set_gpu_counter_event(|event: &mut GpuCounterEvent| {
                        for i in COUNTER_IDS.iter() {
                            event.set_counters(|counter: &mut GpuCounter| {
                                counter.set_counter_id(*i);
                                match i {
                                    1 => counter.set_double_value(elapsed_secs.sin()),
                                    2 => counter.set_double_value(elapsed_secs.cos()),
                                    _ => counter.set_double_value(elapsed_secs.tan()),
                                };
                            });
                        }
                        if was_cleared {
                            event.set_counter_descriptor(|desc: &mut GpuCounterDescriptor| {
                                for i in COUNTER_IDS.iter() {
                                    desc.set_specs(|desc: &mut GpuCounterSpec| {
                                        desc.set_counter_id(*i);
                                        match i {
                                            1 => desc.set_name("sin"),
                                            2 => desc.set_name("cos"),
                                            _ => desc.set_name("tan"),
                                        };
                                    });
                                }
                            });
                        }
                    });
                });
            });
        });
        let counter_period = gpu_counter_config
            .lock()
            .unwrap()
            .counter_period_ns
            .map(Duration::from_nanos)
            .unwrap_or(Duration::from_secs(1));
        std::thread::sleep(counter_period);
    }
}
