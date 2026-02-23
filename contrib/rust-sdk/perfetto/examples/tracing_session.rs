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
    heap_buffer::HeapBuffer,
    pb_msg::{PbMsg, PbMsgWriter},
    producer::*,
    protos::config::{
        data_source_config::DataSourceConfig,
        trace_config::{BufferConfig, DataSource, TraceConfig},
        track_event::track_event_config::TrackEventConfig,
    },
    scoped_track_event,
    tracing_session::TracingSession,
    track_event::*,
    track_event_categories, track_event_instant,
};
use std::{
    error::Error,
    fs::OpenOptions,
    io::Write,
    sync::{Arc, Mutex},
};

track_event_categories! {
    pub mod example_te_ns {
        ( "cat1", "Test category 1", [ "tag1" ] ),
    }
}

use example_te_ns as perfetto_te_ns;

fn main() -> Result<(), Box<dyn Error>> {
    let file = Arc::new(Mutex::new(
        OpenOptions::new()
            .write(true)
            .truncate(true)
            .open("example.pftrace")
            .expect("Failed to open file"),
    ));

    let producer_args = ProducerInitArgsBuilder::new().backends(Backends::IN_PROCESS);
    Producer::init(producer_args.build());
    TrackEvent::init();
    perfetto_te_ns::register()?;

    let session_config = {
        let writer = PbMsgWriter::new();
        let hb = HeapBuffer::new(writer.stream_writer());
        let mut msg = PbMsg::new(&writer).unwrap();
        {
            let mut cfg = TraceConfig { msg: &mut msg };
            cfg.set_buffers(|buf_cfg: &mut BufferConfig| {
                buf_cfg.set_size_kb(1024);
            });
            cfg.set_data_sources(|data_sources: &mut DataSource| {
                data_sources.set_config(|ds_cfg: &mut DataSourceConfig| {
                    ds_cfg.set_name("track_event");
                    ds_cfg.set_track_event_config(|te_cfg: &mut TrackEventConfig| {
                        te_cfg.set_enabled_categories("cat1");
                    });
                });
            });
        }
        msg.finalize();
        let cfg_size = writer.stream_writer().get_written_size();
        let mut cfg_buffer: Vec<u8> = vec![0u8; cfg_size];
        hb.copy_into(&mut cfg_buffer);
        cfg_buffer
    };
    let mut session = TracingSession::in_process()?;
    session.setup(&session_config);
    session.start_blocking();

    for _ in 0..5 {
        track_event_instant!("cat1", "instant_hello", |ctx: &mut EventContext| {
            ctx.add_debug_arg("from", TrackEventDebugArg::String("perfetto"));
            ctx.add_debug_arg("sdk", TrackEventDebugArg::String("rust"));
        });
        {
            scoped_track_event!("cat1", "scoped_hello", |ctx: &mut EventContext| {
                ctx.add_debug_arg("what", TrackEventDebugArg::String("sleep"));
                ctx.add_debug_arg("ms", TrackEventDebugArg::Int64(1000));
            });
            std::thread::sleep(std::time::Duration::from_millis(1000));
        }
        std::thread::sleep(std::time::Duration::from_millis(1000));
    }

    session.stop_blocking();

    let file_for_write = Arc::clone(&file);
    session.read_trace_blocking(move |data, _end| {
        let mut file = file_for_write.lock().unwrap();
        file.write_all(data).expect("Failed to write to file");
    });
    file.lock().unwrap().flush().expect("Failed to flush file");
    println!("Trace written to example.pftrace");
    Ok(())
}
