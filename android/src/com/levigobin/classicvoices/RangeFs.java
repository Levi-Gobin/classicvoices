package com.levigobin.classicvoices;

import java.io.ByteArrayOutputStream;
import java.io.EOFException;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.zip.DataFormatException;
import java.util.zip.Inflater;

/**
 * A few files out of an IPSW's root filesystem straight from Apple, by HTTP
 * range requests, without downloading the image (Firmware.streamRootfs
 * downloads all of it, ~1.2 GB for iOS 7). Used for engine-only firmware:
 * iOS 7.1.2's engine is 3 MB, and this fetches about 50 MB. The NVDA add-on's
 * rangefs.py, line for line.
 *
 * The root filesystem is a zip member, deflated; it is encrypted (encrcdsa),
 * so after a short Huffman-coded start deflate wrote it as stored blocks: a
 * 5-byte header, then ~16.4 KB copied as is. Their lengths vary by a few
 * bytes, so where a byte of the image sits in the zip is known only up to a
 * few headers. Two things pin it:
 * - Between two places whose positions are known exactly, the number of
 *   headers is exact, and the count up to any point between them is off by a
 *   small fraction of a header (a random walk of a few bytes per block), so
 *   it rounds to the exact one.
 * - Where the decrypted content can say (a deflated chunk of the disk image
 *   inside starts with a zlib stream; text and code aren't noise), it must
 *   agree, and it decides where the count can't.
 * Reading on from a stretch already placed needs neither: the next header's
 * position is known.
 */
final class RangeFs {
    private RangeFs() {}

    static final int BLOCK = 16390;  // typical stored-block length
    static final int WINDOW = 300000;  // bytes fetched on either side of an estimate

    static double entropy(byte[] b, int o, int n) {
        int[] c = new int[256];
        for (int i = o; i < o + n; i++) c[b[i] & 0xFF]++;
        double h = 0;
        for (int v : c) {
            if (v == 0) continue;
            double p = v / (double) n;
            h -= p * (Math.log(p) / Math.log(2));
        }
        return h;
    }

    /** Stored blocks from header h in d: {header position, length}. */
    static List<int[]> chain(byte[] d, int h) {
        List<int[]> out = new ArrayList<>();
        while (h + 5 <= d.length) {
            int n = Source.le16(d, h + 1), nn = Source.le16(d, h + 3);
            if ((d[h] != 0 && d[h] != 1) || (n ^ nn) != 0xFFFF || n == 0) break;
            out.add(new int[] {h, n});
            h += 5 + n;
        }
        return out;
    }

    static List<int[]> findChain(byte[] d, int need) {
        for (int h = 0; h < d.length - 5; h++) {
            List<int[]> c = chain(d, h);
            if (c.size() >= need) return c;
        }
        return new ArrayList<>();
    }

    /** How many places in body disagree with the encrypted image's structure there, or null if none can tell. */
    interface Check {
        Integer check(long plain0, byte[] body);
    }

    private static final class Span {
        final long at;
        final byte[] b;
        final int o, len;

        Span(long at, byte[] b, int o, int len) {
            this.at = at;
            this.b = b;
            this.o = o;
            this.len = len;
        }
    }

    /** Seekable view of a deflated zip member whose body is stored blocks. */
    static final class Member extends Source {
        final String url;
        final long start, csize, size;
        Check check;  // null: it can't tell
        long fetched;
        private final Progress progress;
        private final List<Span> spans = new ArrayList<>();  // sorted by plain start
        private final List<long[]> anchors = new ArrayList<>();  // {compressed payload start, plain start}, exact, sorted
        private final List<long[]> runs = new ArrayList<>();  // {plain end, compressed position of the next header}
        private final double mean;

        Member(String url, long start, long csize, long usize, Progress progress) throws IOException {
            this.url = url;
            this.start = start;
            this.csize = csize;
            this.size = usize;
            this.progress = progress;
            byte[] head = get(start, start + (4 << 20) - 1);
            List<int[]> c = findChain(head, 8);
            if (c.isEmpty()) throw new IOException("the root filesystem isn't stored the expected way");
            byte[] plain = inflate(new Inflater(true), head, 0, c.get(0)[0]);
            span(0, plain, 0, plain.length);
            long sum = 0;
            for (int[] x : c) sum += x[1];
            mean = sum / (double) c.size();
            add(head, start, c, plain.length);
            // the member ends with a stored block: its end is the image's end
            byte[] tail = get(start + csize - BLOCK - 64, start + csize - 1);
            boolean found = false;
            for (int h = 0; h < tail.length - 5; h++) {
                int n = Source.le16(tail, h + 1), nn = Source.le16(tail, h + 3);
                if ((tail[h] == 0 || tail[h] == 1) && (n ^ nn) == 0xFFFF && h + 5 + n == tail.length) {
                    insort(anchors, new long[] {start + csize - n, usize - n});
                    span(usize - n, tail, h + 5, tail.length - h - 5);
                    found = true;
                    break;
                }
            }
            if (!found) throw new IOException("the root filesystem doesn't end the expected way");
        }

