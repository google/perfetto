#!/usr/bin/env python3
"""Expands interned TrackEvent fields by replacing IIDs with their resolved
string values. Works directly on the protobuf wire format, no generated code
needed.

Handles:
  TrackEvent:
    - name_iid (10) -> name (23)
    - category_iids (3) -> categories (22)
    - source_location_iid (34) -> source_location (33), inline submessage
  DebugAnnotation (recursively, including nested dict_entries/array_values):
    - name_iid (1) -> name (10)
    - string_value_iid (17) -> string_value (6)
    - proto_type_name_iid (13) -> proto_type_name (16)
"""

import sys

# Wire types
WT_VARINT = 0
WT_FIXED64 = 1
WT_LEN = 2
WT_FIXED32 = 5


def decode_varint(data, pos):
    result = 0
    shift = 0
    while pos < len(data):
        byte = data[pos]
        result |= (byte & 0x7F) << shift
        pos += 1
        shift += 7
        if (byte & 0x80) == 0:
            break
    return result, pos


def encode_varint(value):
    out = bytearray()
    while value > 0x7F:
        out.append((value & 0x7F) | 0x80)
        value >>= 7
    out.append(value & 0x7F)
    return bytes(out)


def encode_tag(field_number, wire_type):
    return encode_varint((field_number << 3) | wire_type)


def encode_len_field(field_number, data):
    return encode_tag(field_number, 2) + encode_varint(len(data)) + data


def encode_varint_field(field_number, value):
    return encode_tag(field_number, 0) + encode_varint(value)


def parse_fields(data):
    """Parse all fields from a protobuf message. Returns list of
    (field_num, wire_type, value, raw_bytes)."""
    fields = []
    pos = 0
    while pos < len(data):
        start = pos
        tag, pos = decode_varint(data, pos)
        field_num = tag >> 3
        wire_type = tag & 0x7
        if wire_type == WT_VARINT:
            value, pos = decode_varint(data, pos)
        elif wire_type == WT_FIXED64:
            value = data[pos:pos + 8]
            pos += 8
        elif wire_type == WT_LEN:
            length, pos = decode_varint(data, pos)
            value = data[pos:pos + length]
            pos += length
        elif wire_type == WT_FIXED32:
            value = data[pos:pos + 4]
            pos += 4
        else:
            raise ValueError(f'Unknown wire type {wire_type} for field '
                             f'{field_num} at offset {start}')
        fields.append((field_num, wire_type, value, data[start:pos]))
    return fields


def parse_iid_name(data):
    """Parse a message with iid (field 1, varint) and name (field 2, bytes)."""
    iid = None
    name = None
    for fnum, wt, val, _ in parse_fields(data):
        if fnum == 1 and wt == WT_VARINT:
            iid = val
        elif fnum == 2 and wt == WT_LEN:
            name = val
    return iid, name


def parse_source_location(data):
    """Parse a SourceLocation: iid=1, file_name=2, function_name=3,
    line_number=4.  Returns (iid, submessage_bytes_without_iid)."""
    iid = None
    rest = bytearray()
    for fnum, wt, val, raw in parse_fields(data):
        if fnum == 1 and wt == WT_VARINT:
            iid = val
        else:
            rest += raw
    return iid, bytes(rest)


class InternState:
    """Accumulates interned data across packets."""
    def __init__(self):
        self.event_names = {}          # iid -> name (bytes)
        self.event_categories = {}     # iid -> name (bytes)
        self.debug_ann_names = {}      # iid -> name (bytes)
        self.debug_ann_str_values = {} # iid -> string (bytes)
        self.debug_ann_vtype_names = {}# iid -> name (bytes)
        self.source_locations = {}     # iid -> submessage bytes (no iid)

    def absorb(self, interned_data_bytes):
        """Parse an InternedData message and merge into running maps."""
        for fnum, wt, val, _ in parse_fields(interned_data_bytes):
            if wt != WT_LEN:
                continue
            if fnum == 2:    # event_names
                iid, name = parse_iid_name(val)
                if iid is not None and name is not None:
                    self.event_names[iid] = name
            elif fnum == 1:  # event_categories
                iid, name = parse_iid_name(val)
                if iid is not None and name is not None:
                    self.event_categories[iid] = name
            elif fnum == 3:  # debug_annotation_names
                iid, name = parse_iid_name(val)
                if iid is not None and name is not None:
                    self.debug_ann_names[iid] = name
            elif fnum == 29: # debug_annotation_string_values
                iid, name = parse_iid_name(val)
                if iid is not None and name is not None:
                    self.debug_ann_str_values[iid] = name
            elif fnum == 27: # debug_annotation_value_type_names
                iid, name = parse_iid_name(val)
                if iid is not None and name is not None:
                    self.debug_ann_vtype_names[iid] = name
            elif fnum == 4:  # source_locations
                iid, body = parse_source_location(val)
                if iid is not None:
                    self.source_locations[iid] = body

    def strip_expanded(self, interned_data_bytes):
        """Return InternedData with the expanded field types removed."""
        # InternedData fields we fully expand. Note: source_locations (4)
        # is NOT stripped because TaskExecution.posted_from_iid still
        # references it and has no inline variant.
        STRIP = {1, 2, 3, 27, 29}
        out = bytearray()
        for fnum, wt, val, raw in parse_fields(interned_data_bytes):
            if fnum in STRIP:
                continue
            out += raw
        return bytes(out) if out else None


