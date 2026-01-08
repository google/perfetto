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

use perfetto_sdk_sys::*;
use std::{ffi::c_void, time::Duration};
use thiserror::Error;

/// Tracing session errors.
#[derive(Error, Debug, PartialEq)]
pub enum TracingSessionError {
    /// Error creating tracing session.
    #[error("Failed to create tracing session.")]
    CreateError,
}

type FlushCallback = Box<dyn Fn(bool) + Send + Sync + 'static>;

unsafe extern "C" fn flush_callback_trampoline(
    _impl: *mut PerfettoTracingSessionImpl,
    success: bool,
    user_arg: *mut c_void,
) {
    let result = std::panic::catch_unwind(|| {
        // Take back ownership of the boxed callback, which will be dropped at the end of the
        // scope.
        //
        // SAFETY: `user_arg` must be a boxed FlushCallback.
        let f: Box<FlushCallback> = unsafe { Box::from_raw(user_arg as *mut FlushCallback) };
        f(success);
    });
    if let Err(err) = result {
        eprintln!("Fatal panic: {:?}", err);
        std::process::abort();
    }
}

type ReadCallback = Box<dyn Fn(&[u8], bool) + Send + Sync + 'static>;

unsafe extern "C" fn read_callback_trampoline(
    _impl: *mut PerfettoTracingSessionImpl,
    data: *const c_void,
    size: usize,
    has_more: bool,
    user_arg: *mut c_void,
) {
    let result = std::panic::catch_unwind(|| {
        // SAFETY: `data` must point to a buffer of `size` length.
        let bytes = unsafe { std::slice::from_raw_parts(data as *const u8, size) };
        if has_more {
            // SAFETY: `user_arg` must be a boxed ReadCallback.
            let f: &ReadCallback = unsafe { &mut *(user_arg as *mut _) };
            f(bytes, has_more);
        } else {
            // Take back ownership of the boxed callback, which will be dropped at the end of the
            // scope.
            //
            // SAFETY: `user_arg` must be a boxed ReadCallback.
            let f: Box<ReadCallback> = unsafe { Box::from_raw(user_arg as *mut ReadCallback) };
            f(bytes, has_more);
        }
    });
    if let Err(err) = result {
        eprintln!("Fatal panic: {:?}", err);
        std::process::abort();
    }
}

/// An opaque structure used as the representation of a tracing session.
pub struct TracingSession {
    impl_: *mut PerfettoTracingSessionImpl,
}

impl TracingSession {
    /// Creates a system wide tracing session.
    pub fn system() -> Result<Self, TracingSessionError> {
        // SAFETY: FFI call with no outstanding preconditions.
        let impl_ = unsafe { PerfettoTracingSessionSystemCreate() };
        if impl_.is_null() {
            return Err(TracingSessionError::CreateError);
        }
        Ok(Self { impl_ })
    }

    /// Creates an in-process tracing session.
    pub fn in_process() -> Result<Self, TracingSessionError> {
        // SAFETY: FFI call with no outstanding preconditions.
        let impl_ = unsafe { PerfettoTracingSessionInProcessCreate() };
        if impl_.is_null() {
            return Err(TracingSessionError::CreateError);
        }
        Ok(Self { impl_ })
    }

    /// Setup tracing session using the provided `cfg` trace config.
    ///
    /// # Safety
    ///
    /// - `cfg` must be a properly encoded trace config.
    pub fn setup(&mut self, cfg: &[u8]) {
        // SAFETY:
        // - `self.impl_` must be created using `PerfettoTracingSessionSystemCreate` or
        //   `PerfettoTracingSessionInProcessCreate`.
        // - `cfg` must be a properly encoded trace config.
        unsafe { PerfettoTracingSessionSetup(self.impl_, cfg.as_ptr() as *mut c_void, cfg.len()) };
    }

    /// Asynchronous start of tracing session.
    pub fn start_async(&mut self) {
        // SAFETY: `self.impl_` must be created using `PerfettoTracingSessionSystemCreate` or
        // `PerfettoTracingSessionInProcessCreate`.
        unsafe { PerfettoTracingSessionStartAsync(self.impl_) };
    }

