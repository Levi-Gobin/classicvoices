"""Write one member of a xar archive (an installer .pkg) to stdout, without
unpacking the rest: tools/xar_member.py Essentials.pkg Payload | tar -xzf - ...

Members are written as stored: a pkg's Payload is a gzip'd cpio archive, which
tar reads directly.
"""
import struct
import sys
import xml.etree.ElementTree as ET
import zlib


def main():
    path, name = sys.argv[1], sys.argv[2]
    with open(path, "rb") as f:
        magic, hsize, _ver, toc_len, _toc_raw, _alg = struct.unpack(">4sHHQQI", f.read(28))
        if magic != b"xar!":
            sys.exit("%s: not a xar archive" % path)
        f.seek(hsize)
        toc = ET.fromstring(zlib.decompress(f.read(toc_len)))
        heap = hsize + toc_len
        for node in toc.iter("file"):
            if node.findtext("name") == name and node.find("data") is not None:
                data = node.find("data")
                offset, length = int(data.findtext("offset")), int(data.findtext("length"))
                break
        else:
            sys.exit("%s: no member %s" % (path, name))
        f.seek(heap + offset)
        out = sys.stdout.buffer
        while length:
            chunk = f.read(min(length, 1 << 20))
            if not chunk:
                sys.exit("%s: truncated" % path)
            out.write(chunk)
            length -= len(chunk)


if __name__ == "__main__":
    main()
