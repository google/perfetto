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

use crate::{
    heap_buffer::HeapBuffer,
    pb_msg::{PbMsg, PbMsgWriter},
    protos::{
        common::data_source_descriptor::DataSourceDescriptor, trace::trace_packet::TracePacket,
    },
    stream_writer::StreamWriter,
};
use perfetto_sdk_sys::*;
use std::{
    cell::RefCell,
    collections::HashMap,
    default::Default,
    marker::PhantomData,
    os::raw::c_void,
    ptr,
    sync::{
        Mutex, OnceLock,
        atomic::{AtomicBool, AtomicU64, Ordering},
    },
};
use thiserror::Error;

/// Data source errors.
#[derive(Error, Debug, PartialEq)]
pub enum DataSourceError {
    /// Data source has already been registered.
    #[error("Data source have already been registered.")]
    AlreadyRegisteredError,
    /// Unknown error occured when trying to register data source.
    #[error("Failed to register data source.")]
    RegisterError,
}

/// Opaque handle used to perform operations from the OnSetup callback. Unused
/// for now.
pub struct OnSetupArgs {
    _args: *mut PerfettoDsOnSetupArgs,
}

type OnSetupCallback = Box<dyn FnMut(u32, &[u8], &mut OnSetupArgs) + Send + Sync + 'static>;

/// Opaque handle used to perform operations from the OnSetup callback. Unused
/// for now.
pub struct OnStartArgs {
    _args: *mut PerfettoDsOnStartArgs,
}

type OnStartCallback = Box<dyn FnMut(u32, &mut OnStartArgs) + Send + Sync + 'static>;

/// A scope-based guard to signal that the data source stop operation is
/// complete when dropped.
#[must_use = "dropping StopGuard immediately defeats its purpose"]
pub struct StopGuard {
    async_stopper: *mut PerfettoDsAsyncStopper,
}

impl Drop for StopGuard {
    fn drop(&mut self) {
        // SAFETY: `self.async_stopper` must have been created using
        // `PerfettoDsOnStopArgsPostpone`.
        unsafe {
            PerfettoDsStopDone(self.async_stopper);
        }
    }
}

// SAFETY: The underlying PerfettoDsAsyncStopper is thread-safe.
unsafe impl Send for StopGuard {}

// SAFETY: The underlying PerfettoDsAsyncStopper is thread-safe.
unsafe impl Sync for StopGuard {}

/// Opaque handle used to perform operations from the OnStop callback.
pub struct OnStopArgs {
    args: *mut PerfettoDsOnStopArgs,
}

impl OnStopArgs {
    /// Tells the tracing service to postpone the stopping of a data source
    /// instance. The returned handle can be used to signal the tracing
    /// service when the data source instance can be stopped.
    #[must_use = "StopGuard must be kept alive until the desired stop point"]
    pub fn postpone(&mut self) -> StopGuard {
        assert!(!self.args.is_null());
        // SAFETY: `self.args` must be pointing to a valid PerfettoDsOnStopArgs handle.
        let async_stopper = unsafe { PerfettoDsOnStopArgsPostpone(self.args) };
        StopGuard { async_stopper }
    }
}

type OnStopCallback = Box<dyn FnMut(u32, &mut OnStopArgs) + Send + Sync + 'static>;

/// A scope-based guard to signal that the data source flush operation is
/// complete when dropped.
#[must_use = "dropping FlushGuard immediately defeats its purpose"]
pub struct FlushGuard {
    async_flusher: *mut PerfettoDsAsyncFlusher,
}

impl Drop for FlushGuard {
    fn drop(&mut self) {
        // SAFETY: `self.async_flusher` must have been created using
        // `PerfettoDsOnFlushArgsPostpone`.
        unsafe {
            PerfettoDsFlushDone(self.async_flusher);
        }
    }
}

// SAFETY: The underlying PerfettoDsAsyncFlusher is thread-safe.
unsafe impl Send for FlushGuard {}

// SAFETY: The underlying PerfettoDsAsyncFlusher is thread-safe.
unsafe impl Sync for FlushGuard {}

