"""Independent small DeviceSQL USB export fixtures for native acceptance."""

import struct

PAGE_SIZE = 4096


def device_string(text, long_ascii=False):
    if text.isascii():
        contents = text.encode("ascii")
        if len(contents) <= 126 and not long_ascii:
            return bytes([(len(contents) + 1) * 2 + 1]) + contents
        return b"\x40" + struct.pack("<H", len(contents) + 4) + b"\x00" + contents
    contents = text.encode("utf-16-le")
    return b"\x90" + struct.pack("<H", len(contents) + 4) + b"\x00" + contents


def track_row(identifier, title=None, path=None):
    row = bytearray(136)
    struct.pack_into("<H", row, 0, 0x24)
    for offset, value in {8: 48000, 12: 5, 16: 4000000, 32: 7, 40: 9, 44: 5,
                          48: 320, 52: identifier, 56: 12750, 60: 8, 64: 6, 68: 5, 72: identifier}.items():
        struct.pack_into("<I", row, offset, value)
    struct.pack_into("<H", row, 80, 2026)
    struct.pack_into("<H", row, 84, 123)
    values = {10: "2026-10-07", 14: "/PIONEER/USBANLZ/test/ANLZ0000.DAT", 16: "USB yorum",
              17: title or {1: "Birinci", 2: "İkinci", 3: "Dışarı"}.get(identifier, f"Track {identifier}"),
              19: f"Track {identifier}.wav", 20: path or f"/Contents/Örnek DJ/Track {identifier}.wav"}
    for position in range(21):
        struct.pack_into("<H", row, 94 + position * 2, len(row))
        row.extend(device_string(values.get(position, "")))
    return bytes(row)


def _page(index, kind, rows, next_page, deleted=()):
    page = bytearray(PAGE_SIZE)
    struct.pack_into("<IIIIII", page, 0, 0, index, kind, next_page, 1, 0)
    page[24:27] = (len(rows) | (len(rows) - len(deleted)) << 13).to_bytes(3, "little")
    page[27] = 0x24
    used = 0
    for slot, row in enumerate(rows):
        base = PAGE_SIZE - slot // 16 * 36
        struct.pack_into("<H", page, base - 6 - slot % 16 * 2, used)
        if slot not in deleted:
            flags = struct.unpack_from("<H", page, base - 4)[0] | 1 << (slot % 16)
            struct.pack_into("<H", page, base - 4, flags)
        page[40 + used:40 + used + len(row)] = row
        used += len(row)
    index_size = (len(rows) + 15) // 16 * 36
    assert used + index_size + 40 < PAGE_SIZE
    struct.pack_into("<HH", page, 28, PAGE_SIZE - used - index_size - 40, used)
    return page


def export_pdb(track_count=3, path=None):
    artist = struct.pack("<HHIBB", 0x60, 0, 5, 3, 10) + device_string("Örnek DJ")
    album = struct.pack("<HHIIIIBB", 0x80, 0, 0, 5, 6, 0, 3, 22) + device_string("USB Album")
    tables = {0: [track_row(identifier, path=path if identifier == 1 else None) for identifier in range(1, track_count + 1)],
              1: [struct.pack("<I", 8) + device_string("House")], 2: [artist], 3: [album],
              4: [struct.pack("<I", 9) + device_string("Label")],
              5: [struct.pack("<II", 7, 7) + device_string("8A")],
              7: [struct.pack("<IIIII", 10, 0, 1, 12, 0) + device_string("USB Dışarı"),
                  struct.pack("<IIIII", 0, 0, 0, 10, 1) + device_string("USB Setleri"),
                  struct.pack("<IIIII", 10, 0, 0, 11, 0) + device_string("USB Sıra")],
              8: [struct.pack("<III", 3, 1, 11), struct.pack("<III", 1, 2, 11),
                  struct.pack("<III", 2, 1, 11), struct.pack("<III", 1, 3, 12), b"\xff" * 12]}
    pages = []
    pointers = []
    for kind, rows in tables.items():
        batches = []
        batch = []
        for row in rows:
            if sum(len(member) for member in batch) + len(row) > 3000:
                batches.append(batch)
                batch = []
            batch.append(row)
        batches.append(batch)
        first = len(pages) + 1
        index_page = bytearray(PAGE_SIZE)
        struct.pack_into("<IIII", index_page, 0, 0, first, kind, first + 1)
        index_page[24:27] = b"\xff\xff\xff"
        index_page[27] = 0x64
        pages.append(index_page)
        for position, members in enumerate(batches):
            index = len(pages) + 1
            deleted = (len(members) - 1,) if kind == 8 and position == len(batches) - 1 else ()
            pages.append(_page(index, kind, members, index + 1, deleted))
        pointers.append(struct.pack("<IIII", kind, 0, first, len(pages)))
    header = bytearray(PAGE_SIZE)
    struct.pack_into("<IIIIIII", header, 0, 0, PAGE_SIZE, len(tables), len(pages) + 1, 5, 2, 0)
    header[28:28 + 16 * len(pointers)] = b"".join(pointers)
    return bytes(header + b"".join(pages))
