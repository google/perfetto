def _file_generator(path: str):
  with open(path, 'rb') as f:
    yield from _read_generator(f)


# Limit parsing file to 32MB to maintain parity with the UI
MAX_BYTES_LOADED = 32 * 1024 * 1024


def _read_generator(trace: BinaryIO):
  while True:
    chunk = trace.read(MAX_BYTES_LOADED)
    if not chunk:
      break
    yield chunk


def _merge_dicts(a: Dict[str, str], b: Dict[str, str]):
  return {**a, **b}
