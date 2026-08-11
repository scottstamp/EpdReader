#!/usr/bin/env python3
import os
import sys
import re
import zipfile
import shutil
import tempfile
import subprocess
import xml.etree.ElementTree as ET
import posixpath

def get_clean_text(element):
    """Recursively extract and concatenate all text within an element, including nested spans."""
    text_parts = []
    if element.text:
        text_parts.append(element.text)
    for child in element:
        text_parts.append(get_clean_text(child))
        if child.tail:
            text_parts.append(child.tail)
    return "".join(text_parts)

def parse_chapter_xhtml(content):
    """Parse chapter XHTML content and extract paragraphs and transitions in order."""
    root = parse_xml_safely(content)
    body = root.find('.//{http://www.w3.org/1999/xhtml}body')
    if body is None:
        body = root
        
    paragraphs = []
    
    def traverse(node):
        tag = node.tag.split('}')[-1]
        node_class = node.attrib.get('class', '')
        
        # Detect scene transition markers (like hr/div transition)
        if tag in ('hr', 'div') and 'transition' in node_class:
            if not paragraphs or paragraphs[-1] != '* * *':
                paragraphs.append('* * *')
            return # Do not traverse children of transition elements
            
        if tag == 'p':
            text = get_clean_text(node)
            text = re.sub(r'[ \t\r\n\f]+', ' ', text).strip()
            if text:
                paragraphs.append(text)
            return # Do not traverse children of paragraphs
            
        for child in node:
            traverse(child)
            
    traverse(body)
    return paragraphs

def resolve_zip_path(base_zip_path, relative_path):
    """Resolve a relative URL inside the zip, stripping URL fragments."""
    relative_path = relative_path.split('#')[0]
    base_dir = posixpath.dirname(base_zip_path)
    return posixpath.normpath(posixpath.join(base_dir, relative_path))

def find_ebook_convert():
    """Locate Calibre's ebook-convert executable on PATH or in common install locations."""
    tool = shutil.which('ebook-convert')
    if tool:
        return tool
    candidates = [
        r'C:\Program Files\Calibre2\ebook-convert.exe',
        r'C:\Program Files (x86)\Calibre2\ebook-convert.exe',
        '/Applications/calibre.app/Contents/MacOS/ebook-convert',
        '/usr/bin/ebook-convert',
        '/usr/local/bin/ebook-convert',
    ]
    for c in candidates:
        if os.path.exists(c):
            return c
    return None

def convert_to_epub(input_path):
    """Convert an azw3/mobi file to a temporary EPUB using Calibre's ebook-convert.

    Returns (epub_path, temp_dir). The caller is responsible for removing temp_dir.
    """
    tool = find_ebook_convert()
    if not tool:
        raise RuntimeError(
            "Calibre's 'ebook-convert' tool was not found. Install Calibre "
            "(https://calibre-ebook.com) to convert azw3/mobi files."
        )

    temp_dir = tempfile.mkdtemp(prefix='convert_ebook_')
    epub_path = os.path.join(temp_dir, 'converted.epub')
    print(f"Converting {input_path} to EPUB via ebook-convert...")
    # Force EPUB 3 output so a navigation document (nav.xhtml) is generated,
    # which the EPUB parser below relies on for the table of contents.
    cmd = [tool, input_path, epub_path, '--epub-version', '3']
    try:
        subprocess.run(cmd, check=True, capture_output=True, text=True)
    except subprocess.CalledProcessError as e:
        shutil.rmtree(temp_dir, ignore_errors=True)
        raise RuntimeError(f"ebook-convert failed:\n{e.stderr or e.stdout}")
    return epub_path, temp_dir

def collect_li_a(li):
    """Return the first <a> child of an <li>, or None."""
    for child in li:
        if child.tag.split('}')[-1] == 'a':
            return child
    return None

def collect_li_ol(li):
    """Return the first nested <ol> child of an <li>, or None."""
    for child in li:
        if child.tag.split('}')[-1] == 'ol':
            return child
    return None

NCX_NS = '{http://www.daisy.org/z3986/2005/ncx/}'