    /// Synchronous start of tracing session.
    pub fn start_blocking(&mut self) {
        // SAFETY: `self.impl_` must be created using `PerfettoTracingSessionSystemCreate` or
        // `PerfettoTracingSessionInProcessCreate`.
        unsafe { PerfettoTracingSessionStartBlocking(self.impl_) };
    }

    /// Synchronous stop of tracing session.
    pub fn stop_blocking(&mut self) {
        // SAFETY: `self.impl_` must be created using `PerfettoTracingSessionSystemCreate` or
        // `PerfettoTracingSessionInProcessCreate`.
        unsafe { PerfettoTracingSessionStopBlocking(self.impl_) };
    }

    /// Issues a flush request, asking all data sources to ack the request, within
    /// the specified timeout. A "flush" is a fence to ensure visibility of data in
    /// the async tracing pipeline. It guarantees that all data written before the
    /// call will be visible in the trace buffer. Returns immediately and
    /// invokes a callback when the flush request is complete.
    /// Args:
    ///  `cb`: will be invoked on an internal perfetto thread when all data
    ///    sources have acked, or the timeout is reached.
    ///  `timeout`: how much time the service will wait for data source acks. If
    ///    0, the global timeout specified in the TraceConfig (flush_timeout)
    ///    will be used. If flush_timeout is also unspecified, a default value
    ///    of 5s will be used.
    pub fn flush_async<F>(&mut self, timeout: Duration, cb: F)
    where
        F: Fn(bool) + Send + Sync + 'static,
    {
        let boxed: Box<FlushCallback> = Box::new(Box::new(cb));
        let user_arg = Box::into_raw(boxed) as *mut c_void;
        // SAFETY:
        // - `self.impl_` must be created using `PerfettoTracingSessionSystemCreate` or
        //   `PerfettoTracingSessionInProcessCreate`.
        // - `user_arg` must be a boxed FlushCallback.
        unsafe {
            PerfettoTracingSessionFlushAsync(
                self.impl_,
                timeout.as_millis() as u32,
                Some(flush_callback_trampoline),
                user_arg,
            )
        };
    }

    /// Like flush_async(), but blocks until the flush is complete (i.e. every data
    /// source has acknowledged or the timeout has expired).
    pub fn flush_blocking(&mut self, timeout: Duration) {
        // SAFETY: `self.impl_` must be created using `PerfettoTracingSessionSystemCreate` or
        // `PerfettoTracingSessionInProcessCreate`.
        unsafe { PerfettoTracingSessionFlushBlocking(self.impl_, timeout.as_millis() as u32) };
    }

    /// Repeatedly calls `cb` with data from the tracing session.
    pub fn read_trace_blocking<F>(&mut self, cb: F)
    where
        F: Fn(&[u8], bool) + Send + Sync + 'static,
    {
        let boxed: Box<ReadCallback> = Box::new(Box::new(cb));
        let user_arg = Box::into_raw(boxed) as *mut c_void;
        // SAFETY:
        // - `self.impl_` must be created using `PerfettoTracingSessionSystemCreate` or
        //   `PerfettoTracingSessionInProcessCreate`.
        // - `user_arg` must be a boxed ReadCallback.
        unsafe {
            PerfettoTracingSessionReadTraceBlocking(
                self.impl_,
                Some(read_callback_trampoline),
                user_arg,
            )
        };
    }
}

impl Drop for TracingSession {
    fn drop(&mut self) {
        // SAFETY: `self.impl_` must be created using `PerfettoTracingSessionSystemCreate` or
        // `PerfettoTracingSessionInProcessCreate`.
        unsafe { PerfettoTracingSessionDestroy(self.impl_) };
    }
}

#[cfg(test)]
mod tests {
    use crate::data_source::DataSource;
    use crate::tests::{TracingSessionBuilder, acquire_test_environment};
    use crate::{track_event::TrackEvent, track_event_categories};
    use std::{
        error::Error,
        sync::{MutexGuard, OnceLock},
    };

    const DATA_SOURCE_NAME: &str = "dev.perfetto.example_data_source";
    static DATA_SOURCE: OnceLock<DataSource> = OnceLock::new();

