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

//! Example demonstrating the tracing-perfetto-sdk layer.
//!
//! This shows how to use Rust's `tracing` crate with Perfetto as a
//! backend. Spans become duration slices and events become instants
//! in the Perfetto trace.
//!
//! ## In-process mode (default)
//!
//! Collects a trace to a file without needing a tracing service:
//! ```sh
//! cargo run --example tracing
//! ```
//! Then open `/tmp/trace.pftrace` in <https://ui.perfetto.dev>.
//!
//! ## System mode
//!
//! Connects to a running Perfetto tracing service:
//! ```sh
//! perfetto -o /tmp/trace.pftrace -c - --txt <<EOF
//!   buffers { size_kb: 65536 }
//!   data_sources { config { name: "track_event" } }
//!   duration_ms: 5000
//! EOF
//!
//! cargo run --example tracing -- --system
//! ```

use std::thread;
use std::time::Duration;

use tracing::{info, info_span, warn};
use tracing_subscriber::prelude::*;

fn main() {
    let system_mode = std::env::args().any(|arg| arg == "--system");

    if system_mode {
        tracing_perfetto_sdk::init_system();
    } else {
        tracing_perfetto_sdk::init_in_process();
    }

    tracing_subscriber::registry()
        .with(tracing_perfetto_sdk::PerfettoLayer::new())
        .init();

    // For in-process mode, start a tracing session.
    let mut session = if !system_mode {
        let mut session =
            perfetto_sdk::tracing_session::TracingSession::in_process().expect("session");
        // Build a minimal trace config enabling track_event.
        let config = build_trace_config();
        session.setup(&config);
        session.start_blocking();
        Some(session)
    } else {
        eprintln!("System mode: waiting for external trace session...");
        None
    };

    // Run the workload.
    info!("application started");
    for i in 0..3 {
        let _span = info_span!("iteration", i).entered();
        do_work(i);
    }
    info!("application finished");

    // For in-process mode, stop and write the trace.
    if let Some(ref mut session) = session {
        session.flush_blocking(Duration::from_secs(5));
        session.stop_blocking();
        let trace_data = std::sync::Arc::new(std::sync::Mutex::new(Vec::new()));
        let trace_data_clone = trace_data.clone();
        session.read_trace_blocking(move |data, _has_more| {
            trace_data_clone.lock().unwrap().extend_from_slice(data);
        });
        let trace_data = std::sync::Arc::try_unwrap(trace_data)
            .unwrap()
            .into_inner()
            .unwrap();
        std::fs::write("/tmp/trace.pftrace", &trace_data).expect("write trace");
        eprintln!(
            "Trace written to /tmp/trace.pftrace ({} bytes)",
            trace_data.len()
        );
    }
}

fn do_work(iteration: u32) {
    let _span = info_span!("do_work", iteration).entered();

    info!(iteration, "starting work");
    thread::sleep(Duration::from_millis(10));

    {
        let _inner = info_span!("inner_task", iteration).entered();
        thread::sleep(Duration::from_millis(5));
        warn!("something happened");
    }

    info!("work complete");
}

/// Build a minimal TraceConfig enabling the track_event data source.
fn build_trace_config() -> Vec<u8> {
    use perfetto_sdk::heap_buffer::HeapBuffer;
    use perfetto_sdk::pb_msg::{PbMsg, PbMsgWriter};
    use perfetto_sdk::protos::config::{
        data_source_config::DataSourceConfig,
        trace_config::{TraceConfig, TraceConfigBufferConfig, TraceConfigDataSource},
        track_event::track_event_config::TrackEventConfig,
    };

    let writer = PbMsgWriter::new();
    let hb = HeapBuffer::new(writer.stream_writer());
    let mut msg = PbMsg::new(&writer).unwrap();
    {
        let mut cfg = TraceConfig { msg: &mut msg };
        cfg.set_buffers(|buf_cfg: &mut TraceConfigBufferConfig| {
            buf_cfg.set_size_kb(4096);
        });
        cfg.set_data_sources(|data_sources: &mut TraceConfigDataSource| {
            data_sources.set_config(|ds_cfg: &mut DataSourceConfig| {
                ds_cfg.set_name("track_event");
                ds_cfg.set_track_event_config(|te_cfg: &mut TrackEventConfig| {
                    te_cfg.set_enabled_categories("tracing");
                });
            });
        });
    }
    msg.finalize();
    let cfg_size = writer.stream_writer().get_written_size();
    let mut cfg_buffer = vec![0u8; cfg_size];
    hb.copy_into(&mut cfg_buffer);
    cfg_buffer
}
