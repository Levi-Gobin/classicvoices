# Panthera Speech's disc reader

`pantherahfs.py` is from Panthera Speech by Tamas Geczy
(https://github.com/tgeczy/panthera-speech, commit 7a02c80,
`panthera/addon/synthDrivers/_panthera/pantherahfs.py`), MIT licensed (see
LICENSE), unchanged. It reads HFS+ volumes, Apple partition maps, UDIF disk
images, xar packages and cpio payloads. tools/fetch_old_macintalk.py uses it to
read the Tiger, Leopard and Snow Leopard install discs on archive.org without
downloading them whole.
