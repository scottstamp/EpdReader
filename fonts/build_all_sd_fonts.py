import subprocess
import sys
import os

def run_conversion(ttf, size, output_name):
    script = os.path.join("fonts", "convert_to_bin_font.py")
    interpreter = sys.executable
    cmd = [interpreter, script, os.path.join("fonts", ttf), str(size), os.path.join("sd_fonts", "fonts", output_name)]
    print(f"Running: {' '.join(cmd)}")
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"Error: {res.stderr}")
    else:
        print(res.stdout.strip())

def main():
    # Ensure output directory exists
    os.makedirs(os.path.join("sd_fonts", "fonts"), exist_ok=True)
    
    # Fonts configuration: (source_ttf, pixel_size, output_font_name)
    font_configs = [
        # Literata / Serif
        ("Literata-Regular.ttf", 14, "Literata9.font"),
        ("Literata-Regular.ttf", 14, "Serif9.font"),
        ("Literata-Regular.ttf", 18, "Literata12.font"),
        ("Literata-Regular.ttf", 18, "Serif12.font"),
        ("Literata-Regular.ttf", 26, "Literata18.font"),
        ("Literata-Regular.ttf", 26, "Serif18.font"),
        
        # Atkinson / Sans
        ("AtkinsonHyperlegibleNext-Regular.otf", 14, "Atkinson9.font"),
        ("AtkinsonHyperlegibleNext-Regular.otf", 14, "Sans9.font"),
        ("AtkinsonHyperlegibleNext-Regular.otf", 18, "Atkinson12.font"),
        ("AtkinsonHyperlegibleNext-Regular.otf", 18, "Sans12.font"),
        ("AtkinsonHyperlegibleNext-Regular.otf", 26, "Atkinson18.font"),
        ("AtkinsonHyperlegibleNext-Regular.otf", 26, "Sans18.font"),
        
        # Bookerly
        ("Bookerly.ttf", 15, "Bookerly9.font"),
        ("Bookerly.ttf", 18, "Bookerly12.font"),
        ("Bookerly.ttf", 26, "Bookerly18.font")
    ]
    
    for ttf, size, out_name in font_configs:
        run_conversion(ttf, size, out_name)

if __name__ == '__main__':
    main()
