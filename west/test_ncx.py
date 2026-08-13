import zipfile

zf = zipfile.ZipFile(r'C:\Users\Scott\Downloads\He Who Fights with Monsters 1 - Shirtaloon.epub', 'r')
ncx_buf = zf.read('toc.ncx').decode('utf-8', errors='ignore')

p = 0
matches = 0
while True:
    np = ncx_buf.lower().find('<navpoint', p)
    if np == -1:
        break
    p = ncx_buf.find('>', np) + 1
    close_np = ncx_buf.lower().find('</navpoint>', p)
    next_np = ncx_buf.lower().find('<navpoint', p)
    limit = close_np
    if next_np != -1 and (limit == -1 or next_np < limit):
        limit = next_np
    if limit == -1:
        limit = len(ncx_buf)
    
    lbl = ncx_buf.lower().find('<text>', p)
    if lbl == -1 or lbl >= limit: 
        print(f'lbl fail at p={p}, lbl={lbl}, limit={limit}')
        continue
    lbl += 6
    lbl_end = ncx_buf.find('<', lbl)
    title = ncx_buf[lbl:lbl_end]
    src = ncx_buf.lower().find('src=', p)
    if src == -1 or src >= limit:
        print(f'src fail at p={p}, src={src}, limit={limit}')
        continue
    src += 4
    if ncx_buf[src] in ('"', "'"):
        src += 1
    end_q = ncx_buf.find('"', src)
    href = ncx_buf[src:end_q].split('#')[0].split('/')[-1]
    print(f'MATCH #{matches+1}: "{title}" -> "{href}"')
    matches += 1

print(f'Total matches: {matches}')
