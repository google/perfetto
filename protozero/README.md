ProtoZero
---------

ProtoZero is a zero-copy zero-malloc append-only protobuf library.
It's designed to be fast and efficient at the cost of a reduced API
surface for generated stubs. The main limitations consist of:
- Append-only interface: no readbacks are possible from the stubs.
- No runtime checks for duplicated or missing mandatory fields.
- Mandatory ordering when writing of nested messages: once a nested message is
  started it must be completed before adding any fields to its parent.

See also: [Design doc](https://goo.gl/EKvEfa]).
