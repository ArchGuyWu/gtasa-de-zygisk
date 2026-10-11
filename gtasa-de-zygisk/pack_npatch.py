#!/usr/bin/env python3
"""Pack the NPatch module apk around the already built native libraries."""

import argparse
import struct
import zipfile
from pathlib import Path

NO = 0xFFFFFFFF
TYPE_STRING = 0x03
TYPE_INT_DEC = 0x10
TYPE_INT_BOOL = 0x12
BOOL_TRUE = 0xFFFFFFFF
BOOL_FALSE = 0


def u16(value):
    return struct.pack("<H", value)


def u32(value):
    return struct.pack("<I", value & 0xFFFFFFFF)


def align4(blob):
    pad = (-len(blob)) % 4
    return blob + (b"\0" * pad)


def utf16_string(text):
    encoded = text.encode("utf-16le")
    return u16(len(text)) + encoded + u16(0)


def chunk(kind, header_size, header_rest, body):
    if 8 + len(header_rest) != header_size:
        raise SystemExit("bad chunk header")
    size = header_size + len(body)
    return u16(kind) + u16(header_size) + u32(size) + header_rest + body


def string_pool(strings):
    encoded = [utf16_string(item) for item in strings]
    offsets = []
    data = b""
    for item in encoded:
        offsets.append(len(data))
        data += item
    data = align4(data)
    header_size = 0x1C
    offset_blob = b"".join(u32(offset) for offset in offsets)
    strings_start = header_size + len(offset_blob)
    header_rest = u32(len(strings)) + u32(0) + u32(0) + u32(strings_start) + u32(0)
    return chunk(0x0001, header_size, header_rest, offset_blob + data)


def resource_map(ids):
    return chunk(0x0180, 8, b"", b"".join(u32(item) for item in ids))


def xml_node(kind, extra):
    return chunk(kind, 16, u32(1) + u32(NO), extra)


def attr(ns, name, raw, value_type, data):
    return u32(ns) + u32(name) + u32(raw) + u16(8) + bytes((0, value_type)) + u32(data)


def start_element(ns, name, attributes):
    extra = u32(ns) + u32(name)
    extra += u16(0x14) + u16(0x14) + u16(len(attributes)) + u16(0) + u16(0) + u16(0)
    extra += b"".join(attributes)
    return xml_node(0x0102, extra)


def end_element(ns, name):
    return xml_node(0x0103, u32(ns) + u32(name))


def manifest_xml(version_name, version_code):
    strings = [
        "manifest",
        "package",
        "http://schemas.android.com/apk/res/android",
        "android",
        "versionCode",
        "versionName",
        "uses-sdk",
        "minSdkVersion",
        "targetSdkVersion",
        "application",
        "label",
        "hasCode",
        "extractNativeLibs",
        "com.grok.gtasa.de.npatch",
        version_name,
        "GTASA DE",
    ]
    ids = [
        0,
        0,
        0,
        0,
        0x0101021B,
        0x0101021C,
        0,
        0x0101020C,
        0x01010270,
        0,
        0x01010001,
        0x0101000C,
        0x010104EA,
        0,
        0,
        0,
    ]
    android = 2
    prefix = 3
    parts = [
        string_pool(strings),
        resource_map(ids),
        xml_node(0x0100, u32(prefix) + u32(android)),
        start_element(
            NO,
            0,
            [
                attr(NO, 1, 13, TYPE_STRING, 13),
                attr(android, 4, NO, TYPE_INT_DEC, version_code),
                attr(android, 5, 14, TYPE_STRING, 14),
            ],
        ),
        start_element(
            NO,
            6,
            [
                attr(android, 7, NO, TYPE_INT_DEC, 30),
                attr(android, 8, NO, TYPE_INT_DEC, 34),
            ],
        ),
        end_element(NO, 6),
        start_element(
            NO,
            9,
            [
                attr(android, 10, 15, TYPE_STRING, 15),
                attr(android, 11, NO, TYPE_INT_BOOL, BOOL_FALSE),
                attr(android, 12, NO, TYPE_INT_BOOL, BOOL_TRUE),
            ],
        ),
        end_element(NO, 9),
        end_element(NO, 0),
        xml_node(0x0101, u32(prefix) + u32(android)),
    ]
    body = b"".join(parts)
    xml = chunk(0x0003, 8, b"", body)
    position = 8
    while position < len(xml):
        _, header, size = struct.unpack_from("<HHI", xml, position)
        if size < header or position + size > len(xml):
            raise SystemExit("broken manifest chunk")
        position += size
    if position != len(xml):
        raise SystemExit("manifest size mismatch")
    return xml


def add_stored(archive, name, data):
    info = zipfile.ZipInfo(name)
    info.compress_type = zipfile.ZIP_STORED
    archive.writestr(info, data)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--version-name", required=True)
    parser.add_argument("--version-code", type=int, required=True)
    parser.add_argument("--lib-dir", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    lib_dir = Path(args.lib_dir)
    libraries = [
        "libgtasa_npatch.so",
        "libgtasa_cop_react.so",
        "libgtasa_fixes.so",
        "libgtasa_probe.so",
    ]
    xml = manifest_xml(args.version_name, args.version_code)
    with zipfile.ZipFile(args.out, "w") as archive:
        add_stored(archive, "AndroidManifest.xml", xml)
        meta = {
            "META-INF/xposed/module.prop": "minApiVersion=100\ntargetApiVersion=100\nstaticScope=true\n",
            "META-INF/xposed/scope.list": "com.rockstargames.gtasa.de\n",
            "META-INF/xposed/native_init.list": "libgtasa_npatch.so\n",
            "assets/native_init": "libgtasa_npatch.so\n",
        }
        for name, text in meta.items():
            add_stored(archive, name, text.encode())
        for name in libraries:
            payload = (lib_dir / name).read_bytes()
            add_stored(archive, f"lib/arm64-v8a/{name}", payload)


if __name__ == "__main__":
    main()