        /** Bytes a..b of the IPSW, clamped to the member: an inverted range makes Apple's server send the whole file. */
        private byte[] get(long a, long b) throws IOException {
            b = Math.min(b, start + csize - 1);
            if (a > b) return new byte[0];
            if (progress != null && progress.cancelled()) throw new Progress.Cancelled();
            HttpURLConnection c = Source.open(url, a, b);
            byte[] d;
            try {
                InputStream in = c.getInputStream();
                ByteArrayOutputStream out = new ByteArrayOutputStream((int) Math.min(b - a + 1, 1 << 24));
                byte[] buf = new byte[65536];
                int r;
                while ((r = in.read(buf)) > 0) out.write(buf, 0, r);
                in.close();
                d = out.toByteArray();
            } finally {
                c.disconnect();
            }
            fetched += d.length;
            return d;
        }

        private static byte[] inflate(Inflater inf, byte[] in, int o, int n) throws IOException {
            inf.setInput(in, o, n);
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[1 << 16];
            try {
                while (!inf.finished()) {
                    int r = inf.inflate(buf);
                    if (r == 0) break;  // needs more input (or a dictionary): all there is
                    out.write(buf, 0, r);
                }
            } catch (DataFormatException e) {
                throw new IOException("damaged firmware: " + e.getMessage());
            } finally {
                inf.end();
            }
            return out.toByteArray();
        }

        /** bisect.bisect_right over the spans' starts. */
        private int bisect(long at) {
            int lo = 0, hi = spans.size();
            while (lo < hi) {
                int mid = (lo + hi) >>> 1;
                if (at < spans.get(mid).at) hi = mid;
                else lo = mid + 1;
            }
            return lo;
        }

        private void span(long at, byte[] b, int o, int len) {
            spans.add(bisect(at), new Span(at, b, o, len));
        }

        private static int cmp(long[] a, long[] b) {
            return a[0] != b[0] ? Long.compare(a[0], b[0]) : Long.compare(a[1], b[1]);
        }

        private static void insort(List<long[]> l, long[] a) {
            int lo = 0, hi = l.size();
            while (lo < hi) {
                int mid = (lo + hi) >>> 1;
                if (cmp(a, l.get(mid)) < 0) hi = mid;
                else lo = mid + 1;
            }
            l.add(lo, a);
        }

        private void add(byte[] d, long dAt, List<int[]> c, long plain) {
            long[] a = {dAt + c.get(0)[0] + 5, plain};
            for (int[] x : c) {
                int h = x[0], n = x[1];
                span(plain, d, h + 5, Math.min(n, d.length - (h + 5)));  // the last may be cut short
                plain += n;
            }
            int[] last = c.get(c.size() - 1);
            runs.add(new long[] {plain, dAt + last[0] + 5 + last[1]});
            boolean have = false;
            for (long[] x : anchors) have |= x[0] == a[0] && x[1] == a[1];
            if (!have) insort(anchors, a);
        }

        /** The span holding off (the last one starting at or before it), or null. */
        private Span piece(long off) {
            int i = bisect(off) - 1;
            if (i < 0) return null;
            Span s = spans.get(i);
            return off < s.at + s.len ? s : null;
        }

        private byte[] gather(long off, int n) {
            byte[] out = new byte[n];
            int got = 0;
            while (got < n) {
                Span s = piece(off + got);
                if (s == null) return null;
                int at = (int) (off + got - s.at), take = Math.min(n - got, s.len - at);
                System.arraycopy(s.b, s.o + at, out, got, take);
                got += take;
            }
            return out;
        }