/// Opaque handle used to perform operations from the OnStop callback.
pub struct OnFlushArgs {
    args: *mut PerfettoDsOnFlushArgs,
}

impl OnFlushArgs {
    /// Tells the tracing service to postpone acknowledging the flushing of a data
    /// source instance. The returned guard can be used to signal the tracing
    /// service when the data source instance flushing has completed.
    #[must_use = "FlushGuard must be kept alive until the desired stop point"]
    pub fn postpone(&mut self) -> FlushGuard {
        assert!(!self.args.is_null());
        // SAFETY: `self.args` must be pointing to a valid PerfettoDsOnFlushArgs handle.
        let async_flusher = unsafe { PerfettoDsOnFlushArgsPostpone(self.args) };
        FlushGuard { async_flusher }
    }
}

type OnFlushCallback = Box<dyn FnMut(u32, &mut OnFlushArgs) + Send + Sync + 'static>;

/// Data source buffer exhausted policy.
#[derive(Default, PartialEq)]
pub enum DataSourceBufferExhaustedPolicy {
    /// If the data source runs out of space when trying to acquire a new chunk,
    /// it will drop data.
    #[default]
    Drop,
    /// If the data source runs out of space when trying to acquire a new chunk,
    /// it will stall, retry and eventually abort if a free chunk is not acquired
    /// after a few seconds.
    StallAndAbort,
    /// If the data source runs out of space when trying to acquire a new chunk,
    /// it will stall, retry and eventually drop data if a free chunk is not
    /// acquired after a few seconds.
    StallAndDrop,
}

pub(crate) trait ToDsBufferExhaustedPolicy {
    fn to_ds_policy(&self) -> PerfettoDsBufferExhaustedPolicy;
}

impl ToDsBufferExhaustedPolicy for DataSourceBufferExhaustedPolicy {
    fn to_ds_policy(&self) -> PerfettoDsBufferExhaustedPolicy {
        use DataSourceBufferExhaustedPolicy::*;
        match self {
            Drop => PerfettoDsBufferExhaustedPolicy_PERFETTO_DS_BUFFER_EXHAUSTED_POLICY_DROP,
            StallAndAbort => {
                PerfettoDsBufferExhaustedPolicy_PERFETTO_DS_BUFFER_EXHAUSTED_POLICY_STALL_AND_ABORT
            }
            StallAndDrop => {
                PerfettoDsBufferExhaustedPolicy_PERFETTO_DS_BUFFER_EXHAUSTED_POLICY_STALL_AND_DROP
            }
        }
    }
}

#[derive(Default)]
struct DsCallbacks {
    on_setup: Option<OnSetupCallback>,
    on_start: Option<OnStartCallback>,
    on_stop: Option<OnStopCallback>,
    on_flush: Option<OnFlushCallback>,
}

/// Data source arguments struct.
#[derive(Default)]
pub struct DataSourceArgs {
    callbacks: DsCallbacks,
    buffer_exhausted_policy: DataSourceBufferExhaustedPolicy,
    buffer_exhausted_policy_configurable: bool,
    will_notify_on_stop: bool,
    handles_incremental_state_clear: bool,
}

/// Data source arguments builder.
#[derive(Default)]
#[must_use = "This is a builder; remember to call `.build()` (or keep chaining)."]
pub struct DataSourceArgsBuilder {
    args: DataSourceArgs,
}

impl DataSourceArgsBuilder {
    /// Create new data source arguments builder.
    pub fn new() -> Self {
        Self::default()
    }

    /// Set buffer exhausted policy.
    #[must_use = "Builder methods return an updated builder; use the returned value or keep chaining."]
    pub fn buffer_exhausted_policy(
        mut self,
        buffer_exhausted_policy: DataSourceBufferExhaustedPolicy,
    ) -> Self {
        self.args.buffer_exhausted_policy = buffer_exhausted_policy;
        self
    }

