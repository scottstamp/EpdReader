import os
from PIL import Image, ImageDraw, ImageFont

def convert_font(font_path, pixel_size, font_name_c):
    try:
        font = ImageFont.truetype(font_path, pixel_size)
    except Exception as e:
        print(f"Error loading font {font_path}: {e}")
        return None

    # Get line spacing (yAdvance)
    ascent, descent = font.getmetrics()
    yAdvance = ascent + descent
    print(f"Font: {font_name_c}, Pixel Size: {pixel_size}, ascent: {ascent}, descent: {descent}, yAdvance: {yAdvance}")

    # Pre-pass to find base_bottom using 'v', 'w', 'x', 'z'
    flat_bottoms = []
    for char in ['v', 'w', 'x', 'z']:
        try:
            mask, offset = font.getmask2(char, mode="1", anchor="ls")
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

    # Baseline-bound characters (must sit on the baseline)
    baseline_chars = (
        [chr(c) for c in range(65, 91) if chr(c) not in ['Q', 'J']] +
        [chr(c) for c in range(97, 123) if chr(c) not in ['g', 'j', 'p', 'q', 'y']] +
        [chr(c) for c in range(48, 58)]
    )

    bitmaps = []
    glyphs = []
    current_offset = 0
    
    for code in range(32, 127):
        char = chr(code)
        mask, offset = font.getmask2(char, mode="1", anchor="ls")
        width, height = mask.size
        
        if width == 0 or height == 0:
            width = 0
            height = 0
            xOffset = 0
            yOffset = 0
            xAdvance = int(round(font.getlength(char)))
            pixels = []
        else:
            xOffset, yOffset = offset
            xAdvance = int(round(font.getlength(char)))
            
            # Find actual bottom of non-zero pixels
            last_row = -1
            for y in range(height):
                for x in range(width):
                    if mask.getpixel((x, y)) > 0:
                        last_row = y
                        break
            
            if last_row != -1 and char in baseline_chars:
                pixel_bottom = last_row + yOffset
                if pixel_bottom < base_bottom:
                    shift = base_bottom - pixel_bottom
                    yOffset += shift
                    
            pixels = []
            for y in range(height):
                for x in range(width):
                    pixels.append(mask.getpixel((x, y)))
            
        # Pack bits
        glyph_bytes = []
        current_byte = 0
        bit_count = 0
        for pixel in pixels:
            current_byte = (current_byte << 1) | (1 if pixel > 0 else 0)
            bit_count += 1
            if bit_count == 8:
                glyph_bytes.append(current_byte)
                current_byte = 0
                bit_count = 0
        if bit_count > 0:
            current_byte = current_byte << (8 - bit_count)
            glyph_bytes.append(current_byte)
            
        glyphs.append({
            'char': char,
            'code': code,
            'offset': current_offset,
            'width': width,
            'height': height,
            'xAdvance': xAdvance,
            'xOffset': xOffset,
            'yOffset': yOffset
        })
        
        bitmaps.extend(glyph_bytes)
        current_offset += len(glyph_bytes)

    # Format output code
    out = []
    out.append("#pragma once")
    out.append("#include <Adafruit_GFX.h>")
    out.append("")
    out.append(f"const uint8_t {font_name_c}Bitmaps[] PROGMEM = {{")
    
    bitmap_lines = []
    for i in range(0, len(bitmaps), 12):
        chunk = bitmaps[i:i+12]
        hex_str = ", ".join(f"0x{b:02X}" for b in chunk)
        bitmap_lines.append("    " + hex_str)
    out.append(",\n".join(bitmap_lines))
    out.append("};")
    out.append("")
    
    out.append(f"const GFXglyph {font_name_c}Glyphs[] PROGMEM = {{")
    glyph_lines = []
    for g in glyphs:
        comment = f"// 0x{g['code']:02X} '{g['char']}'" if g['code'] != 0x5C else f"// 0x{g['code']:02X} '\\'"
        glyph_lines.append(f"    {{{g['offset']}, {g['width']}, {g['height']}, {g['xAdvance']}, {g['xOffset']}, {g['yOffset']}}}, {comment}")
    out.append("\n".join(glyph_lines))
    out.append("};")
    out.append("")
    
    out.append(f"const GFXfont {font_name_c} PROGMEM = {{")
    out.append(f"    (uint8_t *){font_name_c}Bitmaps,")
    out.append(f"    (GFXglyph *){font_name_c}Glyphs,")
    out.append("    0x20, 0x7E,")
    out.append(f"    {yAdvance}")
    out.append("};")
    
    return "\n".join(out)

def main():
    fonts_dir = "fonts"
    src_dir = "src"
    
    # Fonts configuration: (source_ttf, pixel_size, output_c_name, output_h_filename)
    font_configs = [
        # Bookerly Regular versions
        ("Bookerly.ttf", 14, "Bookerly9pt7b", "Bookerly9pt7b.h"),
        ("Bookerly.ttf", 18, "Bookerly12pt7b", "Bookerly12pt7b.h"),
        ("Bookerly.ttf", 26, "Bookerly18pt7b", "Bookerly18pt7b.h"),
        # Bookerly Bold version for menus
        ("Bookerly Bold.ttf", 14, "Bookerly_Bold9pt7b", "Bookerly_Bold9pt7b.h"),
        # Literata Regular versions
        ("Literata-Regular.ttf", 14, "Literata9pt7b", "Literata9pt7b.h"),
        ("Literata-Regular.ttf", 18, "Literata12pt7b", "Literata12pt7b.h"),
        ("Literata-Regular.ttf", 26, "Literata18pt7b", "Literata18pt7b.h"),
        # Amazon Ember Medium version for menus
        ("Amazon-Ember-Medium.ttf", 14, "AmazonEmber_Medium9pt7b", "AmazonEmber_Medium9pt7b.h")
    ]
    
    for ttf_file, size, name_c, h_file in font_configs:
        ttf_path = os.path.join(fonts_dir, ttf_file)
        h_path = os.path.join(src_dir, h_file)
        
        print(f"Converting {ttf_path} ({size}px) to {h_path}...")
        code = convert_font(ttf_path, size, name_c)
        if code:
            with open(h_path, "w", encoding="utf-8") as f:
                f.write(code)
            print("Done.")

if __name__ == "__main__":
    main()
