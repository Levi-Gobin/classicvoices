package com.levigobin.classicvoices;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Just enough HFS+ (and iOS's case-sensitive HFSX) to find files in a root
 * filesystem and copy them out, after applereaders.py's Volume: every leaf
 * of the catalog B-tree is read into memory instead of implementing HFS+'s
 * key ordering, and files with more than eight extents get the rest from
 * the extents overflow tree.
 */
final class Hfs {
    static final int ROOT_ID = 2;

    static final class Entry {
        final String name;
        final boolean dir;
        final long id, size;
        List<long[]> extents;  // {startBlock, blockCount}; null when too fragmented
        long blocks;
        long rsrcSize;                                   // the resource fork, where
        final List<long[]> rsrcExtents = new ArrayList<>();  // compressed files keep their data

        Entry(String name, boolean dir, long id, long size, List<long[]> extents, long blocks) {
            this.name = name;
            this.dir = dir;
            this.id = id;
            this.size = size;
            this.extents = extents;
            this.blocks = blocks;
        }
    }

    private final Source f;
    private final long base;
    private final long bs;
    private final Map<Long, List<Entry>> kids = new HashMap<>();
    private final Map<Long, Entry> shortOnes = new HashMap<>();

    /** The offset of the HFS+ volume inside a container: a bare volume or an Apple partition map. */
    static long findHfs(Source s) throws IOException {
        byte[] b0 = s.read(0, 16);
        if (b0[0] != 'E' || b0[1] != 'R') {
            byte[] sig = s.read(1024, 2);
            if ((sig[0] == 'H' && (sig[1] == '+' || sig[1] == 'X'))) return 0;
            long gpt = findHfsGpt(s);
            if (gpt >= 0) return gpt;
            throw new IOException("no Apple partition map and no HFS volume");
        }
        int stride = be16(b0, 2);
        if (stride == 0) stride = 512;
        byte[] e = s.read(stride, 512);
        if (e[0] != 'P' || e[1] != 'M') throw new IOException("the partition map is not where block zero says");
        long count = Source.be32(e, 4);
        for (int i = 0; i < count; i++) {
            byte[] p = s.read((long) stride * (1 + i), 512);
            if (p[0] != 'P' || p[1] != 'M') break;
            long start = Source.be32(p, 8);
            String kind = new String(p, 48, 32, "ISO-8859-1");
            int z = kind.indexOf('\0');
            if (z >= 0) kind = kind.substring(0, z);
            if (kind.equals("Apple_HFS") || kind.equals("Apple_HFSX")) return start * stride;
        }
        throw new IOException("no Apple_HFS partition in the image");
    }

    // Apple HFS+ partition type 48465300-0000-11AA-AA11-00306543ECAC, as stored on disk.
    private static final byte[] GPT_HFS = {0x00, 0x53, 0x46, 0x48, 0x00, 0x00, (byte) 0xAA, 0x11, (byte) 0xAA, 0x11, 0x00,
            0x30, 0x65, 0x43, (byte) 0xEC, (byte) 0xAC};

    /** The HFS+ partition in a GUID partition table (iOS 10.3's root filesystem), or -1. */
    static long findHfsGpt(Source s) throws IOException {
        for (int sector : new int[] {512, 4096}) {
            byte[] h = s.read(sector, 92);
            if (!new String(h, 0, 8, "ISO-8859-1").equals("EFI PART")) continue;
            long lba = Source.le64(h, 72);
            long count = Source.le32(h, 80), esize = Source.le32(h, 84);
            for (int i = 0; i < Math.min(count, 128); i++) {
                byte[] e = s.read(lba * sector + i * esize, (int) esize);
                if (java.util.Arrays.equals(java.util.Arrays.copyOf(e, 16), GPT_HFS)) {
                    long start = Source.le64(e, 32) * sector;
                    byte[] sig = s.read(start + 1024, 2);
                    if (sig[0] == 'H' && (sig[1] == '+' || sig[1] == 'X')) return start;
                }
            }
        }
        return -1;
    }

    private static int be16(byte[] b, int o) { return Source.be16(b, o); }

    Hfs(Source f, long base) throws IOException {
        this.f = f;
        this.base = base;
        byte[] vh = f.read(base + 1024, 512);
        if (vh[0] != 'H' || (vh[1] != '+' && vh[1] != 'X')) throw new IOException("no HFS+ volume header");
        bs = Source.be32(vh, 40);
        volumeHeader = vh;
        readTree(vh, 272, true);
        if (!shortOnes.isEmpty()) {
            readTree(vh, 192, false);
            for (Entry e : shortOnes.values()) {
                Collections.sort(overflowFor(e.id), new Comparator<long[]>() {
                    @Override
                    public int compare(long[] a, long[] b) {
                        return Long.compare(a[0], b[0]);
                    }
                });
                long have = 0;
                for (long[] x : e.extents) have += x[1];
                for (long[] rec : overflowFor(e.id)) {
                    for (int j = 0; j < 8; j++) {
                        if (rec[2 + 2 * j + 1] != 0) {
                            e.extents.add(new long[] {rec[2 + 2 * j], rec[2 + 2 * j + 1]});
                            have += rec[2 + 2 * j + 1];
                        }
                    }
                }
                if (have < e.blocks) e.extents = null;
            }
        }
    }

    private final Map<Long, List<long[]>> overflow = new HashMap<>();

    private List<long[]> overflowFor(long id) {
        List<long[]> l = overflow.get(id);
        if (l == null) {
            l = new ArrayList<>();
            overflow.put(id, l);
        }
        return l;
    }

    private void readTree(byte[] vh, int forkOff, boolean catalog) throws IOException {
        readTree(vh, forkOff, catalog ? 0 : 1);
    }

    /** kind: 0 catalog, 1 extents overflow, 2 attributes. */
    private void readTree(byte[] vh, int forkOff, int kind) throws IOException {
        long logical = Source.be64(vh, forkOff);
        if (logical > Integer.MAX_VALUE) throw new IOException("B-tree too large");
        byte[] tree = new byte[(int) logical];
        int at = 0;
        for (int i = 0; i < 8 && at < tree.length; i++) {
            long start = Source.be32(vh, forkOff + 16 + 8 * i), count = Source.be32(vh, forkOff + 20 + 8 * i);
            if (count == 0) continue;
            int n = (int) Math.min(count * bs, tree.length - at);
            f.read(base + start * bs, tree, at, n);
            at += n;
        }
        if (tree.length < 14) return;
        if (at < tree.length) throw new IOException("a B-tree is truncated (more than eight extents)");
        int nodeSize = be16(tree, 32);
        long node = Source.be32(tree, 24);
        java.util.HashSet<Long> seen = new java.util.HashSet<>();
        while (node != 0 && seen.add(node)) {
            int no = (int) (node * nodeSize);
            if (no + nodeSize > tree.length) break;
            long flink = Source.be32(tree, no);
            byte nodeKind = tree[no + 8];
            int nrecs = be16(tree, no + 10);
            if (nodeKind != -1) break;  // not a leaf
            for (int i = 0; i < nrecs; i++) {
                int a = be16(tree, no + nodeSize - 2 * (i + 1));
                int b = be16(tree, no + nodeSize - 2 * (i + 2));
                if (kind == 0) catalogRecord(tree, no + a, b - a);
                else if (kind == 1) overflowRecord(tree, no + a, b - a);
                else attributeRecord(tree, no + a, b - a);
            }
            node = flink;
        }
    }

    private void catalogRecord(byte[] t, int o, int len) {
        if (len < 8) return;
        int klen = be16(t, o);
        long parent = Source.be32(t, o + 2);
        int nlen = be16(t, o + 6);
        char[] name = new char[nlen];
        for (int i = 0; i < nlen; i++) name[i] = (char) be16(t, o + 8 + 2 * i);
        int d = o + 2 + klen + (klen % 2);
        if (d + 12 > o + len) return;
        int kind = (short) be16(t, d);
        long id = Source.be32(t, d + 8);
        List<Entry> l = kids.get(parent);
        if (l == null) {
            l = new ArrayList<>();
            kids.put(parent, l);
        }
        if (kind == 1) {
            l.add(new Entry(new String(name), true, id, 0, new ArrayList<long[]>(), 0));
        } else if (kind == 2) {
            int fork = d + 88;
            if (fork + 80 > t.length) return;
            long size = Source.be64(t, fork);
            long blocks = Source.be32(t, fork + 12);
            List<long[]> ext = new ArrayList<>();
            long have = 0;
            for (int j = 0; j < 8; j++) {
                long s = Source.be32(t, fork + 16 + 8 * j), c = Source.be32(t, fork + 20 + 8 * j);
                if (c != 0) {
                    ext.add(new long[] {s, c});
                    have += c;
                }
            }
            Entry e = new Entry(new String(name), false, id, size, ext, blocks);
            int rf = d + 168;
            if (rf + 80 <= t.length) {
                e.rsrcSize = Source.be64(t, rf);
                for (int j = 0; j < 8 && e.rsrcSize > 0; j++) {
                    long s = Source.be32(t, rf + 16 + 8 * j), c = Source.be32(t, rf + 20 + 8 * j);
                    if (c != 0) e.rsrcExtents.add(new long[] {s, c});
                }
            }
            l.add(e);
            if (have < blocks) shortOnes.put(id, e);
        }
    }

    private void overflowRecord(byte[] t, int o, int len) {
        if (len < 12 + 64) return;
        int klen = be16(t, o);
        int forkType = t[o + 2] & 0xFF;
        long fileId = Source.be32(t, o + 4), startBlock = Source.be32(t, o + 8);
        if (forkType != 0) return;
        int d = o + 2 + klen;
        long[] rec = new long[18];
        rec[0] = startBlock;
        rec[1] = 0;
        for (int j = 0; j < 8; j++) {
            rec[2 + 2 * j] = Source.be32(t, d + 8 * j);
            rec[2 + 2 * j + 1] = Source.be32(t, d + 8 * j + 4);
        }
        overflowFor(fileId).add(rec);
    }

    // ---------------------------------------------------------------- HFS+ compression

    private byte[] volumeHeader;
    private Map<Long, byte[]> decmpfs;

    private void attributeRecord(byte[] t, int o, int len) {
        if (len < 14) return;
        int klen = be16(t, o);
        long fileId = Source.be32(t, o + 4);
        int nlen = be16(t, o + 12);
        if (14 + 2 * nlen > len) return;
        char[] name = new char[nlen];
        for (int i = 0; i < nlen; i++) name[i] = (char) be16(t, o + 14 + 2 * i);
        if (!new String(name).equals("com.apple.decmpfs")) return;
        int d = o + 2 + klen;
        if (Source.be32(t, d) != 0x10) return;  // inline attribute data
        int n = (int) Source.be32(t, d + 12);
        if (d + 16 + n > t.length) return;
        decmpfs.put(fileId, java.util.Arrays.copyOfRange(t, d + 16, d + 16 + n));
    }

    /**
     * A file stored with HFS+ compression (empty data fork, com.apple.decmpfs
     * attribute), as iOS 7 and 10 keep their system files: its contents, or
     * null when it isn't compressed. Types 3 (zlib in the attribute) and 4
     * (zlib chunks in the resource fork).
     */
    byte[] compressed(Entry e) throws IOException {
        if (e.dir || e.size != 0) return null;
        if (decmpfs == null) {
            decmpfs = new HashMap<>();
            readTree(volumeHeader, 352, 2);
        }
        byte[] h = decmpfs.get(e.id);
        if (h == null || h.length < 16 || h[0] != 'f' || h[1] != 'p' || h[2] != 'm' || h[3] != 'c') return null;
        int type = (int) Source.le32(h, 4);
        long size = Source.le64(h, 8);
        if (size > Integer.MAX_VALUE) throw new IOException(e.name + " is too large");
        java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream((int) size);
        if (type == 3) {
            if ((h[16] & 0xFF) == 0xFF) out.write(h, 17, (int) Math.min(size, h.length - 17));
            else inflate(h, 16, h.length - 16, out);
        } else if (type == 4) {
            if (e.rsrcSize > (256 << 20)) throw new IOException(e.name + ": resource fork too large");
            byte[] r = new byte[(int) e.rsrcSize];
            int at = 0;
            for (long[] x : e.rsrcExtents) {
                int n = (int) Math.min(x[1] * bs, r.length - at);
                if (n <= 0) break;
                f.read(base + x[0] * bs, r, at, n);
                at += n;
            }
            if (at < r.length) throw new IOException(e.name + ": resource fork too fragmented");
            int head = (int) Source.be32(r, 0);
            long n = Source.le32(r, head + 4);
            for (int i = 0; i < n; i++) {
                int off = (int) Source.le32(r, head + 8 + 8 * i), ln = (int) Source.le32(r, head + 12 + 8 * i);
                int c = head + 4 + off;
                if ((r[c] & 0xFF) == 0xFF) out.write(r, c + 1, ln - 1);
                else inflate(r, c, ln, out);
            }
        } else {
            throw new IOException(e.name + " uses HFS+ compression type " + type + ", which this reader can't expand");
        }
        byte[] b = out.toByteArray();
        return b.length > size ? java.util.Arrays.copyOf(b, (int) size) : b;
    }

    private static void inflate(byte[] b, int o, int n, java.io.OutputStream out) throws IOException {
        java.util.zip.Inflater inf = new java.util.zip.Inflater();
        inf.setInput(b, o, n);
        byte[] buf = new byte[65536];
        try {
            while (!inf.finished()) {
                int r = inf.inflate(buf);
                if (r == 0 && (inf.needsInput() || inf.needsDictionary())) break;
                out.write(buf, 0, r);
            }
        } catch (java.util.zip.DataFormatException x) {
            throw new IOException("damaged compressed file: " + x.getMessage());
        } finally {
            inf.end();
        }
    }

    Entry entry(String path) {
        long node = ROOT_ID;
        Entry found = null;
        for (String part : path.split("/")) {
            if (part.isEmpty()) continue;
            Entry hit = null;
            List<Entry> l = kids.get(node);
            if (l != null) for (Entry e : l) if (e.name.equals(part)) { hit = e; break; }
            if (hit == null) return null;
            found = hit;
            node = hit.id;
        }
        return found;
    }

    List<Entry> children(Entry e) {
        List<Entry> l = kids.get(e.id);
        return l != null ? l : new ArrayList<Entry>();
    }

    void copyFile(Entry e, File dest, Progress p) throws IOException {
        //noinspection ResultOfMethodCallIgnored
        dest.getParentFile().mkdirs();
        byte[] packed = compressed(e);
        if (packed != null) {
            OutputStream o = new FileOutputStream(dest);
            try {
                o.write(packed);
            } finally {
                o.close();
            }
            return;
        }
        if (e.extents == null) throw new IOException(e.name + " is too fragmented to read");
        OutputStream out = new FileOutputStream(dest);
        try {
            long left = e.size;
            byte[] buf = new byte[(int) Math.min(1 << 20, Math.max(bs, 4096) * 64)];
            for (long[] x : e.extents) {
                long off = base + x[0] * bs, n = Math.min(x[1] * bs, left);
                while (n > 0) {
                    if (p != null && p.cancelled()) throw new Progress.Cancelled();
                    int take = (int) Math.min(buf.length, n);
                    f.read(off, buf, 0, take);
                    out.write(buf, 0, take);
                    off += take;
                    n -= take;
                    left -= take;
                }
                if (left <= 0) break;
            }
        } finally {
            out.close();
        }
    }

    void copyTree(Entry dir, File dest, Progress p) throws IOException {
        for (Entry c : children(dir)) {
            if (c.name.contains("/") || c.name.equals("..") || c.name.equals(".")) continue;
            File d = new File(dest, c.name);
            if (c.dir) copyTree(c, d, p);
            else copyFile(c, d, p);
        }
    }
}
