import struct, sys
d = open("extracted/ios3/VoiceServices.framework/Support/voiced",'rb').read()
DATA_VM, DATA_OFF = 0x190000, 1634304
def rd(a, n):
    return d[DATA_OFF + a - DATA_VM: DATA_OFF + a - DATA_VM + n] if a >= DATA_VM else d[a-0x1000:a-0x1000+n]
for a in sys.argv[1:]:
    a = int(a, 16); print(hex(a), [hex(w) for w in struct.unpack('<8I', rd(a, 32))])