# Patterns that identify a real chapter entry. These match the label text
# (e.g. "Chapter 1", "Capítulo 3", "Chap. 12") or the href filename stem
# (e.g. "chapter01.xhtml"). Multilingual patterns are common in EPUBs.
_CHAPTER_LABEL_RE = re.compile(
    r'^\s*(?:chapter|chap\.?|cap[ií]tulo|chapitre|kapitel)\b',
    re.IGNORECASE,
)
_CHAPTER_FILE_RE = re.compile(
    r'^(?:ch|cap|chapter|chapter|chapitre|kapitel)',
    re.IGNORECASE,
)

# Patterns that identify front- or back-matter entries that should be
# dropped from the output. Matched against label and the filename stem
# (without extension) of the href.
_NON_CHAPTER_RE = re.compile(
    r'^\s*(?:'
    r'cubierta|cover(?:\s+page)?|t[ií]tulo|title(?:\s+page)?|'
    r'map[s]?|mapa[s]?|dedicatoria|dedication|info|'
    r'sinopsis|synopsis|summary|colophon|copyright(?:\s+page)?|'
    r'imprint|acknowledg(?:e?ments?)?|foreword|preface|'
    r'prologue|epilogue|afterword|appendix|index|glossary|'
    r'bibliography|half[\s\-]?title|table[\s_]of[\s_]contents|toc|'
    r'author|about[\s_]the[\s_]author|'
    r'contents'
    r')\b',
    re.IGNORECASE,
)


def looks_like_chapter(label, href=''):
    """True if the navPoint's label/href look like a real chapter entry.

    Conservative: matches "Chapter N" style labels, or hrefs whose filename
    stem starts with "ch"/"chapter"/etc. Anything matching the
    non-chapter patterns (front/back matter) is excluded even if it would
    otherwise match the chapter pattern.
    """
    label = (label or '').strip()
    href = href or ''
    fname = os.path.basename(href.split('#', 1)[0]).rsplit('.', 1)[0]

    if not label and not fname:
        return False

    if label and _NON_CHAPTER_RE.match(label):
        return False
    if fname and _NON_CHAPTER_RE.match(fname):
        return False

    if label and _CHAPTER_LABEL_RE.match(label):
        return True
    if fname and _CHAPTER_FILE_RE.match(fname):
        return True

    return False

# HTML named entities that XML parsers don't recognize. EPUBs in the wild
# commonly contain &nbsp; and friends; we rewrite to numeric refs before
# handing the bytes to ElementTree, which only understands the 5 XML builtins
# plus numeric character references.
_HTML_ENTITIES = {
    'nbsp': '&#160;',
    'copy': '&#169;',
    'reg': '&#174;',
    'trade': '&#8482;',
    'hellip': '&#8230;',
    'mdash': '&#8212;',
    'ndash': '&#8211;',
    'lsquo': '&#8216;',
    'rsquo': '&#8217;',
    'ldquo': '&#8220;',
    'rdquo': '&#8221;',
    'laquo': '&#171;',
    'raquo': '&#187;',
    'middot': '&#183;',
    'bull': '&#8226;',
    'deg': '&#176;',
    'times': '&#215;',
    'divide': '&#247;',
    'euro': '&#8364;',
}
_HTML_ENTITY_RE = re.compile(r'&(' + '|'.join(_HTML_ENTITIES) + r');')

def replace_html_entities(data):
    """Replace HTML named entities in bytes/str with numeric refs."""
    if isinstance(data, bytes):
        text = data.decode('utf-8', errors='replace')
        return _HTML_ENTITY_RE.sub(lambda m: _HTML_ENTITIES[m.group(1)], text).encode('utf-8')
    return _HTML_ENTITY_RE.sub(lambda m: _HTML_ENTITIES[m.group(1)], data)

def parse_xml_safely(data):
    """ElementTree.fromstring wrapper that survives HTML named entities."""
    if isinstance(data, bytes):
        data = replace_html_entities(data)
    else:
        data = replace_html_entities(data)
    return ET.fromstring(data)