        private void fetch(long off, long n) throws IOException {
            for (int i = 0, count = runs.size(); i < count; i++) {
                long end = runs.get(i)[0], nxt = runs.get(i)[1];
                if (end != off) continue;
                // right after a placed stretch: the next header is known
                double ratio = (mean + 5) / mean;
                byte[] d = get(nxt, nxt + (long) (n * ratio) + 2 * BLOCK);
                List<int[]> c = chain(d, 0);
                if (!c.isEmpty()) {
                    add(d, nxt, c, off);
                    return;
                }
                if (d.length > 0 && start + csize - nxt < 1 << 20) {
                    // the member ends in compressed blocks: inflate them, with
                    // the 32 KB before as their dictionary
                    byte[] window = off >= 32768 ? gather(off - 32768, 32768) : null;
                    if (window != null) {
                        byte[] z = get(nxt, nxt + (1 << 20));
                        Inflater inf = new Inflater(true);
                        inf.setDictionary(window);
                        byte[] plain = inflate(inf, z, 0, z.length);
                        span(off, plain, 0, plain.length);
                        return;
                    }
                }
            }
            place(off, n);
        }

        private void place(long off, long n) throws IOException {
            long[] below = null, above = null;
            for (long[] a : anchors) {
                if (a[1] <= off) {
                    if (below == null || a[1] > below[1]) below = a;
                } else if (above == null || a[1] < above[1]) {
                    above = a;
                }
            }
            if (below == null) throw new IOException("nothing placed before " + off);
            long ca = below[0], pa = below[1];
            double nAb = 0, mean = this.mean;
            if (above != null) {
                long cb = above[0], pb = above[1];
                nAb = ((cb - ca) - (pb - pa)) / 5.0;
                if (nAb > 0) mean = (pb - pa) / nAb;
            }
            double ratio = (mean + 5) / mean;
            long est = ca + (long) ((off - pa) * ratio);
            long lo = Math.max(start, est - WINDOW);
            byte[] d = get(lo, est + (long) (n * ratio) + WINDOW);
            List<int[]> c = findChain(d, 3);
            if (c.isEmpty()) throw new IOException("no stored blocks near " + off);
            long h0 = lo + c.get(0)[0];
            double kf = (h0 + 5 - ca) / (mean + 5);  // headers from the anchor, estimated
            long k0 = (long) Math.rint(kf);  // Python's round: halves to even
            double nt = Math.abs(kf);
            double sigma = (nAb > nt ? Math.sqrt(nt * (1 - nt / nAb)) : Math.sqrt(nt) + 50) * 4.6 / mean;
            ByteArrayOutputStream bo = new ByteArrayOutputStream();
            for (int[] x : c) bo.write(d, x[0] + 5, Math.min(x[1], d.length - (x[0] + 5)));
            byte[] body = bo.toByteArray();
            Integer[] verdict = new Integer[9];  // k0 - 4 .. k0 + 4
            List<Long> good = new ArrayList<>();
            for (int i = 0; i < 9; i++) {
                long k = k0 - 4 + i;
                Integer v = check != null ? check.check(pa + (h0 + 5 - ca) - 5 * k, body) : null;
                verdict[i] = v;
                if (v != null && v == 0) good.add(k);
            }
            long k;
            if (sigma < 0.15) {  // the count is certain; the content mustn't disagree
                k = k0;
                if (verdict[4] != null && verdict[4] != 0 && !good.isEmpty()) {
                    throw new IOException("couldn't place the data near " + off);
                }
            } else if (good.size() == 1) {
                k = good.get(0);
            } else {
                throw new IOException("couldn't place the data near " + off);
            }
            add(d, lo, c, pa + (h0 + 5 - ca) - 5 * k);
        }

        @Override
        long size() {
            return size;
        }

        @Override
        void read(long off, byte[] b, int o, int n) throws IOException {
            if (off < 0) throw new EOFException("read before the start of the root filesystem");
            if (off + n > size) {  // as a Python read would, short: the rest reads as zeros
                int have = (int) Math.max(0, size - off);
                Arrays.fill(b, o + have, o + n, (byte) 0);
                n = have;
            }
            int got = 0, tries = 0;
            while (got < n) {
                Span s = piece(off + got);
                if (s != null) {
                    int at = (int) (off + got - s.at), take = Math.min(n - got, s.len - at);
                    System.arraycopy(s.b, s.o + at, b, o + got, take);
                    got += take;
                    continue;
                }
                if (++tries > 64) throw new IOException("couldn't read the firmware at " + (off + got));
                fetch(off + got, n - got);
            }
        }
    }

