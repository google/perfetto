# Copyright (C) 2026 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from perfetto.trace_processor.protos import ProtoFactory


def _not_supported_yet(what: str) -> NotImplementedError:
  return NotImplementedError(
      f'{what} is not supported yet when connected to a trace processor '
      'session; only query() is.')


class TraceProcessorRpc:
  """Client side of the trace processor RPC protocol.

  Speaks the same byte protocol as the C++ RemoteTraceProcessor
  (src/trace_processor/rpc/remote_trace_processor.cc): each request is a
  serialized TraceProcessorRpcStream holding one TraceProcessorRpc; the
  server answers with a stream of TraceProcessorRpc messages, each framed
  as a length-delimited protobuf field (tag 0x0A, varint length, payload).

  Works over any transport providing send(bytes), recv(max_bytes) and close().
  """

  def __init__(self, transport, protos: ProtoFactory):
    self.transport = transport
    self.protos = protos

  def execute_query(self, query: str):
    """Runs `query` and returns a single QueryResult.

    The server streams the result as several messages, each carrying one
    batch of cells and only the last one flagged is_last_batch. They are
    merged here so callers get the same shape TraceProcessorHttp returns.
    """
    request = self.protos.TraceProcessorRpc()
    request.request = self.protos.TraceProcessorRpc.TPM_QUERY_STREAMING
    request.query_args.sql_query = query
    self._send_request(request)

    result = self.protos.QueryResult()
    while True:
      msg = self._read_message()
      # Repeated fields (batch, column_names) are appended; scalar fields
      # (error, statement_count) are overwritten if set in this message.
      result.MergeFrom(msg.query_result)
      if result.batch and result.batch[-1].is_last_batch:
        return result

  def close(self):
    self.transport.close()

  # The methods below complete the client interface TraceProcessor expects
  # (see TraceProcessorHttp). The protocol supports them, but they are not
  # implemented over this client yet, so fail with a clear message instead of
  # an AttributeError naming an internal method.

  def compute_metric(self, metrics):
    raise _not_supported_yet('metric()')

  def trace_summary(self, specs, metric_ids=None, metadata_query_id=None):
    raise _not_supported_yet('trace_summary()')

  def enable_metatrace(self):
    raise _not_supported_yet('enable_metatrace()')

  def disable_and_read_metatrace(self):
    raise _not_supported_yet('disable_and_read_metatrace()')

  def export(self, output_file, export_format):
    raise _not_supported_yet('export()')

  def parse(self, chunk):
    raise _not_supported_yet('Loading a trace')

  def notify_eof(self):
    raise _not_supported_yet('Loading a trace')

  def _send_request(self, request):
    """Wraps one TraceProcessorRpc in a TraceProcessorRpcStream and sends it."""
    stream = self.protos.TraceProcessorRpcStream()
    stream.msg.add().CopyFrom(request)
    self.transport.send(stream.SerializeToString())

  def _recv_exactly(self, n: int) -> bytes:
    """Reads exactly `n` bytes; recv() alone may return fewer."""
    buf = b''
    while len(buf) < n:
      chunk = self.transport.recv(n - len(buf))
      if not chunk:
        # recv() returns empty bytes once the peer has disconnected.
        raise ConnectionError('Trace processor session closed the connection')
      buf += chunk
    return buf

  def _read_varint(self) -> int:
    """Reads a protobuf varint (base-128, little-endian, MSB = continuation)."""
    result = 0
    shift = 0
    while True:
      # Indexing into 'bytes' produces an 'int'.
      byte = self._recv_exactly(1)[0]
      # Each varint byte carries its payload in the low 7 bits, least
      # significant group first, so shift each new group further left.
      result |= (byte & 0x7F) << shift
      if not byte & 0x80:
        # The last byte of a varint has its most significant bit cleared.
        return result
      shift += 7

  def _read_message(self):
    """Reads and parses the next TraceProcessorRpc off the stream."""
    # Every message is TraceProcessorRpcStream.msg (field 1, length-delimited),
    # whose tag is (1 << 3) | 2 == 0x0A.
    tag = self._recv_exactly(1)[0]
    if tag != 0x0A:
      raise ValueError(f'Unexpected tag {tag:#x} in trace processor stream')
    length = self._read_varint()
    data = self._recv_exactly(length)
    msg = self.protos.TraceProcessorRpc()
    msg.ParseFromString(data)
    return msg
