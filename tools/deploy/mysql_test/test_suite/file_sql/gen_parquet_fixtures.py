#!/usr/bin/env python3
# Generate the parquet fixtures of the file_sql suite (data_ext/*.parquet.b64).
#
# The writer is self-contained (thrift compact protocol, RLE / bit packed hybrid, snappy
# and gzip pages); zstd pages are compressed by the `zstd` command line tool. Output is
# deterministic, so the fixtures can be regenerated and compared:
#
#   python3 gen_parquet_fixtures.py data_ext
#
# Files:
#   types.parquet     3 row groups, v1 pages, SNAPPY, every supported physical / logical
#                     type, nullable columns, dictionary strings, two pages in one chunk
#   v2_gzip.parquet   2 row groups, v2 pages, GZIP, RLE booleans, RLE_DICTIONARY strings
#   v2_zstd.parquet   1 row group, v2 pages, ZSTD
#   plain_dict.parquet  UNCOMPRESSED, PLAIN_DICTIONARY pages, a one entry dictionary
#   wide.parquet      3000 rows in 3 row groups of 4 pages each, SNAPPY
#   nested.parquet    a nested group column (not supported by read_parquet)
#   truncated.parquet a valid file cut in the middle (corrupt)

import base64
import os
import struct
import subprocess
import sys
import zlib

# --------------------------------------------------------------------------------------
# thrift compact protocol
# --------------------------------------------------------------------------------------
CT_TRUE, CT_FALSE, CT_BYTE, CT_I16, CT_I32, CT_I64, CT_DOUBLE, CT_BINARY, CT_LIST, \
    CT_SET, CT_MAP, CT_STRUCT = range(1, 13)


def varint(v):
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def zigzag(v):
    return (v << 1) ^ (v >> 63)


class Struct:
    """Fields: list of (id, kind, value); kind in i32/i64/bin/bool/byte/struct/list_*."""

    def __init__(self, *fields):
        self.fields = [f for f in fields if f[2] is not None]

    def encode(self):
        out = bytearray()
        last = 0
        for fid, kind, value in self.fields:
            ctype = {
                'i32': CT_I32, 'i64': CT_I64, 'bin': CT_BINARY, 'byte': CT_BYTE,
                'struct': CT_STRUCT, 'list_struct': CT_LIST, 'list_i32': CT_LIST,
                'list_bin': CT_LIST,
            }.get(kind)
            if kind == 'bool':
                ctype = CT_TRUE if value else CT_FALSE
            delta = fid - last
            if 0 < delta <= 15:
                out.append((delta << 4) | ctype)
            else:
                out.append(ctype)
                out += varint(zigzag(fid))
            last = fid
            if kind in ('i32', 'i64'):
                out += varint(zigzag(value))
            elif kind == 'byte':
                out.append(value & 0xFF)
            elif kind == 'bin':
                data = value.encode() if isinstance(value, str) else value
                out += varint(len(data)) + data
            elif kind == 'struct':
                out += value.encode()
            elif kind.startswith('list_'):
                etype = {'list_struct': CT_STRUCT, 'list_i32': CT_I32,
                         'list_bin': CT_BINARY}[kind]
                if len(value) < 15:
                    out.append((len(value) << 4) | etype)
                else:
                    out.append(0xF0 | etype)
                    out += varint(len(value))
                for e in value:
                    if kind == 'list_struct':
                        out += e.encode()
                    elif kind == 'list_i32':
                        out += varint(zigzag(e))
                    else:
                        data = e.encode() if isinstance(e, str) else e
                        out += varint(len(data)) + data
        out.append(0)
        return bytes(out)


# --------------------------------------------------------------------------------------
# parquet constants
# --------------------------------------------------------------------------------------
BOOLEAN, INT32, INT64, INT96, FLOAT, DOUBLE, BYTE_ARRAY, FIXED_LEN_BYTE_ARRAY = range(8)
REQUIRED, OPTIONAL, REPEATED = range(3)
UTF8, DECIMAL, DATE, TIMESTAMP_MILLIS, TIMESTAMP_MICROS, UINT_32, INT_16 = 0, 5, 6, 9, 10, 13, 16
PLAIN, PLAIN_DICTIONARY, RLE, RLE_DICTIONARY = 0, 2, 3, 8
UNCOMPRESSED, SNAPPY, GZIP, ZSTD = 0, 1, 2, 6
DATA_PAGE, DICTIONARY_PAGE, DATA_PAGE_V2 = 0, 2, 3