    /// Set buffer exhausted policy configurable flag.
    #[must_use = "Builder methods return an updated builder; use the returned value or keep chaining."]
    pub fn buffer_exhausted_policy_configurable(
        mut self,
        buffer_exhausted_policy_configurable: bool,
    ) -> Self {
        self.args.buffer_exhausted_policy_configurable = buffer_exhausted_policy_configurable;
        self
    }

    /// Set notify on stop flag.
    #[must_use = "Builder methods return an updated builder; use the returned value or keep chaining."]
    pub fn will_notify_on_stop(mut self, will_notify_on_stop: bool) -> Self {
        self.args.will_notify_on_stop = will_notify_on_stop;
        self
    }

    /// Set incremental state clear flag.
    #[must_use = "Builder methods return an updated builder; use the returned value or keep chaining."]
    pub fn handles_incremental_state_clear(
        mut self,
        handles_incremental_state_clear: bool,
    ) -> Self {
        self.args.handles_incremental_state_clear = handles_incremental_state_clear;
        self
    }

    /// Set setup callback.
    #[must_use = "Builder methods return an updated builder; use the returned value or keep chaining."]
    pub fn on_setup<F>(mut self, cb: F) -> Self
    where
        F: FnMut(u32, &[u8], &mut OnSetupArgs) + Send + Sync + 'static,
    {
        self.args.callbacks.on_setup = Some(Box::new(cb));
        self
    }

    /// Set start callback.
    #[must_use = "Builder methods return an updated builder; use the returned value or keep chaining."]
    pub fn on_start<F>(mut self, cb: F) -> Self
    where
        F: FnMut(u32, &mut OnStartArgs) + Send + Sync + 'static,
    {
        self.args.callbacks.on_start = Some(Box::new(cb));
        self
    }

    /// Set stop callback.
    #[must_use = "Builder methods return an updated builder; use the returned value or keep chaining."]
    pub fn on_stop<F>(mut self, cb: F) -> Self
    where
        F: FnMut(u32, &mut OnStopArgs) + Send + Sync + 'static,
    {
        self.args.callbacks.on_stop = Some(Box::new(cb));
        self
    }

    /// Set flush callback.
    #[must_use = "Builder methods return an updated builder; use the returned value or keep chaining."]
    pub fn on_flush<F>(mut self, cb: F) -> Self
    where
        F: FnMut(u32, &mut OnFlushArgs) + Send + Sync + 'static,
    {
        self.args.callbacks.on_flush = Some(Box::new(cb));
        self
    }

    /// Returns data source arguments struct.
    pub fn build(self) -> DataSourceArgs {
        self.args
    }
}

type FlushCallback = Box<dyn FnMut() + Send + Sync + 'static>;

// Flush callbacks are not guaranteed to be called so store them in a global
// map to prevent them from leaking. Uncalled callbacks are not currently
// removed from the map as it's hard to determine when it is safe to do so,
// which should be fine as an occurrence of such a callback is rare.
static NEXT_FLUSH_ID: AtomicU64 = AtomicU64::new(1);
static FLUSH_CALLBACKS: OnceLock<Mutex<HashMap<u64, FlushCallback>>> = OnceLock::new();

fn flush_callbacks() -> &'static Mutex<HashMap<u64, FlushCallback>> {
    FLUSH_CALLBACKS.get_or_init(|| Mutex::new(HashMap::new()))
}

// Register a flush callback, returns the sequence ID.
fn register_flush_callback(cb: FlushCallback) -> u64 {
    let id = NEXT_FLUSH_ID.fetch_add(1, Ordering::Relaxed);
    flush_callbacks().lock().unwrap().insert(id, cb);
    id
}

// Remove the flush callback (if present) and return it.
fn take_flush_callback(id: u64) -> Option<FlushCallback> {
    flush_callbacks().lock().unwrap().remove(&id)
}

unsafe extern "C" fn flush_callback_trampoline(user_arg: *mut c_void) {
    let result = std::panic::catch_unwind(|| {
        // Decode the callback `id`.
        let id = user_arg as usize as u64;
        // Remove flush callback, which will be dropped at the end of the scope.
        if let Some(mut cb) = take_flush_callback(id) {
            cb();
        }
    });
    if let Err(err) = result {
        eprintln!("Fatal panic: {:?}", err);
        std::process::abort();
    }
}

