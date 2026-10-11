import struct, sys
V = "extracted/ios6/VoiceServices.framework/Support/voiced"
d = open(V,'rb').read()
DATA_VM, DATA_OFF = 0x207000, None
# __DATA fileoff: TEXT filesize = 2121728
DATA_OFF = 2121728
def rd(addr, n):
    if addr >= DATA_VM: return d[DATA_OFF + addr - DATA_VM: DATA_OFF + addr - DATA_VM + n]
    return d[addr - 0x1000: addr - 0x1000 + n]
for a in sys.argv[1:]:
    a = int(a, 16)
    words = struct.unpack('<8I', rd(a, 32))
    print(hex(a), [hex(w) for w in words])