    fn get_data_source() -> &'static DataSource<'static> {
        use crate::data_source::DataSourceArgsBuilder;
        DATA_SOURCE.get_or_init(|| {
            let data_source_args = DataSourceArgsBuilder::new();
            let mut data_source = DataSource::new();
            data_source
                .register(DATA_SOURCE_NAME, data_source_args.build())
                .expect("failed to register data source");
            data_source
        })
    }

    #[test]
    fn data_source() -> Result<(), Box<dyn Error>> {
        use crate::data_source::*;
        let _lock = acquire_test_environment();
        let data_source = get_data_source();
        let mut session = TracingSessionBuilder::new()
            .set_data_source_name(DATA_SOURCE_NAME)
            .build()?;
        session.start_blocking();
        assert!(data_source.is_enabled());
        let mut executed: usize = 0;
        data_source.trace(|_ctx: &mut TraceContext| {
            executed += 1;
        });
        session.stop_blocking();
        assert_eq!(executed, 1);
        Ok(())
    }

    track_event_categories! {
        pub mod session_test_te_ns {
            ( "cat1", "Test category 1", [] ),
            ( "cat2", "Test category 2", [] ),
        }
    }

    struct TeTestFixture {
        _lock: MutexGuard<'static, ()>,
    }

    impl TeTestFixture {
        fn new() -> Self {
            let _lock = acquire_test_environment();
            TrackEvent::init();
            session_test_te_ns::register().expect("register failed");
            Self { _lock }
        }
    }

    impl Drop for TeTestFixture {
        fn drop(&mut self) {
            session_test_te_ns::unregister().expect("unregister failed");
        }
    }

    #[test]
    fn track_event() -> Result<(), Box<dyn Error>> {
        use crate::{trace_for_category, track_event::TraceContext, track_event_category_enabled};
        use session_test_te_ns as perfetto_te_ns;
        let _fx = TeTestFixture::new();
        let mut session = TracingSessionBuilder::new()
            .set_data_source_name("track_event")
            .add_enabled_category("cat1")
            .add_disabled_category("*")
            .build()?;
        session.start_blocking();
        assert!(track_event_category_enabled!("cat1"));
        assert!(!track_event_category_enabled!("cat2"));
        let mut executed: usize = 0;
        trace_for_category!("cat1", |_ctx: &mut TraceContext| {
            executed += 1;
        });
        session.stop_blocking();
        assert_eq!(executed, 1);
        Ok(())
    }

    #[test]
    fn read_trace() -> Result<(), Box<dyn Error>> {
        use crate::data_source::TraceContext;
        use crate::pb_decoder::{PbDecoder, PbDecoderField};
        use crate::protos::trace::{test_event::*, trace::*, trace_packet::*};
        use std::sync::{Arc, Mutex};
        let _lock = acquire_test_environment();
        let data_source = get_data_source();
        let mut session = TracingSessionBuilder::new()
            .set_data_source_name(DATA_SOURCE_NAME)
            .build()?;
        session.start_blocking();
        assert!(data_source.is_enabled());
        data_source.trace(|ctx: &mut TraceContext| {
            ctx.add_packet(|packet: &mut TracePacket| {
                packet
                    .set_timestamp(42)
                    .set_for_testing(|for_testing: &mut TestEvent| {
                        for_testing.set_str("This is a string");
                    });
            });
        });
        session.stop_blocking();
        let trace_data = Arc::new(Mutex::new(vec![]));
        let trace_data_for_write = Arc::clone(&trace_data);
        session.read_trace_blocking(move |data, _end| {
            let mut written_data = trace_data_for_write.lock().unwrap();
            written_data.extend_from_slice(data);
        });
        let data = trace_data.lock().unwrap();
        assert!(!data.is_empty());
        let mut for_testing_found = false;
        for trace_field in PbDecoder::new(&data) {
            const PACKET_ID: u32 = TraceFieldNumber::Packet as u32;
            if let (PACKET_ID, PbDecoderField::Delimited(data)) = trace_field.unwrap() {
                for packet_field in PbDecoder::new(data) {
                    const FOR_TESTING_ID: u32 = TracePacketFieldNumber::ForTesting as u32;
                    if let (FOR_TESTING_ID, PbDecoderField::Delimited(_)) = packet_field.unwrap() {
                        for_testing_found = true;
                    }
                }
            }
        }
        assert!(for_testing_found);
        Ok(())
    }
}
