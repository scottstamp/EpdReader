#!/usr/bin/env python3
import os
import sys
import re
import zipfile
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
    root = ET.fromstring(content)
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
            text = re.sub(r'\s+', ' ', text).strip()
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
            container_root = ET.fromstring(container_data)
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
        
        # 2. Read OPF to find the navigation document (nav)
        try:
            opf_data = z.read(opf_path)
            opf_root = ET.fromstring(opf_data)
            # Find manifest items with properties="nav"
            nav_href = None
            manifest = opf_root.find('.//{http://www.idpf.org/2007/opf}manifest')
            if manifest is not None:
                for item in manifest:
                    props = item.attrib.get('properties', '')
                    if 'nav' in props.split():
                        nav_href = item.attrib.get('href')
                        break
            # Fallback: if not found, look for tocNCX or item with id containing nav/toc
            if not nav_href and manifest is not None:
                for item in manifest:
                    item_id = item.attrib.get('id', '').lower()
                    if 'nav' in item_id or 'toc' in item_id:
                        nav_href = item.attrib.get('href')
                        break
        except Exception as e:
            print(f"Error reading OPF file: {e}")
            return False
            
        if not nav_href:
            print("Error: Could not locate navigation document in manifest")
            return False
            
        nav_zip_path = resolve_zip_path(opf_path, nav_href)
        print(f"Found navigation document path: {nav_zip_path}")
        
        # 3. Parse navigation document to get table of contents
        try:
            nav_data = z.read(nav_zip_path)
            nav_root = ET.fromstring(nav_data)
            
            # Find the nav[@epub:type="toc"]
            toc_nav = None
            for el in nav_root.iter():
                tag = el.tag.split('}')[-1]
                if tag == 'nav':
                    for k, v in el.attrib.items():
                        if k.split('}')[-1] == 'type' and v == 'toc':
                            toc_nav = el
                            break
            # Fallback to any nav if type="toc" not found
            if toc_nav is None:
                for el in nav_root.iter():
                    tag = el.tag.split('}')[-1]
                    if tag == 'nav':
                        toc_nav = el
                        break
        except Exception as e:
            print(f"Error parsing navigation document: {e}")
            return False
            
        if toc_nav is None:
            print("Error: Could not find table of contents <nav> element")
            return False
            
        # 4. Extract Parts and Chapters structure from TOC nav
        top_ol = None
        for child in toc_nav:
            if child.tag.split('}')[-1] == 'ol':
                top_ol = child
                break
                
        if top_ol is None:
            print("Error: Could not find ordered list <ol> in table of contents")
            return False
            
        parts = []
        for li in top_ol:
            if li.tag.split('}')[-1] != 'li':
                continue
            # Check for nested ol representing chapters under a part
            nested_ol = None
            for child in li:
                if child.tag.split('}')[-1] == 'ol':
                    nested_ol = child
                    break
            
            if nested_ol is not None:
                part_a = None
                for child in li:
                    if child.tag.split('}')[-1] == 'a':
                        part_a = child
                        break
                part_title = "".join(part_a.itertext()).strip() if part_a is not None else f"Part {len(parts)+1}"
                
                chapters = []
                for chap_li in nested_ol:
                    if chap_li.tag.split('}')[-1] != 'li':
                        continue
                    chap_a = None
                    for child in chap_li:
                        if child.tag.split('}')[-1] == 'a':
                            chap_a = child
                            break
                    if chap_a is not None:
                        chap_title = "".join(chap_a.itertext()).strip()
                        chap_href = chap_a.attrib.get('href', '')
                        chapters.append({
                            'title': chap_title,
                            'href': chap_href
                        })
                parts.append({
                    'title': part_title,
                    'chapters': chapters
                })
                
        if not parts:
            print("Error: No chapterized parts found in table of contents")
            return False
            
        print(f"Successfully extracted {len(parts)} parts.")
        
        # 5. Extract text for each chapter and write to output files
        index_entries = []
        part_idx = 1
        global_chap_idx = 1
        
        for part in parts:
            print(f"Processing part {part_idx}: {part['title']}")
            for chap in part['chapters']:
                chap_title = chap['title']
                chap_href = chap['href']
                
                chap_zip_path = resolve_zip_path(nav_zip_path, chap_href)
                print(f"  Chapter {global_chap_idx}: {chap_title} ({chap_zip_path})")
                
                try:
                    chap_content = z.read(chap_zip_path)
                    paragraphs = parse_chapter_xhtml(chap_content)
                except Exception as e:
                    print(f"    Error reading/parsing chapter {chap_zip_path}: {e}")
                    continue
                    
                # Write to output file
                filename = f"p{part_idx}c{global_chap_idx}.txt"
                filepath = os.path.join(output_dir, filename)
                
                with open(filepath, 'w', encoding='utf-8', newline='\n') as f:
                    for p in paragraphs:
                        f.write(p + '\n')
                        
                print(f"    Wrote {len(paragraphs)} paragraphs to {filename}")
                
                # Add to index.txt entries
                index_title = f"Part {part_idx} Chapter {global_chap_idx}"
                index_entries.append(f"{index_title}:{filename}")
                
                global_chap_idx += 1
            part_idx += 1
            
        # 6. Write index.txt
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
    
    success = convert_epub(epub_arg, output_arg)
    if success:
        print("Conversion completed successfully!")
        sys.exit(0)
    else:
        print("Conversion failed.")
        sys.exit(1)
