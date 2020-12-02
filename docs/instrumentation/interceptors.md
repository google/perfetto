# Trace packet interceptors (Tracing SDK)

A trace packet interceptor is used to redirect trace packets written by a
data source into a custom backend instead of the normal Perfetto tracing
service. For example, the console interceptor prints all trace packets to the
console as they are generated. Another potential use is exporting trace data
to another tracing service such as Android ATrace or Windows ETW.

An interceptor is defined by subclassing the `perfetto::Interceptor` template:

```C++
class MyInterceptor : public perfetto::Interceptor<MyInterceptor> {
 public:
  ~MyInterceptor() override = default;

  // This function is called for each intercepted trace packet. |context|
  // contains information about the trace packet as well as other state
  // tracked by the interceptor (e.g., see ThreadLocalState).
  //
  // Intercepted trace data is provided in the form of serialized protobuf
  // bytes, accessed through the |context.packet_data| field.
  //
  // Warning: this function can be called on any thread at any time. See
  // below for how to safely access shared interceptor data from here.
  static void OnTracePacket(InterceptorContext context) {
    perfetto::protos::pbzero::TracePacket::Decoder packet(
        context.packet_data.data, context.packet_data.size);
    // ... Write |packet| to the desired destination ...
  }
};
```

An interceptor should be registered before any tracing sessions are started.
Note that the interceptor also needs to be activated through the trace config
shown below.

```C++
perfetto::InterceptorDescriptor desc;
desc.set_name("my_interceptor");
MyInterceptor::Register(desc);
```

Finally, an interceptor is enabled through the trace config like this:

```C++
perfetto::TraceConfig cfg;
auto* ds_cfg = cfg.add_data_sources()->mutable_config();
ds_cfg->set_name("data_source_to_intercept");   // e.g. "track_event"
ds_cfg->mutable_interceptor_config()->set_name("my_interceptor");
```

Once an interceptor is enabled, all data from the affected data sources is
sent to the interceptor instead of the main tracing buffer.

## Interceptor state

Besides the serialized trace packet data, the `OnTracePacket` interceptor
function can access three other types of state:

1. **Global state:** this is no different from a normal static function, but
   care must be taken because |OnTracePacket| can be called concurrently on
   any thread at any time.

2. **Per-data source instance state:** since the interceptor class is
   automatically instantiated for each intercepted data source, its fields
   can be used to store per-instance data such as the trace config. This data
   can be maintained through the OnSetup/OnStart/OnStop callbacks:

   ```C++
   class MyInterceptor : public perfetto::Interceptor<MyInterceptor> {
    public:
     void OnSetup(const SetupArgs& args) override {
       enable_foo_ = args.config.interceptor_config().enable_foo();
     }

     bool enable_foo_{};
   };
   ```

   In the interceptor function this data must be accessed through a scoped
   lock for safety:

   ```C++
   class MyInterceptor : public perfetto::Interceptor<MyInterceptor> {
     ...
     static void OnTracePacket(InterceptorContext context) {
       auto my_interceptor = context.GetInterceptorLocked();
       if (my_interceptor) {
          // Access fields of MyInterceptor here.
          if (my_interceptor->enable_foo_) { ... }
       }
       ...
     }
   };
   ```

   Since accessing this data involves holding a lock, it should be done
   sparingly.

3. **Per-thread/TraceWriter state:** many data sources use interning to avoid
   repeating common data in the trace. Since the interning dictionaries are
   typically kept individually for each TraceWriter sequence (i.e., per
   thread), an interceptor can declare a data structure with lifetime
   matching the TraceWriter:

   ```C++
   class MyInterceptor : public perfetto::Interceptor<MyInterceptor> {
    public:
     struct ThreadLocalState
         : public perfetto::InterceptorBase::ThreadLocalState {
       ThreadLocalState(ThreadLocalStateArgs&) override = default;
       ~ThreadLocalState() override = default;

       std::map<size_t, std::string> event_names;
     };
   };
   ```

   This per-thread state can then be accessed and maintained in
   `OnTracePacket` like this:

   ```C++
   class MyInterceptor : public perfetto::Interceptor<MyInterceptor> {
     ...
     static void OnTracePacket(InterceptorContext context) {
       // Updating interned data.
       auto& tls = context.GetThreadLocalState();
       if (parsed_packet.sequence_flags() & perfetto::protos::pbzero::
               TracePacket::SEQ_INCREMENTAL_STATE_CLEARED) {
         tls.event_names.clear();
       }
       for (const auto& entry : parsed_packet.interned_data().event_names())
         tls.event_names[entry.iid()] = entry.name();

       // Looking up interned data.
       if (parsed_packet.has_track_event()) {
         size_t name_iid = parsed_packet.track_event().name_iid();
         const std::string& event_name = tls.event_names[name_iid];
       }
       ...
     }
   };
   ```