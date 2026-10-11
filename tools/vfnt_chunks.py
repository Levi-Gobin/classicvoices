#!/usr/bin/env python3
"""Print the chunk tree of a Vocalizer voice font (RIF4 container, tags/text XOR 0xA9; plain RIFF files are detected)."""
import sys, struct
XOR=0xA9
def tag(b): return bytes(x^XOR for x in b).decode('latin1')
def walk(f, off, end, depth):
    while off+8 <= end:
        f.seek(off); hdr=f.read(8)
        t=tag(hdr[:4]); ln=struct.unpack('<I',hdr[4:])[0]
        print('  '*depth+f'{t!r} @0x{off:x} len={ln}')
        body=off+8
        if t in ('RIF4','RIFF','LIST'):
            f.seek(body); print('  '*depth+f'  form={tag(f.read(4))!r}'); walk(f, body+4, body+ln, depth+1)
        else:
            f.seek(body); d=f.read(min(ln,400)); txt=bytes(x^XOR for x in d)
            if all(32<=c<127 or c in (9,10,13,0) for c in txt[:64]): print('  '*depth+'  text: '+repr(txt.rstrip(b'\0').replace(b'\0',b' ')))
            else: print('  '*depth+'  raw: '+d[:64].hex())
        off=(body+ln+3)&~3
with open(sys.argv[1],'rb') as f:
    if f.read(4) in (b'RIFF', b'RIF4'): XOR=0
    f.seek(0,2); sz=f.tell(); walk(f,0,sz,0)