def parse_ncx_parts(ncx_root):
    """Parse an NCX <navMap> into a parts list of the same shape as parse_toc_parts.

    Top-level navPoints with nested navPoints become Parts; their children
    become chapters. Top-level navPoints without children become single-chapter
    parts (then flattened into one "Chapters" part by the caller).
    """
    nav_map = None
    for el in ncx_root.iter():
        if el.tag == f'{NCX_NS}navMap':
            nav_map = el
            break
    if nav_map is None:
        return None, "NCX has no <navMap>"

    top_points = [c for c in nav_map if c.tag == f'{NCX_NS}navPoint']
    if not top_points:
        return None, "NCX <navMap> is empty"

    def label_text(np):
        for child in np:
            if child.tag == f'{NCX_NS}navLabel':
                for t in child:
                    if t.tag == f'{NCX_NS}text':
                        return "".join(t.itertext()).strip()
        return ''

    def content_src(np):
        for child in np:
            if child.tag == f'{NCX_NS}content':
                return child.attrib.get('src', '')
        return ''

    # First try: nested structure (Parts with chapter children)
    parts = []
    for np in top_points:
        children = [c for c in np if c.tag == f'{NCX_NS}navPoint']
        if children:
            chapters = []
            for child in children:
                cl = label_text(child)
                ch_href = content_src(child)
                if not looks_like_chapter(cl, ch_href):
                    continue
                chapters.append({
                    'title': cl or f'Chapter {len(chapters)+1}',
                    'href': ch_href,
                })
            if not chapters:
                continue
            parts.append({
                'title': label_text(np) or f'Part {len(parts)+1}',
                'header_href': content_src(np),  # part-level anchor, may be ''
                'chapters': chapters,
            })

    if parts:
        return parts, None

    # Fallback: flat — filter to chapter-like top-level navPoints
    chapters = []
    for np in top_points:
        cl = label_text(np)
        ch_href = content_src(np)
        if not looks_like_chapter(cl, ch_href):
            continue
        chapters.append({
            'title': cl or f'Chapter {len(chapters)+1}',
            'href': ch_href,
        })
    if chapters:
        return [{'title': 'Chapters', 'chapters': chapters}], None

    # If filtering wiped everything out, fall back to the unfiltered list
    # so the script still produces output. Print a warning so the caller
    # knows the filter didn't match.
    unfiltered = []
    for np in top_points:
        unfiltered.append({
            'title': label_text(np) or f'Chapter {len(unfiltered)+1}',
            'href': content_src(np),
        })
    if unfiltered:
        print("Warning: chapter filter matched no entries; including all navPoints.")
        return [{'title': 'Chapters', 'chapters': unfiltered}], None

    return None, "NCX contains no navPoints with content"

# Fragments emitted by InDesign (and common in EPUB 2) follow _idParaDest-N.
_NCX_ANCHOR_RE = re.compile(r'_idParaDest-\d+', re.IGNORECASE)

def find_anchor_paragraph_index(paragraphs_with_ids, fragment):
    """Map a URL fragment like '1984.xhtml#_idParaDest-7' to a paragraph index.

    `paragraphs_with_ids` is the parallel list of (id_or_None, text) tuples
    produced by parse_chapter_xhtml_with_ids. Returns -1 if not found.
    """
    if not fragment:
        return -1
    anchor = fragment.split('#', 1)[-1].strip()
    if not anchor:
        return -1
    for i, (pid, _) in enumerate(paragraphs_with_ids):
        if pid and pid == anchor:
            return i
    return -1

def parse_chapter_xhtml_with_ids(content):
    """Like parse_chapter_xhtml but returns [(id_or_None, text), ...] in order.

    The id is captured from <p id="..."> for paragraph elements only.
    """
    root = parse_xml_safely(content)
    body = root.find('.//{http://www.w3.org/1999/xhtml}body')
    if body is None:
        body = root

    paragraphs = []

    def traverse(node):
        tag = node.tag.split('}')[-1]
        node_class = node.attrib.get('class', '')

        if tag in ('hr', 'div') and 'transition' in node_class:
            if not paragraphs or paragraphs[-1][1] != '* * *':
                paragraphs.append((None, '* * *'))
            return

        if tag == 'p':
            text = get_clean_text(node)
            text = re.sub(r'[ \t\r\n\f]+', ' ', text).strip()
            if text:
                paragraphs.append((node.attrib.get('id'), text))
            return

        for child in node:
            traverse(child)

    traverse(body)
    return paragraphs