def rewrite_debug_annotation(data, state):
    """Rewrite a single DebugAnnotation submessage, recursively handling
    dict_entries (field 11) and array_values (field 12)."""
    fields = parse_fields(data)
    out = bytearray()
    for fnum, wt, val, raw in fields:
        # DebugAnnotation.name_iid (1, varint) -> name (10, string)
        if fnum == 1 and wt == WT_VARINT:
            resolved = state.debug_ann_names.get(val)
            if resolved is not None:
                out += encode_len_field(10, resolved)
            else:
                out += raw
        # DebugAnnotation.string_value_iid (17, varint) -> string_value (6, string)
        elif fnum == 17 and wt == WT_VARINT:
            resolved = state.debug_ann_str_values.get(val)
            if resolved is not None:
                out += encode_len_field(6, resolved)
            else:
                out += raw
        # DebugAnnotation.proto_type_name_iid (13, varint) -> proto_type_name (16, string)
        elif fnum == 13 and wt == WT_VARINT:
            resolved = state.debug_ann_vtype_names.get(val)
            if resolved is not None:
                out += encode_len_field(16, resolved)
            else:
                out += raw
        # Recurse into dict_entries (11) and array_values (12)
        elif fnum in (11, 12) and wt == WT_LEN:
            new_child = rewrite_debug_annotation(val, state)
            out += encode_len_field(fnum, new_child)
        else:
            out += raw
    return bytes(out)


def rewrite_track_event(data, state):
    """Rewrite a TrackEvent message, expanding all interned references."""
    fields = parse_fields(data)

    # Collect category_iids so we can emit them at the end.
    cat_iids = []
    needs_rewrite = False
    for fnum, wt, val, _ in fields:
        if fnum == 10 and wt == WT_VARINT:   # name_iid
            needs_rewrite = True
        elif fnum == 3 and wt == WT_VARINT:  # category_iids
            cat_iids.append(val)
            needs_rewrite = True
        elif fnum == 34 and wt == WT_VARINT: # source_location_iid
            needs_rewrite = True
        elif fnum == 4 and wt == WT_LEN:     # debug_annotations
            needs_rewrite = True

    if not needs_rewrite:
        return data

    out = bytearray()
    for fnum, wt, val, raw in fields:
        # name_iid (10) -> name (23)
        if fnum == 10 and wt == WT_VARINT:
            resolved = state.event_names.get(val)
            if resolved is not None:
                out += encode_len_field(23, resolved)
            else:
                out += raw
        # category_iids (3) -> handled below
        elif fnum == 3 and wt == WT_VARINT:
            pass
        # source_location_iid (34) -> source_location (33)
        elif fnum == 34 and wt == WT_VARINT:
            resolved = state.source_locations.get(val)
            if resolved is not None:
                out += encode_len_field(33, resolved)
            else:
                out += raw
        # debug_annotations (4) -> rewrite recursively
        elif fnum == 4 and wt == WT_LEN:
            new_da = rewrite_debug_annotation(val, state)
            out += encode_len_field(4, new_da)
        else:
            out += raw

    # Emit resolved categories
    for iid in cat_iids:
        resolved = state.event_categories.get(iid)
        if resolved is not None:
            out += encode_len_field(22, resolved)
        else:
            out += encode_varint_field(3, iid)

    return bytes(out)


def process_trace(input_path, output_path):
    with open(input_path, 'rb') as f:
        trace_data = f.read()

    state = InternState()
    trace_fields = parse_fields(trace_data)
    n_packets = 0
    n_rewritten = 0

    out = bytearray()
    for tf_num, tf_wt, tf_val, tf_raw in trace_fields:
        if tf_num != 1 or tf_wt != WT_LEN:
            out += tf_raw
            continue

        n_packets += 1
        packet_fields = parse_fields(tf_val)

        # First pass: absorb InternedData
        for pf_num, pf_wt, pf_val, _ in packet_fields:
            if pf_num == 12 and pf_wt == WT_LEN:
                state.absorb(pf_val)

        # Second pass: check if there's a track_event to rewrite
        has_te = any(pf_num == 11 and pf_wt == WT_LEN
                     for pf_num, pf_wt, _, _ in packet_fields)
        if not has_te:
            out += tf_raw
            continue

        # Rewrite the packet
        n_rewritten += 1
        new_packet = bytearray()
        for pf_num, pf_wt, pf_val, pf_raw in packet_fields:
            if pf_num == 11 and pf_wt == WT_LEN:
                new_te = rewrite_track_event(pf_val, state)
                new_packet += encode_len_field(11, new_te)
            elif pf_num == 12 and pf_wt == WT_LEN:
                # Strip expanded interned data entries, keep the rest
                stripped = state.strip_expanded(pf_val)
                if stripped:
                    new_packet += encode_len_field(12, stripped)
                # else: fully stripped, omit the field entirely
            else:
                new_packet += pf_raw
        out += encode_len_field(1, bytes(new_packet))

    with open(output_path, 'wb') as f:
        f.write(out)

    print(f'Read {n_packets} packets, rewritten {n_rewritten} track events')
    print(f'Interned: {len(state.event_names)} event names, '
          f'{len(state.event_categories)} categories, '
          f'{len(state.debug_ann_names)} debug annotation names, '
          f'{len(state.debug_ann_str_values)} debug annotation string values, '
          f'{len(state.debug_ann_vtype_names)} debug annotation type names, '
          f'{len(state.source_locations)} source locations')
    print(f'Output: {output_path}')


def main():
    if len(sys.argv) < 2:
        print(f'Usage: {sys.argv[0]} <input_trace> [output_trace]')
        sys.exit(1)
    inp = sys.argv[1]
    outp = sys.argv[2] if len(sys.argv) > 2 else inp + '.expanded'
    process_trace(inp, outp)


if __name__ == '__main__':
    main()