/// Trace context base struct with passed to data source and track event trace callbacks.
pub struct TraceContextBase {
    pub(crate) iterator: PerfettoDsImplTracerIterator,
}

impl TraceContextBase {
    /// Creates new trace packets and calls `cb` to write data to each of the packets.
    pub fn add_packet<F>(&mut self, mut cb: F)
    where
        F: FnMut(&mut TracePacket),
    {
        let writer = PbMsgWriter {
            writer: StreamWriter {
                // Returns a writer that must be freed using `PerfettoDsTracerImplPacketEnd`.
                //
                // SAFETY:
                //
                // - `self.iterator.tracer` must be a pointer provided by a call to
                //   PerfettoDsImplTraceIterateBegin/Next.
                writer: RefCell::new(unsafe {
                    PerfettoDsTracerImplPacketBegin(self.iterator.tracer)
                }),
            },
        };
        let mut msg = PbMsg::new(&writer).unwrap();
        let mut packet = TracePacket { msg: &mut msg };

        cb(&mut packet);

        packet.msg.finalize();

        let mut inner_writer = writer.writer.writer.borrow_mut();
        // SAFETY:
        //
        // Free writer created above using `PerfettoDsTracerImplPacketBegin`.
        unsafe {
            PerfettoDsTracerImplPacketEnd(self.iterator.tracer, &mut *inner_writer as *mut _);
        }
    }

    /// Forces a commit of the thread-local tracing data written so far to the
    /// service.
    ///
    /// `cb` is called on a dedicated internal thread, when flushing is complete.
    /// It may never be called (e.g. if the tracing service disconnects).
    ///
    /// This is almost never required (tracing data is periodically committed as
    /// trace pages are filled up) and has a non-negligible performance hit.
    pub fn flush<F>(&mut self, cb: F)
    where
        F: FnMut() + Send + Sync + 'static,
    {
        let id = register_flush_callback(Box::new(cb));
        // Encode the callback `id` as a `*mut c_void`.
        let user_arg = id as usize as *mut c_void;
        // SAFETY: callback identified by `id` must be safe to call on any thread.
        unsafe {
            PerfettoDsTracerImplFlush(
                self.iterator.tracer,
                Some(flush_callback_trampoline),
                user_arg,
            )
        };
    }

    /// Returns the index of the current instance.
    pub fn instance_index(&self) -> u32 {
        self.iterator.inst_id
    }
}

/// Default incremental state struct used if not specified.
pub struct IncrementalState {
    /// Set to true when incremental state has been cleared and not yet acknowledged by
    /// a call to with_incremental_state that sets it to false.
    pub was_cleared: bool,
}

impl Default for IncrementalState {
    fn default() -> Self {
        Self { was_cleared: true }
    }
}

/// Trace context struct passed to data source trace callbacks.
pub struct TraceContext<'a, IncrT: Default = IncrementalState> {
    base: TraceContextBase,
    pub(crate) impl_: *mut PerfettoDsImpl,
    pub(crate) _marker: PhantomData<&'a IncrT>,
}

impl<IncrT: Default> TraceContext<'_, IncrT> {
    /// Calls `cb` with the incremental state for the instance.
    pub fn with_incremental_state<F>(&mut self, mut cb: F)
    where
        F: FnMut(&mut Self, &mut IncrT),
    {
        assert!(!self.impl_.is_null());
        // SAFETY:
        //
        // - `self.impl_` must be non-null.
        // - `self.iterator.tracer` must be a pointer provided by a call to
        //   PerfettoDsImplTraceIterateBegin/Next.
        // - `self.iterator.inst_id` must be set by a call to
        //   PerfettoDsImplTraceIterateBegin/Next.
        let ptr = unsafe {
            PerfettoDsImplGetIncrementalState(
                self.impl_,
                self.base.iterator.tracer,
                self.base.iterator.inst_id,
            )
        };
        if ptr.is_null() {
            panic!("missing incremental state");
        }
        // SAFETY:
        //
        // - `buf` must be non-null.
        // - `IncrT` must match the generic type used for on_create_incr_trampoline.
        let state: &mut IncrT = unsafe { &mut *(ptr as *mut IncrT) };
        cb(self, state);
    }
}