# --------------------------------------------------------------------------------------
# compression
# --------------------------------------------------------------------------------------
def snappy_compress(data):
    out = bytearray(varint(len(data)))

    def literal(lo, hi):
        while lo < hi:
            n = min(hi - lo, 65536)
            l = n - 1
            if l < 60:
                out.append(l << 2)
            elif l < 256:
                out.extend(bytes([60 << 2, l]))
            else:
                out.extend(bytes([61 << 2]) + struct.pack('<H', l))
            out.extend(data[lo:lo + n])
            lo += n

    def copy(offset, length):
        while length > 0:
            if 4 <= length <= 11 and offset < 2048:
                out.extend(bytes([((offset >> 8) << 5) | ((length - 4) << 2) | 1, offset & 0xFF]))
                return
            n = min(length, 64)
            out.extend(bytes([((n - 1) << 2) | 2]) + struct.pack('<H', offset))
            length -= n

    table = {}
    i = 0
    pending = 0
    while i + 4 <= len(data):
        key = data[i:i + 4]
        cand = table.get(key)
        table[key] = i
        if cand is not None and i - cand < 65536:
            n = 4
            while i + n < len(data) and data[cand + n] == data[i + n]:
                n += 1
            literal(pending, i)
            copy(i - cand, n)
            i += n
            pending = i
        else:
            i += 1
    literal(pending, len(data))
    return bytes(out)


def compress(codec, data):
    if codec == UNCOMPRESSED:
        return data
    if codec == SNAPPY:
        return snappy_compress(data)
    if codec == GZIP:
        c = zlib.compressobj(6, zlib.DEFLATED, 31)
        return c.compress(data) + c.flush()
    if codec == ZSTD:
        return subprocess.run(['zstd', '-q', '-c', '-19'], input=data, check=True,
                              stdout=subprocess.PIPE).stdout
    raise ValueError(codec)


# --------------------------------------------------------------------------------------
# encodings
# --------------------------------------------------------------------------------------
def bit_width(max_value):
    return max_value.bit_length()


def hybrid(values, width):
    """RLE / bit packed hybrid: runs of 8+ equal values are RLE, the rest bit packed."""
    out = bytearray()
    nbytes = (width + 7) // 8
    i = 0
    while i < len(values):
        j = i
        while j < len(values) and values[j] == values[i]:
            j += 1
        if j - i >= 8:
            out += varint((j - i) << 1) + values[i].to_bytes(nbytes, 'little')
            i = j
            continue
        # one bit packed group of 8 values, the last group is padded with zeros
        group = values[i:i + 8]
        group = group + [0] * (8 - len(group))
        out += varint((1 << 1) | 1)
        acc = 0
        for k, v in enumerate(group):
            acc |= v << (k * width)
        out += acc.to_bytes(width, 'little')
        i += 8
    return bytes(out)


def plain(ptype, values, type_length=0):
    out = bytearray()
    if ptype == BOOLEAN:
        acc = 0
        for k, v in enumerate(values):
            acc |= (1 if v else 0) << (k % 8)
            if k % 8 == 7:
                out.append(acc)
                acc = 0
        if len(values) % 8:
            out.append(acc)
        return bytes(out)
    for v in values:
        if ptype == INT32:
            out += struct.pack('<i', v)
        elif ptype == INT64:
            out += struct.pack('<q', v)
        elif ptype == INT96:
            out += v
        elif ptype == FLOAT:
            out += struct.pack('<f', v)
        elif ptype == DOUBLE:
            out += struct.pack('<d', v)
        elif ptype == BYTE_ARRAY:
            data = v.encode() if isinstance(v, str) else v
            out += struct.pack('<I', len(data)) + data
        elif ptype == FIXED_LEN_BYTE_ARRAY:
            assert len(v) == type_length
            out += v
    return bytes(out)


# --------------------------------------------------------------------------------------
# file writer
# --------------------------------------------------------------------------------------
class Column:
    def __init__(self, name, ptype, values, repetition=OPTIONAL, converted=None,
                 logical=None, scale=None, precision=None, type_length=None,
                 encoding=PLAIN, pages=1):
        self.name = name
        self.ptype = ptype
        self.values = values
        self.repetition = repetition
        self.converted = converted
        self.logical = logical
        self.scale = scale
        self.precision = precision
        self.type_length = type_length
        self.encoding = encoding    # PLAIN, RLE (booleans), PLAIN_DICTIONARY, RLE_DICTIONARY
        self.pages = pages

    def schema(self):
        return Struct((1, 'i32', self.ptype), (2, 'i32', self.type_length),
                      (3, 'i32', self.repetition), (4, 'bin', self.name),
                      (6, 'i32', self.converted), (7, 'i32', self.scale),
                      (8, 'i32', self.precision), (10, 'struct', self.logical))