    /** check() for Member, from the encrypted image's structure (rangefs.py's _checker). */
    static final class Checker implements Check {
        private final Encrcdsa enc;
        private final long[] deflatedStarts;
        private final int bs;
        private final long data;

        Checker(Encrcdsa enc, long[] deflatedStarts) {
            this.enc = enc;
            this.deflatedStarts = deflatedStarts;
            bs = enc.blockSize();
            data = enc.dataOff();
        }

        private byte[] decrypt(byte[] body, long plain0, long n, int count) throws IOException {
            byte[] out = new byte[bs * count];
            int len = 0;
            for (long i = n; i < n + count; i++) {
                int o = (int) (data + i * bs - plain0);
                byte[] p = enc.decrypt(i, body, o, Math.min(bs, body.length - o));
                System.arraycopy(p, 0, out, len, p.length);
                len += p.length;
            }
            return len == out.length ? out : Arrays.copyOf(out, len);
        }

        /** bisect.bisect_left */
        private int bisectLeft(long x) {
            int lo = 0, hi = deflatedStarts.length;
            while (lo < hi) {
                int mid = (lo + hi) >>> 1;
                if (deflatedStarts[mid] < x) lo = mid + 1;
                else hi = mid;
            }
            return lo;
        }

        @Override
        public Integer check(long plain0, byte[] body) {
            try {
                long end = plain0 + body.length;
                // a deflated chunk of the disk image begins here: a zlib stream
                int i = bisectLeft(plain0 - data), j = bisectLeft(end - data - 2L * bs);
                int tried = 0, bad = 0;
                for (int x = i; x < Math.min(j, i + 16); x++) {
                    long st = deflatedStarts[x];
                    long n = st / bs;
                    if (data + n * bs < plain0) continue;
                    byte[] p = decrypt(body, plain0, n, 2);
                    int from = (int) (st % bs);
                    tried++;
                    if (!zlibStarts(p, from, Math.max(0, Math.min(bs, p.length - from)))) bad++;
                }
                if (tried > 0) return bad;
                // elsewhere: text, code and empty space decrypt to something that
                // isn't noise (a wrong candidate is off by 5-40 bytes, never a whole
                // number of AES blocks, so it decrypts to noise throughout)
                long first = Math.max(0, -Math.floorDiv(-(plain0 - data), bs)), last = Math.floorDiv(end - data, bs);
                long step = Math.max(1, Math.floorDiv(last - first, 96));
                for (long n = first; n < last; n += step) {
                    byte[] p = decrypt(body, plain0, n, 1);
                    if (entropy(p, 0, p.length) < 6.5) return 0;
                }
                return null;
            } catch (IOException e) {
                return null;
            }
        }

        /** zlib.decompressobj().decompress(b) doesn't fail. */
        private static boolean zlibStarts(byte[] b, int o, int n) {
            Inflater inf = new Inflater();
            try {
                inf.setInput(b, o, n);
                byte[] out = new byte[1 << 16];
                while (!inf.finished()) {
                    int r = inf.inflate(out);
                    if (r == 0) {
                        if (inf.needsDictionary()) return false;
                        break;
                    }
                }
                return true;
            } catch (DataFormatException e) {
                return false;
            } finally {
                inf.end();
            }
        }
    }

    /** A remote IPSW's root filesystem, and what reading it has fetched so far. */
    static final class Volume {
        final Hfs hfs;
        final Member member;

        Volume(Hfs hfs, Member member) {
            this.hfs = hfs;
            this.member = member;
        }
    }

    static Volume open(Firmware.Build b, Progress p) throws IOException {
        Source.Http ipsw = new Source.Http(b.url);
        Object[] e = Firmware.rootfsEntry(ipsw);
        String name = (String) e[0];
        int method = (Integer) e[1];
        long csize = (Long) e[2], lho = (Long) e[3], usize = (Long) e[4];
        if (method != 8) throw new IOException("the root filesystem isn't deflated");
        byte[] h = ipsw.read(lho, 30);
        Member m = new Member(b.url, lho + 30 + Source.le16(h, 26) + Source.le16(h, 28), csize, usize, p);
        if (p != null) p.step("Fetching the decryption key from The Apple Wiki");
        Encrcdsa enc = new Encrcdsa(m, Firmware.fetchKey(b.build, b.device, name));
        m.check = new Checker(enc, new long[0]);
        Udif udif = new Udif(enc);
        m.check = new Checker(enc, udif.deflatedStarts());
        return new Volume(new Hfs(udif, Hfs.findHfs(udif)), m);
    }
}
