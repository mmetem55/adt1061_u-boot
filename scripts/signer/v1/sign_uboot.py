#!/usr/bin/env python3

import importlib.util
import struct
import sys
from pathlib import Path

# The sibling template module is named "adt1061-uboot.py" (hyphen, to
# match this partition's naming convention), which is not a valid
# Python identifier and can't be reached with a normal "import"/"from
# ... import" statement (that always threw SyntaxError - this script
# could never actually run as shipped). Load it by file path instead.
_template_path = Path(__file__).resolve().parent / "adt1061-uboot.py"
_spec = importlib.util.spec_from_file_location("adt1061_uboot_template", _template_path)
_template = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_template)

HEADER = _template.HEADER
TEE_BLOCK = _template.TEE_BLOCK
SIGNATURE = _template.SIGNATURE
HEADER_SIZE = _template.HEADER_SIZE
TOTAL_SIZE_FIELD_OFFSET = _template.TOTAL_SIZE_FIELD_OFFSET
UBOOT_SIZE_FIELD_OFFSET = _template.UBOOT_SIZE_FIELD_OFFSET
TABLE_SIZE = _template.TABLE_SIZE
TEE_PAD = _template.TEE_PAD
MAGIC = _template.MAGIC
PAD_ALIGN = _template.PAD_ALIGN
PAD_BYTE = _template.PAD_BYTE


def build_trailer_table(uboot_size: int) -> bytes:
    table = bytearray(TABLE_SIZE)
    tee_data_len = len(TEE_BLOCK) - TEE_PAD
    entry1_offset = HEADER_SIZE + uboot_size + TEE_PAD
    struct.pack_into("<I", table, 16, tee_data_len)
    struct.pack_into("<I", table, 24, entry1_offset)
    entry2_len = len(SIGNATURE)
    entry2_offset = HEADER_SIZE + uboot_size + len(TEE_BLOCK) + TABLE_SIZE
    struct.pack_into("<I", table, 32, entry2_len)
    struct.pack_into("<I", table, 40, entry2_offset)

    return bytes(table)


def pad_to_alignment(data: bytes, align: int, pad_byte: int) -> bytes:
    remainder = len(data) % align
    if remainder == 0:
        return data
    pad_len = align - remainder
    return data + bytes([pad_byte]) * pad_len


def build(new_uboot_path: str, output_path: str) -> None:
    src = Path(new_uboot_path)
    if not src.is_file():
        raise FileNotFoundError(f"Input file not found: {new_uboot_path}")

    uboot_payload = src.read_bytes()
    uboot_size = len(uboot_payload)

    if uboot_size == 0:
        raise ValueError("The given u-boot file is empty. (0 byte).")

    total_payload_size = uboot_size + len(TEE_BLOCK)
    if total_payload_size > 0xFFFFFFFF:
        raise ValueError("The payload size does not fit into the 4-byte field. (>4GB).")

    header = bytearray(HEADER)
    assert header[0:4] == MAGIC, "The template header magic is broken!"
    struct.pack_into("<I", header, TOTAL_SIZE_FIELD_OFFSET, total_payload_size)
    struct.pack_into("<I", header, UBOOT_SIZE_FIELD_OFFSET, uboot_size)

    table = build_trailer_table(uboot_size)

    out_data = bytes(header) + uboot_payload + TEE_BLOCK + table + SIGNATURE
    unpadded_size = len(out_data)

    out_data = pad_to_alignment(out_data, PAD_ALIGN, PAD_BYTE)

    out = Path(output_path)
    out.write_bytes(out_data)

    entry1_off = HEADER_SIZE + uboot_size + TEE_PAD
    entry2_off = HEADER_SIZE + uboot_size + len(TEE_BLOCK) + TABLE_SIZE
    pad_added = len(out_data) - unpadded_size

    print("== build_uboot.py completed ==")
    print(f"  input file              : {src}")
    print(f"  uboot size              : {uboot_size} byte  -> size@0x{UBOOT_SIZE_FIELD_OFFSET:02X}")
    print(f"  TEE_CONFIG_HEADER block : {len(TEE_BLOCK)} byte")
    print(f"  total payload           : {total_payload_size} byte -> size@0x{TOTAL_SIZE_FIELD_OFFSET:02X}")
    print(f"  table entry1            : length={len(TEE_BLOCK)-TEE_PAD}  offset={entry1_off}  (RECALCULATED)")
    print(f"  table entry2            : length={len(SIGNATURE)}  offset={entry2_off}  (RECALCULATED)")
    print(f"  signature               : {len(SIGNATURE)} byte")
    print(f"  size before alignment   : {unpadded_size} byte (mod {PAD_ALIGN} = {unpadded_size % PAD_ALIGN})")
    print(f"  added 0xFF padding      : {pad_added} byte")
    print(f"  total output size       : {len(out_data)} byte (mod {PAD_ALIGN} = {len(out_data) % PAD_ALIGN})")
    print(f"  output file             : {out}")
    print()

def main() -> None:
    if len(sys.argv) != 3:
        print("Usage: python3 build_uboot.py <newly_compiled_uboot.bin> <output_file.bin>")
        sys.exit(1)

    build(sys.argv[1], sys.argv[2])


if __name__ == "__main__":
    main()