impl<IncrT: Default> std::ops::Deref for TraceContext<'_, IncrT> {
    type Target = TraceContextBase;
    fn deref(&self) -> &Self::Target {
        &self.base
    }
}

impl<IncrT: Default> std::ops::DerefMut for TraceContext<'_, IncrT> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.base
    }
}

/// Data source struct.
pub struct DataSource<'a: 'static, IncrT: Default = IncrementalState> {
    enabled: *mut bool,
    impl_: *mut PerfettoDsImpl,
    callbacks: Mutex<Option<Box<DsCallbacks>>>,
    _marker: PhantomData<&'a IncrT>,
}

unsafe extern "C" fn on_setup_callback_trampoline(
    _ds: *mut PerfettoDsImpl,
    inst_id: PerfettoDsInstanceIndex,
    ds_config: *mut c_void,
    ds_config_size: usize,
    user_arg: *mut c_void,
    args: *mut PerfettoDsOnSetupArgs,
) -> *mut c_void {
    let result = std::panic::catch_unwind(|| {
        // SAFETY: `user_arg` must be a pointer to a boxed DsCallbacks struct.
        let callbacks: &mut DsCallbacks = unsafe { &mut *(user_arg as *mut _) };
        if let Some(f) = &mut callbacks.on_setup {
            // SAFETY:
            // - `ds_config` must be non-null.
            // - `ds_config_size` bytes starting at `ptr` must be valid for **reads**.
            let config =
                unsafe { std::slice::from_raw_parts(ds_config as *const u8, ds_config_size) };
            let mut on_setup_args = OnSetupArgs { _args: args };
            f(inst_id, config, &mut on_setup_args);
        }
    });
    if let Err(err) = result {
        eprintln!("Fatal panic: {:?}", err);
        std::process::abort();
    }
    // Instance contexts are not supported as preferably handled by the
    // client in Rust code.
    ptr::null_mut()
}

unsafe extern "C" fn on_start_callback_trampoline(
    _ds: *mut PerfettoDsImpl,
    inst_id: PerfettoDsInstanceIndex,
    user_arg: *mut c_void,
    _inst_ctx: *mut c_void,
    args: *mut PerfettoDsOnStartArgs,
) {
    let result = std::panic::catch_unwind(|| {
        // SAFETY: `user_arg` must be a pointer to a boxed DsCallbacks struct.
        let callbacks: &mut DsCallbacks = unsafe { &mut *(user_arg as *mut _) };
        if let Some(f) = &mut callbacks.on_start {
            let mut on_start_args = OnStartArgs { _args: args };
            f(inst_id, &mut on_start_args);
        }
    });
    if let Err(err) = result {
        eprintln!("Fatal panic: {:?}", err);
        std::process::abort();
    }
}

unsafe extern "C" fn on_stop_callback_trampoline(
    _ds: *mut PerfettoDsImpl,
    inst_id: PerfettoDsInstanceIndex,
    user_arg: *mut c_void,
    _inst_ctx: *mut c_void,
    args: *mut PerfettoDsOnStopArgs,
) {
    let result = std::panic::catch_unwind(|| {
        // SAFETY: `user_arg` must be a pointer to a boxed DsCallbacks struct.
        let callbacks: &mut DsCallbacks = unsafe { &mut *(user_arg as *mut _) };
        if let Some(f) = &mut callbacks.on_stop {
            let mut on_stop_args = OnStopArgs { args };
            f(inst_id, &mut on_stop_args);
        }
    });
    if let Err(err) = result {
        eprintln!("Fatal panic: {:?}", err);
        std::process::abort();
    }
}

