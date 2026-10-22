import struct
import sys
import os
from PIL import Image, ImageFont

# .venv310/Scripts/python.exe fonts/convert_to_bin_font.py <your_font.ttf> <size_px> <output_name.font>

# Binary file structure:
# Header:
# char magic[4] = "GFT1"
# uint16_t first = 32
# uint16_t last = 131
# uint8_t yAdvance
# uint8_t reserved = 0
# Metadata:
# uint32_t bitmapSize
# uint32_t numGlyphs = last - first + 1
# Data:
# uint8_t bitmaps[bitmapSize]
# PackedGlyph glyphs[numGlyphs]
#   uint16_t bitmapOffset
#   uint8_t width
#   uint8_t height
#   uint8_t xAdvance
#   int8_t xOffset
#   int8_t yOffset

def convert_font_to_bin(font_path, pixel_size, out_bin_path):
    try:
        font1x = ImageFont.truetype(font_path, pixel_size)
    except Exception as e:
        print(f"Error loading font {font_path}: {e}")
        return False

    # Get line spacing (yAdvance)
    ascent, descent = font1x.getmetrics()
    yAdvance = ascent + descent
    print(f"Font: {os.path.basename(font_path)}, Size: {pixel_size}px, Ascent: {ascent}, Descent: {descent}, yAdvance: {yAdvance}")

    # Pre-pass to find base_bottom using 'v', 'w', 'x', 'z'
    flat_bottoms = []
    for char in ['v', 'w', 'x', 'z']:
        try:
            mask, offset = font1x.getmask2(char, mode="1", anchor="ls")
            w, h = mask.size
            last_row = -1
            for y in range(h):
                for x in range(w):
                    if mask.getpixel((x, y)) > 0:
                        last_row = y
                        break
            if last_row != -1:
                flat_bottoms.append(last_row + offset[1])
        except Exception:
            pass
    base_bottom = max(set(flat_bottoms), key=flat_bottoms.count) if flat_bottoms else -1

    baseline_chars = (
        [chr(c) for c in range(65, 91) if chr(c) not in ['Q', 'J']] +
        [chr(c) for c in range(97, 123) if chr(c) not in ['g', 'j', 'p', 'q', 'y']] +
        [chr(c) for c in range(48, 58)]
    )

    bitmaps = []
    glyphs = []
    current_offset = 0
    
    chars_to_convert = [(code, chr(code)) for code in range(32, 127)]
    chars_to_convert.append((127, '—')) # em dash
    chars_to_convert.append((128, '“')) # left double quote
    chars_to_convert.append((129, '”')) # right double quote
    chars_to_convert.append((130, '’')) # right single quote / apostrophe
    chars_to_convert.append((131, '…')) # ellipsis

    first = chars_to_convert[0][0]
    last = chars_to_convert[-1][0]
    num_glyphs = last - first + 1

    for code, char in chars_to_convert:
        mask, offset = font1x.getmask2(char, mode="L", anchor="ls")
        w, h = mask.size
        
        if w == 0 or h == 0:
            width = 0
            height = 0
            xOffset = 0
            yOffset = 0
            xAdvance = int(round(font1x.getlength(char)))
            pixels = []
        else:
            width = w
            height = h
            xOffset = offset[0]
            yOffset = offset[1]
            xAdvance = int(round(font1x.getlength(char)))
            
            # Quantize 0-255 to 0-3 (coverage level: 0 = transparent, 1 = 33%, 2 = 66%, 3 = 100%)
            pixels = []
            for y in range(height):
                for x in range(width):
                    val = mask.getpixel((x, y))
                    if val < 50:
                        pixels.append(0)
                    elif val < 130:
                        pixels.append(1)
                    elif val < 210:
                        pixels.append(2)
                    else:
                        pixels.append(3)
            
            # Baseline alignment correction
            last_row = -1
            for y in range(height):
                for x in range(width):
                    if pixels[y * width + x] > 0:
                        last_row = y
                        break
            
            if last_row != -1 and char in baseline_chars:
                pixel_bottom = last_row + yOffset
                if pixel_bottom < base_bottom:
                    shift = base_bottom - pixel_bottom
                    yOffset += shift
            
        # Pack bits (2 bits per pixel)
        glyph_bytes = []
        current_byte = 0
        bit_count = 0
        for pixel in pixels:
            current_byte = (current_byte << 2) | pixel
            bit_count += 2
            if bit_count == 8:
                glyph_bytes.append(current_byte)
                current_byte = 0
                bit_count = 0
        if bit_count > 0:
            current_byte = current_byte << (8 - bit_count)
            glyph_bytes.append(current_byte)
            
        glyphs.append({
            'offset': current_offset,
            'width': width,
            'height': height,
            'xAdvance': xAdvance,
            'xOffset': xOffset,
            'yOffset': yOffset
        })
        
        bitmaps.extend(glyph_bytes)
        current_offset += len(glyph_bytes)

    # Ensure output dir exists
    out_dir = os.path.dirname(out_bin_path)
    if out_dir and not os.path.exists(out_dir):
        os.makedirs(out_dir)

    # Write binary file
    with open(out_bin_path, "wb") as f:
        # Header (10 bytes): magic (4), first (2), last (2), yAdvance (1), reserved (1)
        f.write(struct.pack("<4sHHBB", b"GFT2", first, last, yAdvance, 0))
        # Metadata (8 bytes): bitmapSize (4), numGlyphs (4)
        f.write(struct.pack("<II", len(bitmaps), num_glyphs))
        # Bitmaps
        f.write(bytes(bitmaps))
        # Glyphs
        for g in glyphs:
            # GFXglyph struct format in file (7 bytes):
            # uint16_t bitmapOffset
            # uint8_t width
            # uint8_t height
            # uint8_t xAdvance
            # int8_t xOffset
            # int8_t yOffset
            f.write(struct.pack("<HBBBbb", g['offset'], g['width'], g['height'], g['xAdvance'], g['xOffset'], g['yOffset']))
            
    print(f"Successfully generated {out_bin_path} (bitmap: {len(bitmaps)} bytes, glyphs: {num_glyphs * 7} bytes)")
    return True

if __name__ == "__main__":
    if len(sys.argv) < 4:
        print("Usage: python convert_to_bin_font.py <font_path> <pixel_size> <output_font_path>")
        print("Example: python convert_to_bin_font.py Bookerly.ttf 18 Bookerly12.font")
        sys.exit(1)

    font_path = sys.argv[1]
    pixel_size = int(sys.argv[2])
    output_path = sys.argv[3]

    convert_font_to_bin(font_path, pixel_size, output_path)
