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
  buf = b'\x0a\x0f\x42\x0d\x0a\x03\x08\x91\x4e\x12\x04\x6a\x6f\x62\x31\x18\x01\x0a\x0f\x42\x0d\x0a\x03\x08\x91\x4e\x12\x04\x6a\x6f\x62\x31\x18\x02\x10\x80\x94\xeb\xdc\x03\x10\x80\xe4\x97\xd0\x12'
  packet1 = encode_length_delimited(84, buf)
  sys.stdout.buffer.write(encode_length_delimited(1, packet1))


if __name__ == '__main__':
  main()