def write_chunk(out, col, values, codec, page_version):
    start = len(out)
    nullable = col.repetition == OPTIONAL
    dict_offset = None
    encodings = set()
    uncompressed_total = 0
    dictionary = None
    if col.encoding in (PLAIN_DICTIONARY, RLE_DICTIONARY):
        dictionary = []
        for v in values:
            if v is not None and v not in dictionary:
                dictionary.append(v)
        raw = plain(col.ptype, dictionary, col.type_length)
        body = compress(codec, raw)
        dict_encoding = PLAIN_DICTIONARY if col.encoding == PLAIN_DICTIONARY else PLAIN
        header = Struct((1, 'i32', DICTIONARY_PAGE), (2, 'i32', len(raw)),
                        (3, 'i32', len(body)),
                        (7, 'struct', Struct((1, 'i32', len(dictionary)),
                                             (2, 'i32', dict_encoding)))).encode()
        dict_offset = len(out)
        out += header + body
        uncompressed_total += len(header) + len(raw)
        encodings.add(dict_encoding)
    data_offset = len(out)
    per_page = (len(values) + col.pages - 1) // col.pages
    for p in range(0, len(values), per_page):
        page = values[p:p + per_page]
        defs = [0 if v is None else 1 for v in page]
        present = [v for v in page if v is not None]
        if dictionary is not None:
            idx = [dictionary.index(v) for v in present]
            width = bit_width(len(dictionary) - 1)
            value_bytes = bytes([width]) + hybrid(idx, width)
        elif col.encoding == RLE:
            bits = hybrid([1 if v else 0 for v in present], 1)
            value_bytes = struct.pack('<I', len(bits)) + bits
        else:
            value_bytes = plain(col.ptype, present, col.type_length)
        encodings.add(col.encoding)
        level_bytes = hybrid(defs, 1) if nullable else b''
        if page_version == 1:
            raw = (struct.pack('<I', len(level_bytes)) + level_bytes if nullable else b'') \
                + value_bytes
            body = compress(codec, raw)
            header = Struct((1, 'i32', DATA_PAGE), (2, 'i32', len(raw)), (3, 'i32', len(body)),
                            (5, 'struct', Struct((1, 'i32', len(page)), (2, 'i32', col.encoding),
                                                 (3, 'i32', RLE), (4, 'i32', RLE)))).encode()
            uncompressed_total += len(header) + len(raw)
        else:
            body_values = compress(codec, value_bytes)
            body = level_bytes + body_values
            header = Struct((1, 'i32', DATA_PAGE_V2),
                            (2, 'i32', len(level_bytes) + len(value_bytes)),
                            (3, 'i32', len(body)),
                            (8, 'struct', Struct((1, 'i32', len(page)),
                                                 (2, 'i32', len(page) - len(present)),
                                                 (3, 'i32', len(page)), (4, 'i32', col.encoding),
                                                 (5, 'i32', len(level_bytes)), (6, 'i32', 0),
                                                 (7, 'bool', codec != UNCOMPRESSED)))).encode()
            uncompressed_total += len(header) + len(level_bytes) + len(value_bytes)
        out += header + body
    encodings.add(RLE)
    meta = Struct((1, 'i32', col.ptype), (2, 'list_i32', sorted(encodings)),
                  (3, 'list_bin', [col.name]), (4, 'i32', codec), (5, 'i64', len(values)),
                  (6, 'i64', uncompressed_total), (7, 'i64', len(out) - start),
                  (9, 'i64', data_offset), (11, 'i64', dict_offset))
    return Struct((2, 'i64', start), (3, 'struct', meta)), len(out) - start


