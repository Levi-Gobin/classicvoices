package com.levigobin.classicvoices;

import android.util.Base64;

import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.zip.DataFormatException;
import java.util.zip.Inflater;

/**
 * A read-only, random-access view of a compressed (UDIF) disk image, after
 * applereaders.py's UDIFReader: the last 512 bytes are a koly trailer naming
 * an XML plist whose blkx tables say where each run of sectors comes from
 * (stored, deflated, or all zero). iOS root filesystems are this inside the
 * encryption; bzip2 and ADC chunks don't occur in them.
 */
final class Udif extends Source {
    private static final long ZERO = 0, RAW = 1, IGNORE = 2, ZLIB = 0x80000005L, COMMENT = 0x7FFFFFFEL,
            TERMINATOR = 0xFFFFFFFFL;

    private static final class Chunk {
        long kind, out, length, src, srcLen;
    }

    private final Source f;
    private final List<Chunk> chunks = new ArrayList<>();
    private final long size;
    private int cachedIndex = -1;
    private byte[] cached;

    static boolean is(Source s) throws IOException {
        long n = s.size();
        if (n < 512) return false;
        byte[] k = s.read(n - 512, 4);
        return k[0] == 'k' && k[1] == 'o' && k[2] == 'l' && k[3] == 'y';
    }

    Udif(Source f) throws IOException {
        this.f = f;
        long n = f.size();
        byte[] koly = f.read(n - 512, 512);
        long fork = be64(koly, 24);
        long xmlOff = be64(koly, 216), xmlLen = be64(koly, 224);
        if (xmlLen <= 0 || xmlLen > (64 << 20)) throw new IOException("the disk image has no block table");
        String plist = new String(f.read(xmlOff, (int) xmlLen), "UTF-8");
        Matcher m = Pattern.compile("<data>([^<]*)</data>").matcher(plist);
        while (m.find()) {
            byte[] blob = Base64.decode(m.group(1).replaceAll("\\s", ""), Base64.DEFAULT);
            if (blob.length < 204 || blob[0] != 'm' || blob[1] != 'i' || blob[2] != 's' || blob[3] != 'h') continue;
            long start = be64(blob, 8);
            long count = be32(blob, 200);
            for (int i = 0; i < count; i++) {
                int r = 204 + 40 * i;
                if (r + 40 > blob.length) break;
                long kind = be32(blob, r);
                if (kind == COMMENT || kind == TERMINATOR) continue;
                Chunk c = new Chunk();
                c.kind = kind;
                c.out = (start + be64(blob, r + 8)) * 512;
                c.length = be64(blob, r + 16) * 512;
                c.src = be64(blob, r + 24) + fork;
                c.srcLen = be64(blob, r + 32);
                chunks.add(c);
            }
        }
        Collections.sort(chunks, new Comparator<Chunk>() {
            @Override
            public int compare(Chunk a, Chunk b) {
                return Long.compare(a.out, b.out);
            }
        });
        long end = 0;
        for (Chunk c : chunks) end = Math.max(end, c.out + c.length);
        size = end;
    }

    @Override
    long size() {
        return size;
    }

    private int find(long off) {
        int lo = 0, hi = chunks.size() - 1;
        while (lo <= hi) {
            int mid = (lo + hi) >>> 1;
            Chunk c = chunks.get(mid);
            if (off < c.out) hi = mid - 1;
            else if (off >= c.out + c.length) lo = mid + 1;
            else return mid;
        }
        return -1;
    }

    private byte[] bytes(int i) throws IOException {
        if (i == cachedIndex) return cached;
        Chunk c = chunks.get(i);
        byte[] data;
        if (c.length > Integer.MAX_VALUE) throw new IOException("disk image chunk too large");
        if (c.kind == ZERO || c.kind == IGNORE) {
            data = new byte[(int) c.length];
        } else if (c.kind == RAW) {
            data = Arrays.copyOf(f.read(c.src, (int) c.srcLen), (int) c.length);
        } else if (c.kind == ZLIB) {
            Inflater inf = new Inflater();
            inf.setInput(f.read(c.src, (int) c.srcLen));
            data = new byte[(int) c.length];
            try {
                int got = 0;
                while (got < data.length && !inf.finished()) {
                    int r = inf.inflate(data, got, data.length - got);
                    if (r == 0 && (inf.needsInput() || inf.needsDictionary())) break;
                    got += r;
                }
            } catch (DataFormatException e) {
                throw new IOException("damaged disk image: " + e.getMessage());
            } finally {
                inf.end();
            }
        } else {
            throw new IOException(String.format("unsupported disk image chunk type %08x", c.kind));
        }
        cachedIndex = i;
        cached = data;
        return data;
    }

    @Override
    void read(long off, byte[] b, int o, int n) throws IOException {
        while (n > 0) {
            int i = find(off);
            if (i < 0) {  // a hole between chunks reads as zeros
                Arrays.fill(b, o, o + n, (byte) 0);
                return;
            }
            Chunk c = chunks.get(i);
            byte[] d = bytes(i);
            int at = (int) (off - c.out);
            int take = (int) Math.min(n, c.length - at);
            System.arraycopy(d, at, b, o, take);
            off += take;
            o += take;
            n -= take;
        }
    }
}
