package com.levigobin.classicvoices;

import java.io.ByteArrayOutputStream;
import java.io.IOException;

/**
 * A bzip2 decoder, for the Mac OS X installers' Essentials.pkg (MacCompacts):
 * Android has none. Blocks are decoded as bzip2 does (Huffman-coded MTF/RLE2
 * symbols, the inverse Burrows-Wheeler transform, then RLE1); the CRCs aren't
 * checked, as every voice is checked by its SHA-256 afterwards.
 */
final class Bzip2 {
    private Bzip2() {}

    private static final long BLOCK = 0x314159265359L, END = 0x177245385090L;

    private static final class Bits {
        final byte[] b;
        long pos;  // in bits
        Bits(byte[] b) { this.b = b; }

        int bit() throws IOException {
            if ((pos >> 3) >= b.length) throw new IOException("the bzip2 data ended early");
            int v = (b[(int) (pos >> 3)] >> (7 - (int) (pos & 7))) & 1;
            pos++;
            return v;
        }

        long bits(int n) throws IOException {
            long v = 0;
            for (int i = 0; i < n; i++) v = (v << 1) | bit();
            return v;
        }
    }

    static byte[] decompress(byte[] in) throws IOException {
        if (in.length < 4 || in[0] != 'B' || in[1] != 'Z' || in[2] != 'h' || in[3] < '1' || in[3] > '9')
            throw new IOException("not bzip2 data");
        int max = (in[3] - '0') * 100000;
        Bits r = new Bits(in);
        r.pos = 32;
        ByteArrayOutputStream out = new ByteArrayOutputStream(in.length * 4);
        int[] tt = new int[max];
        byte[] ll = new byte[max];
        while (true) {
            long magic = r.bits(48);
            if (magic == END) break;
            if (magic != BLOCK) throw new IOException("bad bzip2 block");
            r.bits(32);  // block CRC
            if (r.bit() != 0) throw new IOException("randomised bzip2 blocks aren't supported");
            int origPtr = (int) r.bits(24);
            int n = block(r, ll, max);
            if (origPtr >= n) throw new IOException("bad bzip2 block");
            // inverse BWT
            int[] cf = new int[257];
            for (int i = 0; i < n; i++) cf[(ll[i] & 0xFF) + 1]++;
            for (int i = 1; i < 257; i++) cf[i] += cf[i - 1];
            for (int i = 0; i < n; i++) tt[cf[ll[i] & 0xFF]++] = i;
            // RLE1: four equal bytes are followed by a count of more
            byte[] blockOut = new byte[n * 2 + 256];
            int o = 0, run = 0, last = -1;
            int p = tt[origPtr];
            for (int k = 0; k < n; k++) {
                int ch = ll[p] & 0xFF;
                p = tt[p];
                if (run == 4) {
                    if (o + ch > blockOut.length) blockOut = java.util.Arrays.copyOf(blockOut, (o + ch) * 2);
                    for (int i = 0; i < ch; i++) blockOut[o++] = (byte) last;
                    run = 0;
                    last = -1;
                    continue;
                }
                if (ch == last) run++;
                else { last = ch; run = 1; }
                if (o >= blockOut.length) blockOut = java.util.Arrays.copyOf(blockOut, blockOut.length * 2);
                blockOut[o++] = (byte) ch;
            }
            out.write(blockOut, 0, o);
        }
        return out.toByteArray();
    }

    /** One block's symbols into ll (before the inverse BWT); returns its length. */
    private static int block(Bits r, byte[] ll, int max) throws IOException {
        // the bytes in use
        int inUse16 = (int) r.bits(16);
        int[] seqToUnseq = new int[256];
        int nInUse = 0;
        for (int i = 0; i < 16; i++) {
            if ((inUse16 & (0x8000 >> i)) == 0) continue;
            int m = (int) r.bits(16);
            for (int j = 0; j < 16; j++) if ((m & (0x8000 >> j)) != 0) seqToUnseq[nInUse++] = i * 16 + j;
        }
        if (nInUse == 0) throw new IOException("bad bzip2 block");
        int alpha = nInUse + 2;
        int nGroups = (int) r.bits(3);
        int nSelectors = (int) r.bits(15);
        if (nGroups < 2 || nGroups > 6 || nSelectors < 1) throw new IOException("bad bzip2 block");
        int[] mtfG = new int[nGroups];
        for (int i = 0; i < nGroups; i++) mtfG[i] = i;
        int[] selectors = new int[nSelectors];
        for (int i = 0; i < nSelectors; i++) {
            int j = 0;
            while (r.bit() == 1) if (++j >= nGroups) throw new IOException("bad bzip2 block");
            int v = mtfG[j];
            System.arraycopy(mtfG, 0, mtfG, 1, j);
            mtfG[0] = v;
            selectors[i] = v;
        }
        // canonical Huffman tables, one per group
        int[][] perm = new int[nGroups][alpha], first = new int[nGroups][22], firstIdx = new int[nGroups][22],
                count = new int[nGroups][22];
        for (int t = 0; t < nGroups; t++) {
            int[] len = new int[alpha];
            int curr = (int) r.bits(5);
            for (int i = 0; i < alpha; i++) {
                while (true) {
                    if (curr < 1 || curr > 20) throw new IOException("bad bzip2 block");
                    if (r.bit() == 0) break;
                    curr += r.bit() == 0 ? 1 : -1;
                }
                len[i] = curr;
                count[t][curr]++;
            }
            int code = 0, idx = 0;
            for (int l = 1; l <= 20; l++) {
                first[t][l] = code;
                firstIdx[t][l] = idx;
                for (int i = 0; i < alpha; i++) if (len[i] == l) perm[t][idx++] = i;
                code = (code + count[t][l]) << 1;
            }
        }
        // the symbols: RUNA/RUNB runs, move-to-front indexes, end of block
        int[] mtf = new int[256];
        for (int i = 0; i < 256; i++) mtf[i] = i;
        int n = 0, group = -1, left = 0, runLen = 0, runBit = 1;
        int eob = nInUse + 1;
        while (true) {
            if (left == 0) {
                if (++group >= nSelectors) throw new IOException("bad bzip2 block");
                left = 50;
            }
            left--;
            int t = selectors[group];
            int c = 0, sym = -1;
            for (int l = 1; l <= 20; l++) {
                c = (c << 1) | r.bit();
                int k = c - first[t][l];
                if (k >= 0 && k < count[t][l]) { sym = perm[t][firstIdx[t][l] + k]; break; }
            }
            if (sym < 0) throw new IOException("bad bzip2 block");
            if (sym <= 1) {  // RUNA, RUNB
                runLen += (sym + 1) * runBit;
                runBit <<= 1;
                continue;
            }
            if (runLen > 0) {
                if (n + runLen > max) throw new IOException("bad bzip2 block");
                byte b = (byte) seqToUnseq[mtf[0]];
                for (int i = 0; i < runLen; i++) ll[n++] = b;
                runLen = 0;
                runBit = 1;
            }
            if (sym == eob) break;
            int j = sym - 1;
            int v = mtf[j];
            System.arraycopy(mtf, 0, mtf, 1, j);
            mtf[0] = v;
            if (n >= max) throw new IOException("bad bzip2 block");
            ll[n++] = (byte) seqToUnseq[v];
        }
        return n;
    }
}