def write_file(path, columns, group_sizes, codec, page_version, extra_schema=None):
    out = bytearray(b'PAR1')
    num_rows = sum(group_sizes)
    for c in columns:
        assert len(c.values) == num_rows, c.name
    row_groups = []
    offset = 0
    for size in group_sizes:
        chunks = []
        total = 0
        for c in columns:
            chunk, n = write_chunk(out, c, c.values[offset:offset + size], codec, page_version)
            chunks.append(chunk)
            total += n
        row_groups.append(Struct((1, 'list_struct', chunks), (2, 'i64', total),
                                 (3, 'i64', size)))
        offset += size
    schema = [Struct((4, 'bin', 'schema'), (5, 'i32', len(columns) + (1 if extra_schema else 0)))]
    schema += [c.schema() for c in columns]
    if extra_schema:
        schema += extra_schema
    footer = Struct((1, 'i32', 1), (2, 'list_struct', schema), (3, 'i64', num_rows),
                    (4, 'list_struct', row_groups),
                    (6, 'bin', 'file_sql fixture writer')).encode()
    out += footer + struct.pack('<I', len(footer)) + b'PAR1'
    with open(path, 'wb') as f:
        f.write(out)
    return bytes(out)


def string_type():
    return Struct((1, 'struct', Struct()))


def timestamp_type(unit, utc=False):
    # unit: 1 millis, 2 micros, 3 nanos
    return Struct((8, 'struct', Struct((1, 'bool', utc), (2, 'struct', Struct((unit, 'struct', Struct()))))))


def int_type(width, signed):
    return Struct((10, 'struct', Struct((1, 'byte', width), (2, 'bool', signed))))


def decimal_type(scale, precision):
    return Struct((5, 'struct', Struct((1, 'i32', scale), (2, 'i32', precision))))


def date_type():
    return Struct((6, 'struct', Struct()))


def int96(days_since_epoch, nanos):
    return struct.pack('<qi', nanos, days_since_epoch + 2440588)


def fixed_decimal(unscaled, length):
    return unscaled.to_bytes(length, 'big', signed=True)