def parse_toc_parts(toc_nav):
    """Parse a TOC <nav> element into a list of {'title', 'chapters'} parts.

    Tries nested structure (parts containing chapter sublists) first, then
    falls back to a flat structure (one part containing all chapters).
    """
    top_ol = None
    for child in toc_nav:
        if child.tag.split('}')[-1] == 'ol':
            top_ol = child
            break
    if top_ol is None:
        return None, "Could not find ordered list <ol> in table of contents"

    # First pass: look for parts (li with nested ol)
    parts = []
    for li in top_ol:
        if li.tag.split('}')[-1] != 'li':
            continue
        nested_ol = collect_li_ol(li)
        if nested_ol is None:
            continue
        part_a = collect_li_a(li)
        part_title = (
            "".join(part_a.itertext()).strip()
            if part_a is not None
            else f"Part {len(parts)+1}"
        )
        chapters = []
        for chap_li in nested_ol:
            if chap_li.tag.split('}')[-1] != 'li':
                continue
            chap_a = collect_li_a(chap_li)
            if chap_a is None:
                continue
            ct = "".join(chap_a.itertext()).strip()
            ch_href = chap_a.attrib.get('href', '')
            if not looks_like_chapter(ct, ch_href):
                continue
            chapters.append({'title': ct, 'href': ch_href})
        if not chapters:
            continue
        parts.append({'title': part_title, 'chapters': chapters})

    if parts:
        return parts, None

    # Fallback: flat TOC — filter to chapter-like top-level li entries
    chapters = []
    for li in top_ol:
        if li.tag.split('}')[-1] != 'li':
            continue
        a = collect_li_a(li)
        if a is None:
            continue
        ct = "".join(a.itertext()).strip()
        ch_href = a.attrib.get('href', '')
        if not looks_like_chapter(ct, ch_href):
            continue
        chapters.append({'title': ct, 'href': ch_href})

    if chapters:
        return [{'title': 'Chapters', 'chapters': chapters}], None

    # If filtering matched nothing, fall back to the unfiltered list with a warning
    unfiltered = []
    for li in top_ol:
        if li.tag.split('}')[-1] != 'li':
            continue
        a = collect_li_a(li)
        if a is not None:
            unfiltered.append({
                'title': "".join(a.itertext()).strip(),
                'href': a.attrib.get('href', ''),
            })
    if unfiltered:
        print("Warning: chapter filter matched no entries; including all navPoints.")
        return [{'title': 'Chapters', 'chapters': unfiltered}], None

    return None, "No chapterized parts found in table of contents"

def find_cover_image(z, opf_root, opf_path):
    # Method 1: Check manifest for properties="cover-image" (EPUB 3)
    manifest = opf_root.find('.//{http://www.idpf.org/2007/opf}manifest')
    if manifest is not None:
        for item in manifest:
            props = item.attrib.get('properties', '')
            if 'cover-image' in props.split():
                href = item.attrib.get('href')
                if href:
                    return resolve_zip_path(opf_path, href)
                    
    # Method 2: Check meta name="cover" (EPUB 2)
    metadata = opf_root.find('.//{http://www.idpf.org/2007/opf}metadata')
    if metadata is not None:
        cover_id = None
        for meta in metadata.findall('.//{http://www.idpf.org/2007/opf}meta'):
            if meta.attrib.get('name') == 'cover':
                cover_id = meta.attrib.get('content')
                break
        if cover_id and manifest is not None:
            for item in manifest:
                if item.attrib.get('id') == cover_id:
                    href = item.attrib.get('href')
                    if href:
                        return resolve_zip_path(opf_path, href)

    # Method 3: Scan zip filenames for cover-like names
    cover_re = re.compile(r'.*cover.*\.(?:jpe?g|png|webp|gif|bmp)', re.IGNORECASE)
    for name in z.namelist():
        if cover_re.match(os.path.basename(name)):
            return name
            
    return None

def convert_ebook(input_path, output_dir):
    """Convert an ebook (epub, azw3, or mobi) to plain-text chapter files.

    EPUB files are processed directly. azw3/mobi files are first converted to a
    temporary EPUB via Calibre, then processed with the same pipeline.
    """
    if not os.path.exists(input_path):
        print(f"Error: Input file {input_path} does not exist.")
        return False

    ext = os.path.splitext(input_path)[1].lower()

    if ext == '.epub':
        return convert_epub(input_path, output_dir)

    if ext in ('.azw3', '.azw', '.mobi'):
        try:
            epub_path, temp_dir = convert_to_epub(input_path)
        except RuntimeError as e:
            print(f"Error: {e}")
            return False
        try:
            return convert_epub(epub_path, output_dir)
        finally:
            shutil.rmtree(temp_dir, ignore_errors=True)

    print(f"Error: Unsupported file type '{ext}'. Supported: .epub, .azw3, .azw, .mobi")
    return False

