import sys


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
  buf = b'\x0a\x18\x92\x10\x15\x0a\x0f\x63\x6f\x6d\x2e\x65\x78\x61\x6d\x70\x6c\x65\x2e\x61\x70\x70\x18\x0a\x20\x00\x0a\x18\x92\x10\x15\x0a\x0f\x63\x6f\x6d\x2e\x65\x78\x61\x6d\x70\x6c\x65\x2e\x61\x70\x70\x18\x14\x20\x01\x10\x80\x94\xeb\xdc\x03\x10\x80\xe4\x97\xd0\x12'
  packet1 = encode_length_delimited(84, buf)
  sys.stdout.buffer.write(encode_length_delimited(1, packet1))


if __name__ == '__main__':
  main()