def main(out_dir):
    os.makedirs(out_dir, exist_ok=True)
    files = {}

    # types.parquet: 10 rows, row groups of 4 / 3 / 3
    n = 10
    cols = [
        Column('id', INT64, list(range(1, n + 1)), repetition=REQUIRED, pages=2),
        Column('flag', BOOLEAN, [True, False, None, True, True, False, None, False, True, True]),
        Column('small', INT32, [-32768, 0, 32767, None, 5, -5, 7, None, 100, -100],
               converted=INT_16, logical=int_type(16, True)),
        Column('u32', INT32, [-1, 0, 1, 2147483647, -2147483648, None, 3, 4, 5, 6],
               converted=UINT_32, logical=int_type(32, False)),
        Column('f', FLOAT, [1.5, -2.25, 0.0, None, 3.75, 1e10, -0.5, 8.0, None, 2.5]),
        Column('d', DOUBLE, [0.1, None, -1e300, 2.5, 3.14159, None, 0.0, -7.125, 1e-5, 42.0]),
        Column('s', BYTE_ARRAY, ['apple', 'banana', 'apple', None, 'cherry', '中文', '',
                                 'banana', None, 'apple'],
               converted=UTF8, logical=string_type(), encoding=RLE_DICTIONARY),
        Column('day', INT32, [0, 19723, -1, None, 19782, 2932896, -719162, 18000, None, 1],
               converted=DATE, logical=date_type()),
        Column('ts', INT64, [0, 1704067200123456, None, -1000000, 1709251199999999,
                             951782400000000, None, 1, 86399999999, 1700000000000000],
               logical=timestamp_type(2)),
        Column('ts_ms', INT64, [0, 1704067200123, 1, None, -86400000, 2, 3, 4, 5, 6],
               converted=TIMESTAMP_MILLIS),
        Column('price', INT64, [12345, -1, 0, None, 999999999999, 5, 6, 7, 8, 100],
               converted=DECIMAL, scale=2, precision=12, logical=decimal_type(2, 12)),
        Column('fdec', FIXED_LEN_BYTE_ARRAY,
               [fixed_decimal(v, 5) for v in [-1234, 1234, 0, 1, -1, 99999999, 7, 8, 9, 10]],
               type_length=5, converted=DECIMAL, scale=3, precision=10,
               logical=decimal_type(3, 10)),
        Column('i96', INT96, [int96(19723, 3723000000000), None, int96(0, 0), int96(-1, 1000),
                              None, int96(10, 5), int96(11, 6), int96(12, 7), int96(13, 8),
                              int96(14, 9)]),
        Column('bin', BYTE_ARRAY, [b'raw', None, b'', b'x', b'y', b'z', b'w', b'v', b'u', b't']),
    ]
    files['types.parquet'] = write_file(os.path.join(out_dir, 'types.parquet'), cols,
                                        [4, 3, 3], SNAPPY, 1)

    # v2_gzip.parquet: 12 rows, 2 row groups, data page v2
    cols = [
        Column('k', INT32, list(range(12)), repetition=REQUIRED, encoding=PLAIN, pages=2),
        Column('b', BOOLEAN, [True] * 9 + [False, None, True], encoding=RLE),
        Column('tag', BYTE_ARRAY, ['x', 'y', None, 'x', 'z', 'x', 'y', None, 'x', 'x', 'z', 'y'],
               converted=UTF8, logical=string_type(), encoding=RLE_DICTIONARY),
        Column('v', DOUBLE, [float(i) / 4 if i % 5 else None for i in range(12)]),
    ]
    files['v2_gzip.parquet'] = write_file(os.path.join(out_dir, 'v2_gzip.parquet'), cols,
                                          [7, 5], GZIP, 2)

    # v2_zstd.parquet
    cols = [
        Column('k', INT64, list(range(1, 101)), repetition=REQUIRED, pages=3),
        Column('name', BYTE_ARRAY, ['name_%d' % (i % 10) if i % 7 else None for i in range(100)],
               converted=UTF8, logical=string_type(), encoding=RLE_DICTIONARY),
        Column('b', BOOLEAN, [i % 3 == 0 for i in range(100)], encoding=RLE),
    ]
    files['v2_zstd.parquet'] = write_file(os.path.join(out_dir, 'v2_zstd.parquet'), cols,
                                          [100], ZSTD, 2)

    # plain_dict.parquet: old style dictionary pages, uncompressed
    cols = [
        Column('city', BYTE_ARRAY, ['Hangzhou', 'Beijing', 'Hangzhou', 'Shanghai', 'Beijing'],
               repetition=REQUIRED, converted=UTF8, encoding=PLAIN_DICTIONARY),
        Column('pop', INT64, [12, 21, None, 24, 21], encoding=PLAIN_DICTIONARY),
        Column('country', BYTE_ARRAY, ['CN'] * 5, repetition=REQUIRED, converted=UTF8,
               encoding=PLAIN_DICTIONARY),
    ]
    files['plain_dict.parquet'] = write_file(os.path.join(out_dir, 'plain_dict.parquet'), cols,
                                             [5], UNCOMPRESSED, 1)

    # wide.parquet: 3000 rows
    n = 3000
    cols = [
        Column('id', INT64, list(range(1, n + 1)), repetition=REQUIRED, pages=4),
        Column('grp', INT32, [i % 7 for i in range(1, n + 1)], repetition=REQUIRED, pages=4),
        Column('name', BYTE_ARRAY, ['n%03d' % (i % 100) for i in range(1, n + 1)],
               repetition=REQUIRED, converted=UTF8, logical=string_type(),
               encoding=RLE_DICTIONARY, pages=4),
        Column('val', DOUBLE, [None if i % 10 == 0 else i * 0.5 for i in range(1, n + 1)],
               pages=4),
    ]
    files['wide.parquet'] = write_file(os.path.join(out_dir, 'wide.parquet'), cols,
                                       [1000, 1000, 1000], SNAPPY, 1)

    # nested.parquet: id plus a group with one child
    cols = [Column('id', INT64, [1, 2], repetition=REQUIRED)]
    group = [Struct((3, 'i32', OPTIONAL), (4, 'bin', 'point'), (5, 'i32', 1)),
             Struct((1, 'i32', INT32), (3, 'i32', OPTIONAL), (4, 'bin', 'x'))]
    files['nested.parquet'] = write_file(os.path.join(out_dir, 'nested.parquet'), cols, [2],
                                         UNCOMPRESSED, 1, extra_schema=group)

    data = files['plain_dict.parquet']
    files['truncated.parquet'] = data[:len(data) // 2] + data[-8:]
    with open(os.path.join(out_dir, 'truncated.parquet'), 'wb') as f:
        f.write(files['truncated.parquet'])

    for name, data in files.items():
        with open(os.path.join(out_dir, name + '.b64'), 'wb') as f:
            f.write(base64.encodebytes(data))
        os.remove(os.path.join(out_dir, name))


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'data_ext')
