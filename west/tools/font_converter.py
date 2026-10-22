#!/usr/bin/env python3
"""
EPUB Reader Binary Font Converter Tool
Converts TTF/OTF fonts to binary SD font files (.fnt) for e-Paper Reader.

Usage:
  python font_converter.py --name Bookerly --size 14 \
      --regular Bookerly-Regular.ttf \
      --bold Bookerly-Bold.ttf \
      --italic Bookerly-Italic.ttf \
      --bolditalic Bookerly-BoldItalic.ttf \
      --output Bookerly.fnt
"""

import sys
import os
import argparse
import struct
from PIL import Image, ImageFont, ImageDraw

MAGIC = b"SDFN"
VERSION = 1

# Comprehensive Unicode Ranges for Readers
UNICODE_RANGES = [
    (32, 126),       # ASCII Printable
    (160, 255),      # Latin-1 Supplement
    (0x0100, 0x017F), # Latin Extended-A
    (0x2010, 0x203A), # Punctuation & Smart Quotes (dash, quotes, ellipsis, single quotes)
    (0x20A0, 0x20BF), # Currency Symbols (€, £, ¥, ¢)
]

STYLE_REGULAR = 0
STYLE_BOLD = 1
STYLE_ITALIC = 2
STYLE_BOLDITALIC = 3

def get_codepoint_list():
    cps = []
    for start, end in UNICODE_RANGES:
        cps.extend(range(start, end + 1))
    return sorted(list(set(cps)))

def render_glyph(font, cp):
    ch = chr(cp)
    
    # Get bounding box
    bbox = font.getbbox(ch)
    if not bbox:
        return None
    
    left, top, right, bottom = bbox
    width = max(1, right - left)
    height = max(1, bottom - top)

    # Render 1-bit grayscale image
    img = Image.new('L', (width, height), 0)
    draw = ImageDraw.Draw(img)
    draw.text((-left, -top), ch, fill=255, font=font)

    # Get xAdvance
    try:
        adv = int(font.getlength(ch))
    except AttributeError:
        adv = width + 1

    # Pack bitmap to 1-bit per pixel
    pixels = img.load()
    bit_data = bytearray()
    curr_byte = 0
    bit_idx = 0

    for y in range(height):
        for x in range(width):
            if pixels[x, y] > 128:
                curr_byte |= (0x80 >> bit_idx)
            bit_idx += 1
            if bit_idx == 8:
                bit_data.append(curr_byte)
                curr_byte = 0
                bit_idx = 0

    if bit_idx > 0:
        bit_data.append(curr_byte)

    return {
        'codepoint': cp,
        'width': width,
        'height': height,
        'xOffset': left,
        'yOffset': top,
        'xAdvance': adv,
        'bitmap': bytes(bit_data)
    }

def convert_fonts(font_paths, size, out_path, name):
    codepoints = get_codepoint_list()
    styles = [
        ('regular', STYLE_REGULAR),
        ('bold', STYLE_BOLD),
        ('italic', STYLE_ITALIC),
        ('bolditalic', STYLE_BOLDITALIC)
    ]

    loaded_fonts = {}
    for style_name, style_id in styles:
        path = font_paths.get(style_name)
        if path and os.path.exists(path):
            try:
                loaded_fonts[style_id] = ImageFont.truetype(path, size)
                print(f"  Loaded {style_name}: {path}")
            except Exception as e:
                print(f"  Warning: Could not load {style_name} ({path}): {e}")
        elif font_paths.get('regular'):
            # Fallback to regular font if specific style missing
            loaded_fonts[style_id] = ImageFont.truetype(font_paths['regular'], size)

    if not loaded_fonts:
        print("Error: No valid fonts loaded.")
        sys.exit(1)

    all_records = []
    
    for style_id, font_obj in loaded_fonts.items():
        for cp in codepoints:
            try:
                rec = render_glyph(font_obj, cp)
                if rec:
                    rec['style_id'] = style_id
                    all_records.append(rec)
            except Exception:
                continue

    print(f"Rendered {len(all_records)} total glyph records across {len(loaded_fonts)} styles.")

    # Calculate offsets & construct binary file
    style_flags = sum(1 << style_id for style_id in loaded_fonts.keys())
    header_size = 32
    index_entry_size = 8
    index_table_size = len(all_records) * index_entry_size
    first_record_offset = header_size + index_table_size

    # Build Header (32 bytes)
    # MAGIC(4s) VERSION(H) style_flags(H) font_size(H) ascent(h) descent(h) line_height(h) glyph_count(H) reserved(14s)
    ascent, descent = 14, 4
    header = struct.pack(
        "<4sHHHhhhH14s",
        MAGIC,
        VERSION,
        style_flags,
        size,
        ascent,
        descent,
        size + 4,
        len(all_records),
        b"\x00" * 14
    )

    # Build Records & Index Table
    index_bytes = bytearray()
    record_bytes = bytearray()
    curr_offset = first_record_offset

    for rec in all_records:
        bm = rec['bitmap']
        # Pack index entry: codepoint (uint32), offset (uint32)
        # Note: top 2 bits of codepoint encode style_id (0..3)
        cp_style_encoded = (rec['codepoint'] & 0x3FFFFFFF) | ((rec['style_id'] & 0x03) << 30)
        index_bytes.extend(struct.pack("<II", cp_style_encoded, curr_offset))

        # Pack glyph record: style_id(B) width(B) height(B) xOffset(b) yOffset(b) xAdvance(B) bitmap_len(H)
        rec_hdr = struct.pack(
            "<BBBbbBH",
            rec['style_id'],
            rec['width'],
            rec['height'],
            rec['xOffset'],
            rec['yOffset'],
            rec['xAdvance'],
            len(bm)
        )
        record_bytes.extend(rec_hdr)
        record_bytes.extend(bm)
        curr_offset += len(rec_hdr) + len(bm)

    with open(out_path, "wb") as f:
        f.write(header)
        f.write(index_bytes)
        f.write(record_bytes)

    file_sz = len(header) + len(index_bytes) + len(record_bytes)
    print(f"Successfully generated SD font file: '{out_path}' ({file_sz:,} bytes, {len(all_records)} glyphs)")

def main():
    parser = argparse.ArgumentParser(description="Convert TTF/OTF fonts to EPUB Reader SD binary font (.fnt)")
    parser.add_argument("--name", default="ReaderFont", help="Font family name")
    parser.add_argument("--size", type=int, default=14, help="Font point size (e.g. 14)")
    parser.add_argument("--regular", required=True, help="Path to Regular font TTF/OTF")
    parser.add_argument("--bold", help="Path to Bold font TTF/OTF")
    parser.add_argument("--italic", help="Path to Italic font TTF/OTF")
    parser.add_argument("--bolditalic", help="Path to BoldItalic font TTF/OTF")
    parser.add_argument("--output", required=True, help="Output .fnt path")

    args = parser.parse_args()
    font_paths = {
        'regular': args.regular,
        'bold': args.bold,
        'italic': args.italic,
        'bolditalic': args.bolditalic
    }

    convert_fonts(font_paths, args.size, args.output, args.name)

if __name__ == "__main__":
    main()