def convert_epub(epub_path, output_dir):
    print(f"Reading EPUB: {epub_path}")
    if not os.path.exists(epub_path):
        print(f"Error: Input file {epub_path} does not exist.")
        return False
        
    os.makedirs(output_dir, exist_ok=True)
    print(f"Output directory: {output_dir}")
    
    with zipfile.ZipFile(epub_path, 'r') as z:
        # 1. Read container.xml to locate the OPF file
        try:
            container_data = z.read('META-INF/container.xml')
            container_root = parse_xml_safely(container_data)
            # Find the full-path of the rootfile
            opf_path = None
            for rf in container_root.findall('.//{urn:oasis:names:tc:opendocument:xmlns:container}rootfile'):
                opf_path = rf.attrib.get('full-path')
                if opf_path:
                    break
        except Exception as e:
            print(f"Error reading META-INF/container.xml: {e}")
            return False
            
        if not opf_path:
            print("Error: Could not locate OPF file path in container.xml")
            return False
            
        print(f"Found OPF path: {opf_path}")
        
        # 2. Read OPF to find the navigation document (nav or NCX)
        try:
            opf_data = z.read(opf_path)
            opf_root = parse_xml_safely(opf_data)
            nav_href = None
            nav_kind = None  # 'nav' (EPUB 3 XHTML) or 'ncx' (EPUB 2)
            manifest = opf_root.find('.//{http://www.idpf.org/2007/opf}manifest')
            if manifest is not None:
                # EPUB 3: manifest item with properties="nav"
                for item in manifest:
                    props = item.attrib.get('properties', '')
                    if 'nav' in props.split():
                        nav_href = item.attrib.get('href')
                        nav_kind = 'nav'
                        break
                # EPUB 2: NCX item (media-type="application/x-dtbncx+xml")
                if not nav_href:
                    for item in manifest:
                        mt = item.attrib.get('media-type', '')
                        if mt == 'application/x-dtbncx+xml':
                            nav_href = item.attrib.get('href')
                            nav_kind = 'ncx'
                            break
                # Generic fallback by id
                if not nav_href and manifest is not None:
                    for item in manifest:
                        item_id = item.attrib.get('id', '').lower()
                        if 'nav' in item_id or 'toc' in item_id or item_id == 'ncx':
                            nav_href = item.attrib.get('href')
                            # Sniff kind from media-type or extension
                            mt = item.attrib.get('media-type', '')
                            if mt == 'application/x-dtbncx+xml' or nav_href.lower().endswith('.ncx'):
                                nav_kind = 'ncx'
                            else:
                                nav_kind = 'nav'
                            break
        except Exception as e:
            print(f"Error reading OPF file: {e}")
            return False
            
        if not nav_href:
            print("Error: Could not locate navigation document in manifest")
            return False
            
        nav_zip_path = resolve_zip_path(opf_path, nav_href)
        print(f"Found navigation document path: {nav_zip_path}")

        # Extract and convert cover art if found
        cover_zip_path = find_cover_image(z, opf_root, opf_path)
        if cover_zip_path:
            print(f"Found cover art image inside EPUB: {cover_zip_path}")
            try:
                import io
                from PIL import Image
                
                img_data = z.read(cover_zip_path)
                img = Image.open(io.BytesIO(img_data))
                
                try:
                    resample = Image.Resampling.LANCZOS
                except AttributeError:
                    resample = Image.ANTIALIAS
                
                img = img.resize((240, 416), resample)
                img = img.convert('1')
                
                mono_bytes = bytearray()
                for y in range(416):
                    current_byte = 0
                    for x in range(240):
                        val = img.getpixel((x, y))
                        bit = 1 if val == 0 else 0
                        current_byte = (current_byte << 1) | bit
                        if (x + 1) % 8 == 0:
                            mono_bytes.append(current_byte)
                            current_byte = 0
                            
                cover_out_path = os.path.join(output_dir, 'cover.mono')
                with open(cover_out_path, 'wb') as f:
                    f.write(mono_bytes)
                print(f"  Wrote cover art to cover.mono ({len(mono_bytes)} bytes)")
            except Exception as e:
                print(f"  Warning: failed to convert cover art: {e}")
        else:
            print("No cover art image found inside EPUB.")
        
        # 3. Parse navigation document (EPUB 3 nav or EPUB 2 NCX)
        try:
            nav_data = z.read(nav_zip_path)
            nav_root = parse_xml_safely(nav_data)
        except Exception as e:
            print(f"Error parsing navigation document: {e}")
            return False

        parts = None
        toc_err = None
        if nav_kind == 'ncx':
            parts, toc_err = parse_ncx_parts(nav_root)
        else:
            # Find the nav[@epub:type="toc"]
            toc_nav = None
            for el in nav_root.iter():
                tag = el.tag.split('}')[-1]
                if tag == 'nav':
                    for k, v in el.attrib.items():
                        if k.split('}')[-1] == 'type' and v == 'toc':
                            toc_nav = el
                            break
                    if toc_nav is not None:
                        break
            # Fallback to any nav if type="toc" not found
            if toc_nav is None:
                for el in nav_root.iter():
                    tag = el.tag.split('}')[-1]
                    if tag == 'nav':
                        toc_nav = el
                        break
            if toc_nav is None:
                print("Error: Could not find table of contents <nav> element")
                return False
            parts, toc_err = parse_toc_parts(toc_nav)

        if parts is None:
            print(f"Error: {toc_err}")
            return False

        if len(parts) == 1 and parts[0]['title'] == 'Chapters':
            print("Note: TOC is flat (no parts) — treating all entries as chapters of one part.")

        print(f"Successfully extracted {len(parts)} parts.")
        
        # 5. Build a per-file slicing plan, then extract text for each chapter.
        # For EPUB 3 nav files, each chapter href typically points at a whole
        # XHTML file. For EPUB 2 NCX, multiple chapters often point at the
        # same file with different URL fragments (#_idParaDest-N), so we parse
        # the file once and slice paragraphs by anchor.
        #
        # Each entry: (part_idx, chap_idx, title, file_path, anchor,
        #              start_override_anchor, end_override_anchor)
        # start_override_anchor is non-empty when this chapter is the first in
        # its part and the part has a header anchor in the same file — used so
        # the "Part N" title paragraph is included at the top.
        # end_override_anchor is non-empty when this chapter is the last in its
        # part and the next part has a header anchor in the same file — used so
        # the next part's title paragraph is NOT included in this chapter.
        chapters = []
        part_idx = 1
        global_chap_idx = 1
        for part in parts:
            part_header_href = part.get('header_href', '')  # NCX-only
            for chap_i, chap in enumerate(part['chapters']):
                chap_href = chap['href']
                if '#' in chap_href:
                    file_path, anchor = chap_href.split('#', 1)
                else:
                    file_path, anchor = chap_href, ''

                start_override = ''
                end_override = ''
                if part_header_href and '#' in part_header_href:
                    hdr_file, hdr_anchor = part_header_href.split('#', 1)
                    if hdr_file == file_path and hdr_anchor:
                        if chap_i == 0:
                            # Include the Part header paragraph in this chapter
                            start_override = hdr_anchor

                chapters.append((
                    part_idx, global_chap_idx, chap['title'],
                    file_path, anchor,
                    start_override, end_override,
                ))
                global_chap_idx += 1
            part_idx += 1

        # Now fill in end_override_anchor using the NEXT part's header (if
        # in the same file as the current last chapter of this part).
        # Build a lookup: (part_idx, last_chap_idx_in_part) -> chapter entry
        for ci, ch in enumerate(chapters):
            part_i = ch[0]
            # Is this the last chapter of its part?
            next_ch = chapters[ci + 1] if ci + 1 < len(chapters) else None
            is_last_in_part = (next_ch is None) or (next_ch[0] != part_i)
            if not is_last_in_part:
                continue
            # Find the next part's header href
            next_part = parts[part_i] if part_i < len(parts) else None
            if not next_part:
                continue
            next_header_href = next_part.get('header_href', '')
            if not next_header_href or '#' not in next_header_href:
                continue
            hdr_file, hdr_anchor = next_header_href.split('#', 1)
            if hdr_file == ch[3] and hdr_anchor:
                # Replace tuple element 6 with end_override
                chapters[ci] = ch[:6] + (hdr_anchor,)

        # Group chapters by file so each file is parsed at most once.
        # Resolve the file path relative to the navigation document first.
        files_in_order = []
        file_to_chapters = {}
        for ch in chapters:
            fp = resolve_zip_path(nav_zip_path, ch[3])
            if fp not in file_to_chapters:
                file_to_chapters[fp] = []
                files_in_order.append(fp)
            file_to_chapters[fp].append(ch)

        # Cache: file_path -> list of (id, text) (None for id when <p> had no id)
        file_paragraph_cache = {}

        def get_file_paragraphs(fp):
            if fp in file_paragraph_cache:
                return file_paragraph_cache[fp]
            try:
                content = z.read(fp)
            except KeyError:
                file_paragraph_cache[fp] = None
                return None
            paragraphs = parse_chapter_xhtml_with_ids(content)
            file_paragraph_cache[fp] = paragraphs
            return paragraphs

        def slice_chapter(file_paragraphs, start_anchor, end_anchor):
            """Return the [start, end) paragraph slice for one chapter."""
            if file_paragraphs is None:
                return []
            n = len(file_paragraphs)
            if not start_anchor:
                # No anchor -> take whole file. Should only apply to the first
                # chapter in a file; subsequent same-file chapters with no
                # anchor are degenerate and yield empty.
                return [t for _, t in file_paragraphs]
            start_idx = find_anchor_paragraph_index(file_paragraphs, start_anchor)
            if start_idx < 0:
                # Anchor not found in file — fall back to whole file
                return [t for _, t in file_paragraphs]
            if not end_anchor:
                end_idx = n
            else:
                end_idx = find_anchor_paragraph_index(file_paragraphs, end_anchor)
                if end_idx < 0 or end_idx <= start_idx:
                    end_idx = n
            return [t for _, t in file_paragraphs[start_idx:end_idx]]

        # 6. Write chapter files
        index_entries = []
        # Print per-part headers as we encounter them
        last_part_idx = 0
        for ch in chapters:
            if ch[0] != last_part_idx:
                last_part_idx = ch[0]
                # Find the part title from the parts list
                part_title = parts[ch[0] - 1]['title'] if ch[0] - 1 < len(parts) else f'Part {ch[0]}'
                print(f"Processing part {ch[0]}: {part_title}")
        for fp in files_in_order:
            file_paragraphs = get_file_paragraphs(fp)
            ch_list = file_to_chapters[fp]
            if file_paragraphs is None:
                print(f"  Warning: could not read {fp} from epub")
                for ch in ch_list:
                    part_i, gc_idx = ch[0], ch[1]
                    filename = f"p{part_i}c{gc_idx}.txt"
                    filepath = os.path.join(output_dir, filename)
                    open(filepath, 'w', encoding='utf-8', newline='\n').close()
                    index_entries.append(
                        f"Part {part_i} Chapter {gc_idx}:{filename}"
                    )
                continue

            for i, ch in enumerate(ch_list):
                part_i, gc_idx, ch_title, _, start_anchor, start_override, end_override = ch
                # Use part-header anchor as the start when this is the first
                # chapter of a part and the override points into the same file.
                effective_start = start_override or start_anchor
                # End anchor priority: explicit part-boundary override > next
                # chapter in the same file > end of file.
                end_anchor = end_override
                if not end_anchor and i + 1 < len(ch_list):
                    end_anchor = ch_list[i + 1][4]
                paragraphs = slice_chapter(file_paragraphs, effective_start, end_anchor)

                filename = f"p{part_i}c{gc_idx}.txt"
                filepath = os.path.join(output_dir, filename)
                with open(filepath, 'w', encoding='utf-8', newline='\n') as f:
                    for p in paragraphs:
                        f.write(p + '\n')

                print(f"  Wrote {len(paragraphs)} paragraphs to {filename} ({ch_title})")
                index_entries.append(f"Part {part_i} Chapter {gc_idx}:{filename}")

        # 7. Write index.txt
        index_path = os.path.join(output_dir, 'index.txt')
        with open(index_path, 'w', encoding='utf-8', newline='\n') as f:
            for entry in index_entries:
                f.write(entry + '\n')
                
        print(f"Successfully wrote index.txt containing {len(index_entries)} chapter mappings.")
        return True

if __name__ == '__main__':
    default_epub = 'books/unformatted/The Compound - Aisling Rawle.epub'
    default_output = 'books/formatted/The_Compound'

    epub_arg = sys.argv[1] if len(sys.argv) > 1 else default_epub
    output_arg = sys.argv[2] if len(sys.argv) > 2 else default_output

    success = convert_ebook(epub_arg, output_arg)
    if success:
        print("Conversion completed successfully!")
        sys.exit(0)
    else:
        print("Conversion failed.")
        sys.exit(1)