unsafe extern "C" fn on_flush_callback_trampoline(
    _ds: *mut PerfettoDsImpl,
    inst_id: PerfettoDsInstanceIndex,
    user_arg: *mut c_void,
    _inst_ctx: *mut c_void,
    args: *mut PerfettoDsOnFlushArgs,
) {
    let result = std::panic::catch_unwind(|| {
        // SAFETY: `user_arg` must be a pointer to a boxed DsCallbacks struct.
        let callbacks: &mut DsCallbacks = unsafe { &mut *(user_arg as *mut _) };
        if let Some(f) = &mut callbacks.on_flush {
            let mut on_flush_args = OnFlushArgs { args };
            f(inst_id, &mut on_flush_args);
        }
    });
    if let Err(err) = result {
        eprintln!("Fatal panic: {:?}", err);
        std::process::abort();
    }
}

unsafe extern "C" fn on_create_incr_trampoline<IncrT: Default>(
    _ds: *mut PerfettoDsImpl,
    _inst_id: PerfettoDsInstanceIndex,
    _tracer: *mut PerfettoDsTracerImpl,
    _user_arg: *mut c_void,
) -> *mut c_void {
    let boxed = Box::new(IncrT::default());
    Box::into_raw(boxed) as *mut c_void
}

unsafe extern "C" fn on_delete_incr_trampoline<IncrT: Default>(data: *mut c_void) {
    // Reclaims the Box and calls drop.
    //
    // SAFETY: `data` must be a pointer to a boxed IncrT struct.
    unsafe { drop(Box::from_raw(data as *mut IncrT)) };
}

impl<'a: 'static, IncrT: Default> DataSource<'a, IncrT> {
    /// Create new data source type with a non-default `IncrT` type.
    pub fn new_with_incremental_state_type() -> Self {
        Self::default()
    }

    /// Registers the data source type named `name` with the global ewperfetto producer.
    pub fn register(&mut self, name: &str, args: DataSourceArgs) -> Result<(), DataSourceError> {
        use DataSourceError::*;
        let mut callbacks = self.callbacks.lock().unwrap();
        if callbacks.is_some() {
            return Err(AlreadyRegisteredError);
        }
        let mut boxed_callbacks = Box::new(args.callbacks);
        let user_arg = crate::__box_as_mut_ptr(&mut boxed_callbacks) as *mut c_void;

        let writer = PbMsgWriter::new();
        let hb = HeapBuffer::new(&writer.writer);
        let mut msg = PbMsg::new(&writer).unwrap();
        {
            let mut desc = DataSourceDescriptor { msg: &mut msg };
            desc.set_name(name);
            desc.set_will_notify_on_stop(args.will_notify_on_stop);
            desc.set_handles_incremental_state_clear(args.handles_incremental_state_clear);
        }
        msg.finalize();
        let desc_size = writer.writer.get_written_size();
        let mut desc_buffer: Vec<u8> = vec![0u8; desc_size];
        hb.copy_into(&mut desc_buffer);
        // SAFETY:
        // - `self.enabled` must be a pointer to a primitive with layout that matches C11
        //   atomic_bool.
        // - `desc_buffer` must be an encoded DataSourceDescriptor messaage.
        let ds_impl = unsafe {
            let ds_impl = PerfettoDsImplCreate();
            PerfettoDsSetOnSetupCallback(ds_impl, Some(on_setup_callback_trampoline));
            PerfettoDsSetOnStartCallback(ds_impl, Some(on_start_callback_trampoline));
            PerfettoDsSetOnStopCallback(ds_impl, Some(on_stop_callback_trampoline));
            PerfettoDsSetOnFlushCallback(ds_impl, Some(on_flush_callback_trampoline));
            PerfettoDsSetOnCreateIncr(ds_impl, Some(on_create_incr_trampoline::<IncrT>));
            PerfettoDsSetOnDeleteIncr(ds_impl, Some(on_delete_incr_trampoline::<IncrT>));
            PerfettoDsSetCbUserArg(ds_impl, user_arg);
            PerfettoDsSetBufferExhaustedPolicy(
                ds_impl,
                args.buffer_exhausted_policy.to_ds_policy(),
            );
            PerfettoDsSetBufferExhaustedPolicyConfigurable(
                ds_impl,
                args.buffer_exhausted_policy_configurable,
            );
            let success = PerfettoDsImplRegister(
                ds_impl,
                &raw mut self.enabled,
                desc_buffer.as_mut_ptr() as *mut c_void,
                desc_size,
            );
            if !success {
                return Err(RegisterError);
            }
            ds_impl
        };
        self.impl_ = ds_impl;
        callbacks.replace(boxed_callbacks);
        Ok(())
    }

    /// Returns true if any active instance exists of data source type.
    pub fn is_enabled(&self) -> bool {
        // SAFETY: `self.enabled` must be a pointer to a primitive with layout that
        // matches C11 atomic_bool.
        unsafe {
            let atomic_ptr = self.enabled as *const AtomicBool;
            (*atomic_ptr).load(Ordering::Relaxed)
        }
    }

    /// Call `cb` for all the active instances (on this thread) of a data source type.
    pub fn trace<F>(&self, mut cb: F)
    where
        F: FnMut(&mut TraceContext<'_, IncrT>),
    {
        // It is safe to call this prior to registering the data source as self.is_enabled()
        // will return false in that case.
        if crate::__unlikely!(self.is_enabled()) {
            assert!(!self.impl_.is_null());
            let mut ctx = TraceContext::<'_, IncrT> {
                base: TraceContextBase {
                    // SAFETY: `self.impl_` must be a pointer to a registered data source. Ie.
                    // non-null and passed to a successful PerfettoDsImplRegister() call. Guaranteed
                    // to be the case as is_enabled() will always return false otherwise and this
                    // cannot be reached.
                    iterator: unsafe { PerfettoDsImplTraceIterateBegin(self.impl_) },
                },
                impl_: self.impl_,
                _marker: PhantomData,
            };
            loop {
                if ctx.base.iterator.tracer.is_null() {
                    break;
                }

                cb(&mut ctx);

                // SAFETY: `self.impl_` must be a pointer to a registered data source. Guaranteed
                // to be the case as is_enabled() will always return false otherwise and this
                // cannot be reached.
                unsafe { PerfettoDsImplTraceIterateNext(self.impl_, &raw mut ctx.base.iterator) };
            }
        }
    }
}

