"""Round-trip checks for compression_lib's raw deflate interception.

Run under the library, with and without the stream takeover:
    COMPRESSION_STREAMS=1 LD_PRELOAD=$PWD/bin/compression_lib.so python3 tests/test_deflate_stream.py
    LD_PRELOAD=$PWD/bin/compression_lib.so python3 tests/test_deflate_stream.py

Each check compresses through Python's zlib/gzip modules (the calls the
library intercepts) and decompresses the result to compare with the input.
"""

import binascii
import gzip
import os
import sys
import zlib

TEST_DIR = "test"


def sample_data():
    # Random bytes don't compress, zeros compress well: covers both paths
    return os.urandom(3 << 20) + bytes(5 << 20) + os.urandom(1000)


def check_gzip_file(data, level):
    os.makedirs(TEST_DIR, exist_ok=True)
    path = os.path.join(TEST_DIR, f"stream_level{level}.gz")
    with gzip.open(path, "wb", compresslevel=level) as f:
        # Many uneven chunks, like nibabel writing slices
        for start in range(0, len(data), 300_000):
            f.write(data[start:start + 300_000])
    with gzip.open(path, "rb") as f:
        return f.read() == data


def check_raw_stream(data):
    c = zlib.compressobj(6, zlib.DEFLATED, -zlib.MAX_WBITS)
    out = c.compress(data[:1000])
    out += c.flush(zlib.Z_SYNC_FLUSH)
    out += c.compress(data[1000:])
    out += c.flush()
    return zlib.decompress(out, -zlib.MAX_WBITS) == data


def check_empty_stream():
    c = zlib.compressobj(6, zlib.DEFLATED, -zlib.MAX_WBITS)
    out = c.compress(b"") + c.flush()
    return zlib.decompress(out, -zlib.MAX_WBITS) == b""


def check_gzip_wrapper_stream(data):
    # wbits 31 (gzip wrapper done by zlib) is never taken over
    c = zlib.compressobj(6, zlib.DEFLATED, 31)
    out = c.compress(data) + c.flush()
    return gzip.decompress(out) == data


def check_crc32(data):
    return (zlib.crc32(data) == binascii.crc32(data)
            and zlib.crc32(data[1000:], zlib.crc32(data[:1000])) == binascii.crc32(data)
            and zlib.crc32(b"") == 0)


def main():
    data = sample_data()
    checks = [
        ("gzip file level 1", lambda: check_gzip_file(data, 1)),
        ("gzip file level 6", lambda: check_gzip_file(data, 6)),
        ("gzip file level 9", lambda: check_gzip_file(data, 9)),
        ("raw stream with sync flush", lambda: check_raw_stream(data)),
        ("empty raw stream", check_empty_stream),
        ("gzip wrapper stream (wbits 31)", lambda: check_gzip_wrapper_stream(data)),
        ("crc32", lambda: check_crc32(data)),
    ]

    failed = 0
    for name, check in checks:
        ok = check()
        failed += not ok
        print(f"{'ok  ' if ok else 'FAIL'} {name}")
    print(f"{len(checks) - failed}/{len(checks)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
