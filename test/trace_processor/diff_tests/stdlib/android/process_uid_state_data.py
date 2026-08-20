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
  buf_uid = b'\x0a\x08\xda\x01\x05\x08\x91\x4e\x10\x02\x0a\x08\xda\x01\x05\x08\x91\x4e\x10\x04\x10\xe8\x07\x10\x88\x27'
  packet1 = encode_length_delimited(84, buf_uid)
  sys.stdout.buffer.write(encode_length_delimited(1, packet1))


if __name__ == '__main__':
  main()