// Monomorphic `new()` on the defaulted type.
impl<'a: 'static> DataSource<'a, IncrementalState> {
    /// Create new data source type.
    pub fn new() -> Self {
        Self::default()
    }
}

impl<'a: 'static, IncrT: Default> Default for DataSource<'a, IncrT> {
    fn default() -> Self {
        Self {
            // `perfetto_atomic_false` is a pointer to a primitive with layout that
            // matches C11 atomic_bool and set to false.
            enabled: &raw mut perfetto_atomic_false,
            impl_: ptr::null_mut(),
            callbacks: Mutex::new(None),
            _marker: PhantomData,
        }
    }
}

/// SAFETY: Internal handle must be thread-safe.
unsafe impl<'a: 'static, IncrT: Default> Send for DataSource<'a, IncrT> {}

/// SAFETY: Internal handle must be thread-safe.
unsafe impl<'a: 'static, IncrT: Default> Sync for DataSource<'a, IncrT> {}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::tests::{
        PRODUCER_SHMEM_SIZE_HINT_KB, TracingSessionBuilder, acquire_test_environment,
    };
    use std::{error::Error, sync::OnceLock};

    const DATA_SOURCE_NAME: &str = "com.example.custom_data_source";
    static DATA_SOURCE: OnceLock<DataSource> = OnceLock::new();

    fn get_data_source() -> &'static DataSource<'static> {
        DATA_SOURCE.get_or_init(|| {
            let data_source_args = DataSourceArgsBuilder::new()
                .buffer_exhausted_policy(DataSourceBufferExhaustedPolicy::StallAndAbort);
            let mut data_source = DataSource::new();
            data_source
                .register(DATA_SOURCE_NAME, data_source_args.build())
                .expect("failed to register data source");
            data_source
        })
    }

    #[test]
    fn is_enabled() -> Result<(), Box<dyn Error>> {
        let _lock = acquire_test_environment();
        let data_source = get_data_source();
        assert!(!data_source.is_enabled());
        let mut session = TracingSessionBuilder::new()
            .set_data_source_name(DATA_SOURCE_NAME)
            .build()?;
        session.start_blocking();
        assert!(data_source.is_enabled());
        session.stop_blocking();
        Ok(())
    }

    #[test]
    fn trace() -> Result<(), Box<dyn Error>> {
        use crate::pb_decoder::{PbDecoder, PbDecoderField};
        use crate::protos::trace::{test_event::*, trace::*, trace_packet::*};
        use std::sync::{Arc, Mutex};
        let _lock = acquire_test_environment();
        let data_source = get_data_source();
        let mut session = TracingSessionBuilder::new()
            .set_data_source_name(DATA_SOURCE_NAME)
            .build()?;
        session.start_blocking();
        data_source.trace(|ctx: &mut TraceContext| {
            ctx.add_packet(|packet: &mut TracePacket| {
                packet.set_for_testing(|for_testing: &mut TestEvent| {
                    for_testing.set_str("123");
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
        let mut test_str = String::new();
        for trace_field in PbDecoder::new(&data) {
            const PACKET_ID: u32 = TraceFieldNumber::Packet as u32;
            if let (PACKET_ID, PbDecoderField::Delimited(data)) = trace_field.unwrap() {
                for packet_field in PbDecoder::new(data) {
                    const FOR_TESTING_ID: u32 = TracePacketFieldNumber::ForTesting as u32;
                    if let (FOR_TESTING_ID, PbDecoderField::Delimited(data)) = packet_field.unwrap()
                    {
                        for test_event_field in PbDecoder::new(data) {
                            const STR_ID: u32 = TestEventFieldNumber::Str as u32;
                            if let (STR_ID, PbDecoderField::Delimited(value)) =
                                test_event_field.unwrap()
                            {
                                test_str = String::from_utf8(value.to_vec()).unwrap()
                            }
                        }
                    }
                }
            }
        }
        assert_eq!(&test_str, "123");
        Ok(())
    }

    #[test]
    fn trace_large_packet() -> Result<(), Box<dyn Error>> {
        use crate::pb_decoder::{PbDecoder, PbDecoderField};
        use crate::protos::trace::{test_event::*, trace::*, trace_packet::*};
        use std::sync::{Arc, Mutex};
        let _lock = acquire_test_environment();
        let data_source = get_data_source();
        let mut session = TracingSessionBuilder::new()
            .set_data_source_name(DATA_SOURCE_NAME)
            .build()?;
        session.start_blocking();
        // Large enough to exceed the producer shmem size.
        let super_long_test_string = "a".repeat(1024 * (PRODUCER_SHMEM_SIZE_HINT_KB as usize + 10));
        data_source.trace(|ctx: &mut TraceContext| {
            ctx.add_packet(|packet: &mut TracePacket| {
                packet.set_for_testing(|for_testing: &mut TestEvent| {
                    for_testing.set_str(&super_long_test_string);
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
        let mut test_str = String::new();
        for trace_field in PbDecoder::new(&data) {
            const PACKET_ID: u32 = TraceFieldNumber::Packet as u32;
            if let (PACKET_ID, PbDecoderField::Delimited(data)) = trace_field.unwrap() {
                for packet_field in PbDecoder::new(data) {
                    const FOR_TESTING_ID: u32 = TracePacketFieldNumber::ForTesting as u32;
                    if let (FOR_TESTING_ID, PbDecoderField::Delimited(data)) = packet_field.unwrap()
                    {
                        for test_event_field in PbDecoder::new(data) {
                            const STR_ID: u32 = TestEventFieldNumber::Str as u32;

                            if let (STR_ID, PbDecoderField::Delimited(value)) =
                                test_event_field.unwrap()
                            {
                                test_str = String::from_utf8(value.to_vec()).unwrap();
                            }
                        }
                    }
                }
            }
        }
        assert_eq!(&test_str, &super_long_test_string);
        Ok(())
    }
}
