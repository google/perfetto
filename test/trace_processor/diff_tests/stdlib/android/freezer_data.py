import sys

# Trace = repeated TracePacket (field 1)
# TracePacket = statsd_atom (field 84)
# We have the raw bytes of StatsdAtom (which we verified structurally matches ShellData)
# Let's emit a Trace protobuf manually without needing google.protobuf!


def encode_varint(n):
  res = bytearray()
  while True:
    b = n & 0x7F
    n >>= 7
    if n:
      res.append(b | 0x80)
    else:
      res.append(b)
      break
  return bytes(res)


def encode_length_delimited(field_num, data):
  tag = (field_num << 3) | 2
  return encode_varint(tag) + encode_varint(len(data)) + data


def main():
  # Derived from my ShellData output which is equivalent to StatsdAtom wire format
  buf1 = b'\x0a\x1d\xf2\x0f\x1a\x08\x01\x10\xd2\x09\x1a\x0f\x63\x6f\x6d\x2e\x65\x78\x61\x6d\x70\x6c\x65\x2e\x61\x70\x70\x20\x00\x30\x00\x10\x80\x94\xeb\xdc\x03'
  buf2 = b'\x0a\x1d\xf2\x0f\x1a\x08\x02\x10\xd2\x09\x1a\x0f\x63\x6f\x6d\x2e\x65\x78\x61\x6d\x70\x6c\x65\x2e\x61\x70\x70\x20\x04\x30\x01\x10\x80\xe4\x97\xd0\x12'

  # TracePacket containing statsd_atom (field 84)
  packet1 = encode_length_delimited(84, buf1)
  packet2 = encode_length_delimited(84, buf2)

  # Trace containing repeated packet (field 1)
  trace = encode_length_delimited(1, packet1) + encode_length_delimited(
      1, packet2)

  sys.stdout.buffer.write(trace)


if __name__ == '__main__':
  main()
