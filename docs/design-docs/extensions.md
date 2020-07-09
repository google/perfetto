Extensions: adding new types to traces
======================================

NOTE: **extensions are work-in-progress and are not ready to be used at the
moment**

Currently, it is not possible to add new types to traces while using Perfetto
without modifying Perfetto proto message definitions upstream.

This page describes ongoing work to use [protobuf
extensions](https://developers.google.com/protocol-buffers/docs/overview#extensions)
in order to make it possible to define new typed messages outside of the
Perfetto repository.

Protozero support
-----------------

Perfetto uses its own implementation of code generation for protocol buffer
messages called [Protozero](/docs/design-docs/protozero.md), which is not a
full-fledged protobuf implementation. Implementation of extensions is fairly
limited, and all extensions are supposed to be nested inside a message that is
used in order to provide the class name for generated code.

For example,

    message MyEvent {
        extend TrackEvent {
            optional string custom_string = 1000;
        }
    }

Is going to generate a subclass of `TrackEvent` called `MyEvent`, that is going
to have a new method to set `custom_string` in addition to all other protobuf
fields defined in `TrackEvent`.

Deserialization
---------------

When analyzing traces, protos are not used directly and instead are parsed into
database, which can be queried by SQL. In order to make it possible, Perfetto
has to know field descriptors (the binary representation of the extended proto
schema) of the extensions. Currently, the only way to do that is to add an
[ExtensionDescriptor
packet](reference/trace-packet-proto.autogen#ExtensionDescriptor). In the
future, there is going to be a way to specify protobuf extensions at compile
time in order to be able to avoid this overhead in every single trace.

However, an ability to specify extension descriptors in the trace itself will
still be useful in order to be able to use a pre-compiled trace processor in the
UI when adding new typed messages during local development.

Deserialization of protobuf extension are supported only for TrackEvent message
at the moment, and is implemented in trace processor via ProtoToArgsUtils class.
The extensions will appear in args table, similar to other trace event
arguments.

Testing extensions support inside Perfetto
------------------------------------------

Perfetto trace processor is mostly tested by integration tests, where input
traces are specified most frequently in textproto format. Textproto format
supports extensions, but the parser has to be aware of all the extensions used.
In order to make it possible, all the extensions that are used in integration
tests have to be specified in the `test_extensions.proto` file. Since this file
is only used in the testing harness and is parsed by protoc, it does not have to
adhere to the convention of all extensions being inside a wrapper message, which
helps with making extension identifiers more concise.
