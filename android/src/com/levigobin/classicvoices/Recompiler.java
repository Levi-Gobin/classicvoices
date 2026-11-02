package com.levigobin.classicvoices;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.nio.charset.Charset;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeSet;

/**
 * tools/recomp.py in Java, so the phone can make its own fast engine code
 * (there's no Python on Android). It must write exactly what recomp.py
 * writes; check with
 *
 *   javac -d /tmp/rc android/src/com/levigobin/classicvoices/Recompiler.java
 *   java -cp /tmp/rc com.levigobin.classicvoices.Recompiler voiced tools/profiles/ios6.txt out --prefix ios6
 *
 * and diff against recomp.py's output. Thumb-2 functions in the engine
 * become C that calls the interpreter's own instruction semantics with
 * constant encodings, which the compiler folds into native code.
 */
final class Recompiler {
    private static final Charset UTF8 = Charset.forName("UTF-8");

    private static final Map<String, String> DIV_HELPERS = new HashMap<>();
    static {
        DIV_HELPERS.put("___divsi3", "K.r[0] = rt_sdiv(K.r[0], K.r[1]);");
        DIV_HELPERS.put("___modsi3", "K.r[0] = rt_smod(K.r[0], K.r[1]);");
        DIV_HELPERS.put("___udivsi3", "K.r[0] = K.r[1] ? K.r[0] / K.r[1] : 0;");
        DIV_HELPERS.put("___umodsi3", "K.r[0] = K.r[1] ? K.r[0] % K.r[1] : 0;");
    }

    // Guest functions whose calls become host code (core/recomp_rt.h), by
    // symbol, in the builds whose code was checked against them (recomp.py
    // HOST_FUNCS / HOST_FUNC_BUILDS).
    private static final Map<String, String> HOST_FUNCS = new HashMap<>();
    static {
        HOST_FUNCS.put("_cstdlib_strlen", "rt_strlen(&K);");
        HOST_FUNCS.put("_cstdlib_strchr", "rt_strchr(&K);");
        HOST_FUNCS.put("_cstdlib_strcmp", "rt_strcmp(&K);");
        HOST_FUNCS.put("_cstdlib_strncmp", "rt_strncmp(&K);");
        HOST_FUNCS.put("_cstdlib_strstr", "rt_strstr(&K);");
        HOST_FUNCS.put("_cstdlib_memcpy", "rt_memcpy(&K);");
        HOST_FUNCS.put("_cstdlib_memmove", "rt_memmove(&K);");
        HOST_FUNCS.put("_log_HasTraceTuningDataSubscriber", "K.r[0] = 0;");
        HOST_FUNCS.put("_fxd_S32ShMultRndS32S32", "rt_fxd_mult_rnd(&K);");
    }
    private static final List<String> HOST_FUNC_BUILDS = Arrays.asList("ios70", "ios7");

    // While a stop is pending (Cpu.interrupt) these functions are answered by
    // host code instead of being called, in HOST_FUNC_BUILDS (recomp.py
    // INTERRUPT_FUNCS): mosyntknowl_Unif2 failing, so a long parse ends.
    private static final Map<String, String[]> INTERRUPT_FUNCS = new HashMap<>();
    static {
        INTERRUPT_FUNCS.put("_mosyntknowl_Unif2", new String[] {
            "{   // recomp.py INTERRUPT_FUNCS: mosyntknowl_Unif2 failing",
            "    uint32_t w = K.r[3];",
            "    if (w && (int32_t)g32(&K, w + 0x258) >= 1) {",
            "        uint32_t i = 0, v;",
            "        do { v = g32(&K, w + 0x25c + 4 * i); i++; s32(&K, w + 4 * v, v); } while ((int32_t)i < (int32_t)g32(&K, w + 0x258));",
            "    }",
            "    if (w) s32(&K, w + 0x258, 0);",
            "    s8(&K, g32(&K, K.r[13] + 0x18), 0);",
            "    K.r[0] = 0;",
            "}",
        });
    }

    // Functions whose leaf calls pass a copy of the registers, in
    // HOST_FUNC_BUILDS (recomp.py COPY_FUNCS).
    private static final List<String> COPY_FUNCS = Arrays.asList("_activeprompt_Consult");

    // Hot loops run as host code, by their exact instructions (recomp.py
    // HOST_LOOPS): chunks of {offset from the start}, then {hw1} or {hw1, hw2}
    // per instruction; "after": the code goes on after the first chunk, else
    // with the guest code at the start.
    private static final int[][][][] HOST_LOOP_PATTERNS = {
        {
            {{0x0}, {0xb2a3}, {0x005d}, {0xfa15, 0xf584}, {0xf852, 0x5035}, {0x2d22}, {0xd106}, {0xeb03, 0x0343}, {0xeb02, 0x03c3}, {0x68db}, {0x428b}, {0xd003}, {0x3401}, {0xb2a3}, {0x4283}, {0xd3ec}}
        },
        {
            {{0x0}, {0x4621}, {0x9818}, {0x9121}, {0xfa10, 0xf081}, {0xf8cd, 0x8088}, {0xb280}, {0x901a}, {0xb288}, {0x902e}, {0x2000}, {0x2800}, {0xf041, 0x8126}, {0x981b}, {0x9a1a}, {0x6800}, {0x4440}, {0x4282}, {0xf201, 0x82bd}, {0xf8db, 0x0000}, {0x2300}, {0x9a2e}, {0xf04f, 0x0b00}, {0x9132}, {0x5c80}, {0x2800}, {0xf001, 0x82d0}, {0x9d32}, {0x9330}, {0xe00a}},
            {{0x5e}, {0x9836}, {0xb2ae}, {0x4286}, {0xf080, 0x8095}, {0xf64f, 0x70ff}, {0xea1b, 0x0f00}, {0xd018}, {0x1c70}, {0x9936}, {0x4288}, {0xd214}, {0x9934}, {0x6809}, {0x5d8a}, {0x2a20}, {0xd10f}, {0xf241, 0x0848}, {0x180c}, {0xf2c0, 0x0814}, {0x44f8}, {0x4640}, {0xf765, 0xf8c1}, {0x4602}, {0x4620}, {0x4641}, {0xf765, 0xf8de}, {0x2800}, {0xd0d1}, {0x46a8}, {0x68bd}, {0xf64f, 0x70ff}, {0xea18, 0x0f00}, {0xd011}, {0x9834}, {0x882c}, {0x6800}, {0x4430}, {0xf910, 0x1c01}, {0xf64c, 0x2016}, {0xf2c0, 0x0014}, {0x4478}, {0xf765, 0xf87e}, {0xb918}, {0xf8cd, 0x80c8}, {0x882a}, {0xe01e}, {0x8828}, {0x2200}, {0x2400}, {0xf8cd, 0x80c8}, {0xb1c0}},
            {{0x114}, {0xf8dd, 0x80d4}, {0xb2a3}, {0x4293}, {0xd218}},
            {{0x150}, {0x9b30}, {0x9824}, {0xb299}, {0x4281}, {0xd21a}, {0x9834}, {0x2400}, {0xf8d8, 0x201c}, {0x3301}, {0x6800}, {0x5c51}, {0x5d80}, {0x4281}, {0xbf08}, {0x2401}, {0x9a32}, {0x4281}, {0xf102, 0x0201}, {0x9232}, {0xf43f, 0xaf62}, {0x9832}, {0x4646}, {0x992e}, {0xf8dd, 0xb0d0}, {0xb280}, {0x1a40}, {0x901e}, {0xe1c5}},
            {{0x51c}, {0x9831}, {0x6900}, {0xf761, 0xfef0}, {0x2801}, {0xd174}},
            {{0x612}, {0x2c01}, {0xf040, 0x84d3}},
            {{0xfbe}, {0xf8dd, 0x8088}, {0x4621}, {0x4608}, {0x9921}, {0xf64b, 0x350a}, {0x2801}, {0xf2c0, 0x0514}, {0x447d}, {0xf000, 0x8149}, {0x2800}, {0xf47f, 0xa81d}, {0xe126}},
            {{0x122e}, {0xf8db, 0x0000}, {0x460c}, {0xb2a1}, {0x5c40}, {0xb140}, {0xb241}, {0x4628}, {0xf763, 0xffc3}, {0x1c61}, {0x2800}, {0xd0f2}, {0xe000}, {0x3401}, {0xf8db, 0x0000}, {0xb2a1}, {0x5c40}, {0x2800}, {0xb241}, {0xf43e, 0xaed2}, {0x4628}, {0xf763, 0xffb3}, {0x2800}, {0xf43e, 0xaecc}, {0xe7ef}}
        }
        ,
        {   // the parser's hash chain (recomp.py HOST_LOOPS)
            {{-0x4}, {0xf8db, 0xb000}},
            {{0x0}, {0xf1bb, 0x0f00}, {0xd04b}, {0x9825}, {0xf9bb, 0x1010}, {0x4281}, {0xd1f5}}
        }
    };
    private static final boolean[] HOST_LOOP_AFTER = {true, false, false};
    // where the code may go: offsets from the start that must be labels
    private static final int[][] HOST_LOOP_EXITS = {{}, {}, {0x9e, 0x10}};

    static String[] hostLoopCode(int p, long a) {
        switch (p) {
        case 0: return new String[] {
            "{   // recomp.py HOST_LOOPS: the first type-" + "0x22" + " record at position r1",
            "    const uint32_t n = K.r[0], want = K.r[1], base = K.r[2];",
            "    uint32_t i = K.r[4], t = K.r[5], h, x, y;",
            "    for (;;) {",
            "        h = i & " + "0xffff" + "u;",
            "        t = g32(&K, base + h * 24u);",
            "        if (t == " + "0x22" + "u) { h = g32(&K, base + h * 24u + 12u); if (h == want) { x = h; y = want; break; } }",
            "        i++;",
            "        h = i & " + "0xffff" + "u;",
            "        if (h >= n) { x = h; y = n; break; }",
            "    }",
            "    K.r[3] = h; K.r[4] = i; K.r[5] = t;",
            "    { uint32_t d = x - y; K.n = d >> 31; K.z = d == 0; K.c = x >= y; K.v = ((x ^ y) & (x ^ d)) >> 31; }",
            "}",
        };
        case 1: return new String[] {
            "{   // recomp.py HOST_LOOPS: a phrase against each word, until a character differs",
            "    const uint32_t sp = K.r[13], cntp = g32(&K, K.r[7] + 8), D = " + hx(a + 0x14cae0) + ";",
            "    uint32_t r4 = K.r[4], r8 = K.r[8], r11 = K.r[11], lr = K.r[14];",
            "    for (;;) {",
            "        const uint32_t pos = r4, p16 = pos & " + "0xffff" + "u, t16 = (g32(&K, sp + " + "0x60" + ") + p16) & " + "0xffff" + "u;",
            "        s32(&K, sp + " + "0x84" + ", pos); s32(&K, sp + " + "0x88" + ", r8); s32(&K, sp + " + "0x68" + ", t16); s32(&K, sp + " + "0xb8" + ", p16);",
            "        if (t16 > g32(&K, g32(&K, sp + " + "0x6c" + ")) + r8) break;",
            "        const uint32_t t0 = g32(&K, r11);",
            "        s32(&K, sp + " + "0xc8" + ", pos);",
            "        if (!g8(&K, t0 + p16)) break;",
            "        uint32_t k = 0, cur = pos, prompt;",
            "        int other = 0;",
            "        for (;; k++, cur++) {  // compare k, at text position cur",
            "            s32(&K, sp + " + "0xc0" + ", k);",
            "            const uint32_t c16 = cur & " + "0xffff" + "u;",
            "            if (c16 >= g32(&K, sp + " + "0xd8" + ")) { other = 1; break; }",
            "            const uint32_t text = g32(&K, g32(&K, sp + " + "0xd0" + "));",
            "            int scan = 1;",
            "            if (c16) { lr = " + hx((a + 0xcc) | 1) + "; scan = rt_in_str(&K, D, (uint8_t)g8(&K, text + c16 - 1)); }",
            "            if (scan) {",
            "                const uint32_t n = g16(&K, cntp);",
            "                if (n) {",
            "                    const uint32_t base = g32(&K, g32(&K, sp + " + "0xcc" + ")), want = g32(&K, sp + " + "0xb4" + ") + c16;",
            "                    uint32_t i = 0;",
            "                    while (i < n && !(g32(&K, base + i * 24u) == " + "0x22" + "u && g32(&K, base + i * 24u + 12u) == want)) i++;",
            "                    if (i < n) { other = 1; break; }",
            "                }",
            "            }",
            "            prompt = g32(&K, sp + " + "0xd4" + ");",
            "            if ((k & " + "0xffff" + "u) >= g32(&K, sp + " + "0x90" + ")) { other = 1; break; }",
            "            const uint32_t pc = g8(&K, g32(&K, prompt + " + "0x1c" + ") + (k & " + "0xffff" + "u)), tc = g8(&K, text + c16);",
            "            s32(&K, sp + " + "0xc8" + ", cur + 1);",
            "            if (pc != tc) break;",
            "        }",
            "        if (other) break;",
            "        s32(&K, sp + " + "0x78" + ", ((cur + 1) & " + "0xffff" + "u) - p16);",
            "        r11 = g32(&K, sp + " + "0xd0" + ");",
            "        g32(&K, g32(&K, sp + " + "0xc4" + ") + " + "0x10" + "); lr = " + hx((a + 0x524) | 1) + ";",
            "        const uint32_t tx = g32(&K, r11);",
            "        uint32_t b;",
            "        for (;;) { b = g8(&K, tx + (r4 & " + "0xffff" + "u)); if (!b) break; lr = " + hx((a + 0x1242) | 1) + "; if (rt_in_str(&K, D, (uint8_t)b)) break; r4++; }",
            "        for (;;) { b = g8(&K, tx + (r4 & " + "0xffff" + "u)); if (!b) break; lr = " + hx((a + 0x1262) | 1) + "; if (!rt_in_str(&K, D, (uint8_t)b)) break; r4++; }",
            "        K.r[0] = 0; K.r[1] = (uint32_t)(int32_t)(int8_t)b; K.r[2] = cur + 1; K.r[3] = k + 1; K.r[4] = r4;",
            "        K.r[5] = D; K.r[6] = prompt; K.r[8] = r8; K.r[11] = r11; K.r[14] = lr;",
            "        K.n = 0; K.z = 1; K.c = 1; K.v = 0;",
            "    }",
            "}",
        };
        case 2: return new String[] {
            "{   // recomp.py HOST_LOOPS: the parser's hash chain, to the entry with a key",
            "    uint32_t n = K.r[11];",
            "    for (;;) {",
            "        if (!n) { K.r[11] = 0; K.n = 0; K.z = 1; K.c = 1; K.v = 0; goto L_" + Long.toHexString(a + 0x9e) + "; }",
            "        const uint32_t key = g32(&K, K.r[13] + 0x94), f = (uint32_t)(int32_t)(int16_t)g16(&K, n + 0x10);",
            "        K.r[0] = key; K.r[1] = f;",
            "        if (f == key) { K.r[11] = n; K.n = 0; K.z = 1; K.c = 1; K.v = 0; goto L_" + Long.toHexString(a + 0x10) + "; }",
            "        n = g32(&K, n);",
            "    }",
            "}",
        };
        default: throw new IllegalArgumentException();
        }
    }

    /** Index into HOST_LOOP_* of the loop at a, where it goes on in end[0] (-1: the guest code at a); -1 if none (recomp.py host_loop_at). */
    static int hostLoopAt(Function fn, long a, Set<Long> labelset, long[] end) {
        for (int p = 0; p < HOST_LOOP_PATTERNS.length; p++) {
            long first = -1;
            boolean ok = true;
            for (int[][] chunk : HOST_LOOP_PATTERNS[p]) {
                long at = a + chunk[0][0];
                for (int q = 1; q < chunk.length && ok; q++) {
                    int[] hw = chunk[q];
                    Insn ins = fn.insns.get(at);
                    if (ins == null || ins.hw1 != hw[0] || ins.size != (hw.length == 2 ? 4 : 2)
                            || (hw.length == 2 && ins.hw2 != hw[1])) { ok = false; break; }
                    at += ins.size;
                }
                if (!ok) break;
                if (first < 0) first = at;
            }
            if (!ok) continue;
            for (int e : HOST_LOOP_EXITS[p]) if (!labelset.contains(a + e)) { ok = false; break; }
            if (!ok) continue;
            if (!HOST_LOOP_AFTER[p]) { end[0] = -1; return p; }
            if (labelset.contains(first)) { end[0] = first; return p; }
        }
        return -1;
    }

    private Recompiler() {}

    static final class BadAddress extends Exception {
        BadAddress(long a) { super(Long.toHexString(a)); }
    }

    static final class MachO {
        final byte[] d;
        final List<long[]> segs = new ArrayList<>();  // vmaddr, fileoff, filesize
        final TreeSet<Long> syms = new TreeSet<>();
        final List<Object[]> symNames = new ArrayList<>();  // (Long addr, String name), sorted like recomp.py's syms
        long textLo, textHi;
        boolean hasText;
        final Map<Long, String> stubNames = new HashMap<>();
        long stubLo, stubHi;

        MachO(byte[] d) throws IOException {
            this.d = d;
            if (u32(0) != 0xFEEDFACEL) throw new IOException("not a 32-bit Mach-O engine");
            long ncmds = u32(16);
            long[] stubs = null;
            int off = 28;
            long symoff = 0, nsyms = 0, stroff = 0, indoff = 0;
            for (long n = 0; n < ncmds; n++) {
                long cmd = u32(off), size = u32(off + 4);
                if (cmd == 1) {
                    long vmaddr = u32(off + 24), fileoff = u32(off + 32), filesize = u32(off + 36);
                    long nsects = u32(off + 48);
                    segs.add(new long[] {vmaddr, fileoff, filesize});
                    for (int s = 0; s < nsects; s++) {
                        int so = off + 56 + 68 * s;
                        int e = so;
                        while (e < so + 16 && d[e] != 0) e++;
                        String sect = new String(d, so, e - so, UTF8);
                        long addr = u32(so + 32), ssize = u32(so + 36), flags = u32(so + 56);
                        long r1 = u32(so + 60), r2 = u32(so + 64);
                        if (sect.equals("__text")) {
                            textLo = addr;
                            textHi = addr + ssize;
                            hasText = true;
                        }
                        if ((flags & 0xFF) == 8) stubs = new long[] {addr, ssize, r1, r2};
                    }
                } else if (cmd == 2) {
                    symoff = u32(off + 8);
                    nsyms = u32(off + 12);
                    stroff = u32(off + 16);
                } else if (cmd == 0xB) {
                    indoff = u32(off + 56);
                }
                off += (int) size;
            }
            List<String> names = new ArrayList<>();
            for (int i = 0; i < nsyms; i++) {
                int so = (int) (symoff + 12 * i);
                long strx = u32(so);
                int ntype = d[so + 4] & 0xFF, nsect = d[so + 5] & 0xFF;
                long value = u32(so + 8);
                int st = (int) (stroff + strx), end = st;
                while (d[end] != 0) end++;
                String nm = new String(d, st, end - st, Charset.forName("ISO-8859-1"));
                names.add(nm);
                if ((ntype & 0x0E) == 0x0E && nsect != 0 && hasText && textLo <= value && value < textHi) {
                    syms.add(value & ~1L);
                    symNames.add(new Object[] {value & ~1L, nm});
                }
            }
            Collections.sort(symNames, new java.util.Comparator<Object[]>() {
                @Override
                public int compare(Object[] x, Object[] y) {
                    int c = Long.compare((Long) x[0], (Long) y[0]);
                    return c != 0 ? c : ((String) x[1]).compareTo((String) y[1]);
                }
            });
            if (stubs != null) {
                long addr = stubs[0], ssize = stubs[1], r1 = stubs[2], r2 = stubs[3];
                for (long k = 0; k < ssize / r2; k++) {
                    long ind = u32((int) (indoff + 4 * (r1 + k)));
                    if ((ind & 0xC0000000L) == 0) stubNames.put(addr + k * r2, names.get((int) ind));
                }
                stubLo = stubs[0];
                stubHi = stubs[0] + stubs[1];
            }
        }

        long u32(int o) {
            return (d[o] & 0xFFL) | (d[o + 1] & 0xFFL) << 8 | (d[o + 2] & 0xFFL) << 16 | (d[o + 3] & 0xFFL) << 24;
        }

        /** File offset of n bytes at addr. */
        int at(long addr, int n) throws BadAddress {
            for (long[] s : segs) {
                if (s[0] <= addr && addr + n <= s[0] + s[2]) return (int) (s[1] + addr - s[0]);
            }
            throw new BadAddress(addr);
        }

        int h16(long addr) throws BadAddress {
            int o = at(addr, 2);
            return (d[o] & 0xFF) | (d[o + 1] & 0xFF) << 8;
        }

        int u8(long addr) throws BadAddress {
            return d[at(addr, 1)] & 0xFF;
        }
    }

    static long sext(long v, int bits) {
        long m = 1L << (bits - 1);
        return (v ^ m) - m;
    }

    /** BL targets in __text, as llvm-objdump's linear sweep sees them (recomp.py scan_bl). */
    static Set<Long> scanBl(MachO mo) throws BadAddress {
        long lo = mo.textLo, hi = mo.textHi;
        int base = mo.at(lo, (int) (hi - lo));
        byte[] data = mo.d;
        Set<Long> out = new HashSet<>();
        int a = 0, n = (int) (hi - lo), itLeft = 0;
        while (a + 2 <= n) {
            int hw1 = (data[base + a] & 0xFF) | (data[base + a + 1] & 0xFF) << 8;
            boolean inIt = itLeft > 0;
            if (inIt) itLeft--;
            if ((hw1 & 0xFF00) == 0xBF00 && (hw1 & 0xF) != 0) {
                int mask = hw1 & 0xF;
                itLeft = ((hw1 >> 4) & 0xF) == 0xE ? 0 : 5 - (32 - Integer.numberOfLeadingZeros(mask & -mask));
                a += 2;
                continue;
            }
            int top = hw1 >> 11;
            if ((top == 0x1D || top == 0x1E || top == 0x1F) && a + 4 <= n) {
                int hw2 = (data[base + a + 2] & 0xFF) | (data[base + a + 3] & 0xFF) << 8;
                if (top == 0x1E && (hw2 & 0xD000) == 0xD000 && !inIt) {
                    int s = (hw1 >> 10) & 1;
                    int i1 = 1 ^ (((hw2 >> 13) & 1) ^ s), i2 = 1 ^ (((hw2 >> 11) & 1) ^ s);
                    long t = lo + a + 4 + sext((long) s << 24 | (long) i1 << 23 | (long) i2 << 22
                            | (long) (hw1 & 0x3FF) << 12 | (long) (hw2 & 0x7FF) << 1, 25);
                    if (lo <= t && t < hi) out.add(t);
                }
                a += 4;
            } else {
                a += 2;
            }
        }
        return out;
    }

    static final class Insn {
        final long addr;
        final int hw1, hw2, size;
        String kind = "op";  // op | it | b | bcond | bl | blx_stub | icall | ijump | stop | tb
        Long target;
        Integer cond;        // IT condition (null = always)
        int inIt;
        String stub;

        Insn(long addr, int hw1, int hw2, int size) {
            this.addr = addr;
            this.hw1 = hw1;
            this.hw2 = hw2;
            this.size = size;
        }
    }

    static Insn decode(MachO mo, long addr) throws BadAddress {
        int hw1 = mo.h16(addr);
        if ((hw1 >> 11) >= 0x1D) {
            int hw2 = mo.h16(addr + 2);
            Insn ins = new Insn(addr, hw1, hw2, 4);
            if ((hw1 & 0xF800) == 0xF000 && (hw2 & 0x8000) != 0) {
                long s = (hw1 >> 10) & 1, j1 = (hw2 >> 13) & 1, j2 = (hw2 >> 11) & 1;
                int kind = hw2 & 0x5000;
                if (kind == 0) {
                    int cond = (hw1 >> 6) & 15;
                    if ((cond & 0xE) == 0xE) return ins;  // misc control (hints, barriers)
                    long imm = (s << 20) | (j2 << 19) | (j1 << 18) | ((long) (hw1 & 0x3F) << 12) | ((long) (hw2 & 0x7FF) << 1);
                    ins.kind = "bcond";
                    ins.target = (addr + 4 + sext(imm, 21)) & 0xFFFFFFFFL;
                    return ins;
                }
                long i1 = 1 - (j1 ^ s), i2 = 1 - (j2 ^ s);
                long imm = (s << 24) | (i1 << 23) | (i2 << 22) | ((long) (hw1 & 0x3FF) << 12) | ((long) (hw2 & 0x7FF) << 1);
                long off = sext(imm, 25);
                if (kind == 0x1000) {
                    ins.kind = "b";
                    ins.target = (addr + 4 + off) & 0xFFFFFFFFL;
                } else if (kind == 0x5000) {
                    ins.kind = "bl";
                    ins.target = (addr + 4 + off) & 0xFFFFFFFFL;
                } else {
                    long t = (((addr + 4) & ~3L) + off) & 0xFFFFFFFFL;
                    if (mo.stubLo <= t && t < mo.stubHi) {
                        ins.kind = "blx_stub";
                        ins.target = t;
                        ins.stub = mo.stubNames.get(t);
                    } else {
                        ins.kind = "icall";  // BLX to ARM code: let the dispatcher handle it
                    }
                }
                return ins;
            }
            if ((hw1 & 0xFE40) == 0xE800 && (hw1 & 0x10) != 0 && (hw2 & 0x8000) != 0) {
                ins.kind = "ijump";  // LDM with PC
            } else if ((hw1 & 0xFFF0) == 0xE8D0 && (hw2 & 0xFFE0) == 0xF000) {
                ins.kind = "tb";
            } else if ((hw1 & 0xFE00) == 0xF800 && (hw1 & 0x10) != 0 && ((hw1 >> 5) & 3) == 2 && (hw2 >> 12) == 15) {
                ins.kind = "ijump";  // LDR PC
            } else if ((hw1 & 0xFE00) == 0xEA00 && ((hw2 >> 8) & 15) == 15 && (hw1 & 0x10) == 0) {
                ins.kind = "ijump";  // data-processing into PC
            }
            return ins;
        }
        Insn ins = new Insn(addr, hw1, 0, 2);
        if ((hw1 & 0xF000) == 0xD000) {
            int cond = (hw1 >> 8) & 15;
            if (cond >= 14) {
                ins.kind = "stop";
            } else {
                ins.kind = "bcond";
                ins.target = (addr + 4 + sext((hw1 & 0xFF) << 1, 9)) & 0xFFFFFFFFL;
            }
        } else if ((hw1 & 0xF800) == 0xE000) {
            ins.kind = "b";
            ins.target = (addr + 4 + sext((hw1 & 0x7FF) << 1, 12)) & 0xFFFFFFFFL;
        } else if ((hw1 & 0xF500) == 0xB100) {
            int off = (((hw1 >> 9) & 1) << 6) | (((hw1 >> 3) & 31) << 1);
            ins.kind = "bcond";
            ins.target = addr + 4 + off;
        } else if ((hw1 & 0xFF00) == 0x4700) {
            ins.kind = (hw1 & 0x80) != 0 ? "icall" : "ijump";
        } else if ((hw1 & 0xFF00) == 0xBD00) {
            ins.kind = "ijump";
        } else if ((hw1 & 0xFC00) == 0x4400 && (((hw1 >> 8) & 3) == 0 || ((hw1 >> 8) & 3) == 2)
                && ((((hw1 >> 4) & 8) | (hw1 & 7)) == 15)) {
            ins.kind = "ijump";
        } else if ((hw1 & 0xFF00) == 0xBF00 && (hw1 & 0xF) != 0) {
            ins.kind = "it";
        } else if ((hw1 & 0xFF00) == 0xBE00) {
            ins.kind = "stop";
        }
        return ins;
    }

    /** A TBB/TBH table that directly follows the instruction. */
    static List<Long> tbTargets(MachO mo, Insn ins) throws BadAddress {
        long base = ins.addr + 4;
        boolean half = (ins.hw2 & 0x10) != 0;
        List<Long> targets = new ArrayList<>();
        if ((ins.hw1 & 15) != 15) return targets;  // table not PC-relative
        long pos = base;
        Long end = null;
        while (end == null || pos < end) {
            int e;
            if (half) {
                e = mo.h16(pos);
                pos += 2;
            } else {
                e = mo.u8(pos);
                pos += 1;
            }
            long t = base + 2L * e;
            targets.add(t);
            end = end == null ? t : Math.min(end, t);
            if (targets.size() > 4096) break;
        }
        return targets;
    }

    static final class Function {
        final long start, end;
        final Map<Long, Insn> insns = new HashMap<>();
        final Set<Long> roots = new HashSet<>(), branchTargets = new HashSet<>(), returnSites = new HashSet<>();

        Function(long start, long end) {
            this.start = start;
            this.end = end;
        }
    }

    static void explore(MachO mo, Function fn) throws BadAddress {
        List<Long> work = new ArrayList<>(new TreeSet<>(fn.roots));
        Map<Long, int[]> itCond = new HashMap<>();  // addr -> (cond, in_it)
        while (!work.isEmpty()) {
            long a = work.remove(work.size() - 1);
            if (fn.insns.containsKey(a) || !(fn.start <= a && a < fn.end)) continue;
            Insn ins;
            try {
                ins = decode(mo, a);
            } catch (BadAddress e) {
                continue;
            }
            int[] ic = itCond.get(a);
            if (ic != null) {
                ins.cond = ic[0];
                ins.inIt = ic[1];
            }
            fn.insns.put(a, ins);
            long nxt = a + ins.size;
            if (ins.kind.equals("it")) {
                int state = ins.hw1 & 0xFF;
                long p = nxt;
                while ((state & 0xF) != 0) {
                    itCond.put(p, new int[] {state >> 4, 1});
                    state = (state & 7) != 0 ? ((state & 0xE0) | ((state << 1) & 0x1F)) : 0;
                    try {
                        p += decode(mo, p).size;
                    } catch (BadAddress e) {
                        break;
                    }
                }
            }
            boolean conditional = ins.cond != null && ins.cond != 14;
            if ((ins.kind.equals("b") || ins.kind.equals("bcond")) && ins.target != null) {
                fn.branchTargets.add(ins.target);
                if (fn.start <= ins.target && ins.target < fn.end) work.add(ins.target);
            }
            if (ins.kind.equals("tb")) {
                for (long t : tbTargets(mo, ins)) {
                    fn.branchTargets.add(t);
                    if (fn.start <= t && t < fn.end) work.add(t);
                }
            }
            if (ins.kind.equals("bl") || ins.kind.equals("blx_stub") || ins.kind.equals("icall")) fn.returnSites.add(nxt);
            boolean terminator = isTerminatorKind(ins.kind) && !conditional;
            if (!terminator) work.add(nxt);
        }
    }

    static boolean isTerminatorKind(String k) {
        return k.equals("b") || k.equals("ijump") || k.equals("tb") || k.equals("stop");
    }

    static String hx(long v) { return "0x" + Long.toHexString(v); }

    static String hx4(int v) { return String.format("0x%04x", v); }

    static String fname(String prefix, long start) { return String.format("nf_%s_%06x", prefix, start); }

    /** VFP/NEON (coprocessor) encodings, and the NEON loads and stores (recomp.py uses_vfp). */
    static boolean usesVfp(Insn ins) {
        if (ins.size != 4) return false;
        return (ins.hw1 & 0xEC00) == 0xEC00 || (ins.hw1 & 0xFF10) == 0xF900;
    }

    private static String exitTo(long t, String indent, Set<Long> labelset, Map<Long, String> entriesOf, String store) {
        if (labelset.contains(t)) return indent + "goto L_" + Long.toHexString(t) + ";";
        if (entriesOf.containsKey(t))
            return indent + "{ " + store + " c->k.r[15] = " + hx(t) + "; c->k.thumb = 1; return " + entriesOf.get(t)
                    + "(c, " + hx(t) + "); }";
        return indent + "{ " + store + " c->k.r[15] = " + hx(t) + "; c->k.thumb = 1; return 1; }";
    }

    static List<Long> emitFunction(Function fn, Map<Long, String> entriesOf, List<String> w, String prefix,
                                   Map<Long, String> hostAt, Map<Long, String> leafOf,
                                   Map<Long, String[]> interruptAt, Set<Long> copyFuncs, Set<Long> nocopyFuncs) {
        String name = fname(prefix, fn.start);
        boolean copyHere = copyFuncs.contains(fn.start);
        boolean icallCopy = ICALL_COPY_BUILDS.contains(prefix) && !nocopyFuncs.contains(fn.start);
        List<Long> labels = new ArrayList<>(fn.insns.keySet());
        Collections.sort(labels);
        Set<Long> labelset = fn.insns.keySet();
        TreeSet<Long> rs = new TreeSet<>(fn.roots);
        rs.addAll(fn.returnSites);
        rs.addAll(fn.branchTargets);
        rs.retainAll(labelset);
        List<Long> resume = new ArrayList<>(rs);
        // Without VFP/NEON instructions the function only ever touches the
        // integer registers: copy just those (recomp_rt.h RT_IN / RT_OUT).
        boolean vfp = false;
        for (Insn ins : fn.insns.values()) if (usesVfp(ins)) { vfp = true; break; }
        String load = vfp ? "K = c->k;" : "RT_IN(K, c);", store = vfp ? "c->k = K;" : "RT_OUT(c, K);";
        w.add("int " + name + "(Cpu *c, uint32_t start) {");
        w.add(vfp ? "    Core K = c->k;" : "    Core K; RT_IN(K, c);");
        w.add("    const uint32_t entry_lr = K.r[14] & ~1u, entry_sp = K.r[13];");
        w.add("    int rc;");
        w.add("    switch (start) {");
        for (long a : resume) w.add("    case " + hx(a) + ": goto L_" + Long.toHexString(a) + ";");
        w.add("    default: c->k.r[15] = start; return 1;");
        w.add("    }");
        for (int i = 0; i < labels.size(); i++) {
            long a = labels.get(i);
            Insn ins = fn.insns.get(a);
            long nxt = a + ins.size;
            w.add("L_" + Long.toHexString(a) + ":");
            long[] loopEnd = new long[1];
            int loop = hostLoopAt(fn, a, labelset, loopEnd);
            if (loop >= 0) {
                for (String line : hostLoopCode(loop, a)) w.add("    " + line);
                if (loopEnd[0] >= 0) {
                    w.add("    goto L_" + Long.toHexString(loopEnd[0]) + ";");
                    continue;
                }
            }
            boolean always = ins.cond == null || ins.cond == 14;
            if (!ins.kind.equals("it")) {
                String ind = "    ";
                List<String> body = new ArrayList<>();
                if (ins.kind.equals("bl") && interruptAt.containsKey(ins.target) && always) {
                    w.add("    if (c->interrupt) { K.r[14] = " + hx(nxt | 1) + ";");
                    for (String line : interruptAt.get(ins.target)) w.add("        " + line);
                    w.add("        goto L_" + Long.toHexString(nxt) + "; }");
                }
                if (ins.kind.equals("bl") && hostAt.containsKey(ins.target)) {
                    body.add("K.r[14] = " + hx(nxt | 1) + ";");
                    body.add(hostAt.get(ins.target));
                } else if (ins.kind.equals("bl") && leafOf.containsKey(ins.target)) {
                    body.add("K.r[14] = " + hx(nxt | 1) + ";");
                    if (copyHere && !leafOf.get(ins.target).startsWith("lfi_")) {
                        body.add("{ Core T; memcpy(&T, &K, RT_INT_PART); int lr = " + leafOf.get(ins.target) + "(&T); memcpy(&K, &T, RT_INT_PART);");
                        body.add("  if (lr) { " + store + " c->k.npc = K.r[15]; rc = rt_call(c, " + hx(nxt | 1) + "); " + load
                                + " if (rc) return 1; } }");
                    } else {
                        body.add("if (" + leafOf.get(ins.target) + "(&K)) { " + store + " c->k.npc = K.r[15]; rc = rt_call(c, "
                                + hx(nxt | 1) + "); " + load + " if (rc) return 1; }");
                    }
                    body.add("if (K.r[15] != " + hx(nxt) + ") { K.npc = K.r[15]; goto dyn; }");
                } else if (ins.kind.equals("bl")) {
                    long t = ins.target;
                    body.add("K.r[14] = " + hx(nxt | 1) + ";");
                    body.add(store);
                    if (entriesOf.containsKey(t)) {
                        body.add("rc = " + entriesOf.get(t) + "(c, " + hx(t) + ");");
                    } else {
                        body.add("c->k.npc = " + hx(t) + "; c->k.thumb = 1; rc = rt_call(c, " + hx(nxt | 1) + ");");
                    }
                    body.add(load + " if (rc) return 1;");
                    body.add("if (K.r[15] != " + hx(nxt) + ") { K.npc = K.r[15]; goto dyn; }");
                } else if (ins.kind.equals("blx_stub")) {
                    body.add("K.r[14] = " + hx(nxt | 1) + ";");
                    String helper = ins.stub != null ? DIV_HELPERS.get(ins.stub) : null;
                    if (helper != null) {
                        body.add(helper);
                    } else {
                        body.add(store + " rc = rt_import(c, " + hx(ins.target) + ", " + hx(nxt | 1) + "); " + load + " if (rc) return 1;");
                        body.add("if (K.r[15] != " + hx(nxt) + ") { K.npc = K.r[15]; goto dyn; }");
                    }
                } else if (ins.kind.equals("icall")) {
                    body.add("K.r[15] = " + hx(a + 4) + "; K.npc = " + hx(nxt) + ";");
                    body.add(ins.size == 2
                            ? "exec16(&K, " + hx4(ins.hw1) + ", " + hx(a) + ", " + ins.inIt + ");"
                            : "exec32(&K, " + hx4(ins.hw1) + ", " + hx4(ins.hw2) + ", " + hx(a) + ");");
                    if (!leafOf.isEmpty()) {
                        // a leaf target runs on these registers; anything else through the dispatcher
                        body.add("{ LeafFn lf = K.thumb ? " + prefix + "_leaf(K.npc) : 0;");
                        if (icallCopy || copyHere) {
                            body.add("  if (lf) { K.r[15] = K.npc; Core T; memcpy(&T, &K, RT_INT_PART); int lr = lf(&T); memcpy(&K, &T, RT_INT_PART);");
                            body.add("    if (lr) { " + store + " c->k.npc = K.r[15]; rc = rt_call(c, " + hx(nxt | 1) + "); " + load
                                    + " if (rc) return 1; } }");
                        } else {
                            body.add("  if (lf) { K.r[15] = K.npc; if (lf(&K)) { " + store + " c->k.npc = K.r[15]; rc = rt_call(c, "
                                    + hx(nxt | 1) + "); " + load + " if (rc) return 1; } }");
                        }
                        body.add("  else { " + store + " rc = rt_icall(c, " + hx(nxt | 1) + "); " + load + " if (rc) return 1; } }");
                    } else {
                        body.add(store + " rc = rt_icall(c, " + hx(nxt | 1) + "); " + load + " if (rc) return 1;");
                    }
                    body.add("if (K.r[15] != " + hx(nxt) + ") { K.npc = K.r[15]; goto dyn; }");
                } else {
                    body.add("K.r[15] = " + hx(a + 4) + "; K.npc = " + hx(nxt) + ";");
                    if (ins.size == 2) {
                        body.add("exec16(&K, " + hx4(ins.hw1) + ", " + hx(a) + ", " + ins.inIt + ");");
                    } else {
                        body.add("exec32(&K, " + hx4(ins.hw1) + ", " + hx4(ins.hw2) + ", " + hx(a) + ");");
                    }
                }
                // PC state must be set even when an IT condition skips the instruction
                w.add("    K.r[15] = " + hx(a + 4) + "; K.npc = " + hx(nxt) + ";");
                List<String> kept = new ArrayList<>();
                for (String b : body) if (!b.startsWith("K.r[15] = ")) kept.add(b);
                if (!always) {
                    w.add("    if (cond_pass(&K, " + ins.cond + ")) {");
                    for (String b : kept) w.add("        " + b);
                    w.add("    }");
                } else {
                    for (String b : kept) w.add(ind + b);
                }
                // control flow after the instruction
                String k = ins.kind;
                if (k.equals("b") || k.equals("bcond")) {
                    boolean plain = k.equals("b") && always;
                    if (!plain) {
                        w.add("    if (K.npc != " + hx(nxt) + ") " + exitTo(ins.target, "", labelset, entriesOf, store).trim());
                    } else {
                        w.add(exitTo(ins.target, "    ", labelset, entriesOf, store));
                    }
                } else if ((k.equals("ijump") || k.equals("tb")) && always) {
                    // Unconditional indirect jump: always dispatch on the target, even
                    // when it happens to be the next address (e.g. jump table entry 0).
                    w.add("    goto dyn;");
                } else if (k.equals("ijump") || k.equals("tb") || k.equals("op")) {
                    w.add("    if (K.npc != " + hx(nxt) + ") goto dyn;");
                } else if (k.equals("stop")) {
                    w.add("    " + store + " c->k.r[15] = " + hx(a) + "; return 1;");
                }
            }
            boolean terminator = isTerminatorKind(ins.kind) && always;
            if (!terminator) {
                Long following = i + 1 < labels.size() ? labels.get(i + 1) : null;
                if (following == null || following != nxt) w.add(exitTo(nxt, "    ", labelset, entriesOf, store));
            }
        }
        w.add("dyn: {");
        w.add("    uint32_t t = K.npc;");
        w.add("    if (K.thumb && t == entry_lr && K.r[13] == entry_sp) { " + store + " c->k.r[15] = t; return 0; }");
        w.add("    if (K.thumb) switch (t) {");
        for (long a : resume) w.add("    case " + hx(a) + ": goto L_" + Long.toHexString(a) + ";");
        w.add("    default: break;");
        w.add("    }");
        w.add("    " + store + " c->k.r[15] = t; return 1;");
        w.add("}");
        w.add("}");
        return resume;
    }

    static String lname(String prefix, long root) { return String.format("lf_%s_%06x", prefix, root); }

    static String liname(String prefix, long root) { return String.format("lfi_%s_%06x", prefix, root); }

    /**
     * Leaves of at most this many instructions are also written as
     * always-inlined functions that direct calls use (recomp.py TINY_LEAF).
     */
    static final int TINY_LEAF = 160;

    /** Builds whose indirect calls to leaves pass a copy of the registers (recomp.py ICALL_COPY_BUILDS). */
    static final java.util.List<String> ICALL_COPY_BUILDS =
            java.util.Arrays.asList("ios3", "ios4", "ios5", "ios6", "ios70", "ios7", "ios10");

    /**
     * The instructions reachable from root if they make a leaf: no calls, no
     * branches out of the function, no VFP/NEON, at most limit of them; else
     * null (recomp.py leaf_region). Its exits are returns, checked at run time.
     */
    static Set<Long> leafRegion(Function fn, long root, int limit) {
        Set<Long> seen = new HashSet<>();
        List<Long> work = new ArrayList<>();
        work.add(root);
        while (!work.isEmpty()) {
            long a = work.remove(work.size() - 1);
            if (seen.contains(a)) continue;
            Insn ins = fn.insns.get(a);
            if (ins == null || ins.kind.equals("bl") || ins.kind.equals("blx_stub") || ins.kind.equals("icall")
                    || ins.kind.equals("stop") || usesVfp(ins)) return null;
            seen.add(a);
            if (seen.size() > limit) return null;
            long nxt = a + ins.size;
            boolean conditional = ins.cond != null && ins.cond != 14;
            if (ins.kind.equals("b") || ins.kind.equals("bcond")) {
                if (!(fn.start <= ins.target && ins.target < fn.end)) return null;
                work.add(ins.target);
            }
            if (ins.kind.equals("tb")) return null;  // its targets aren't kept per instruction
            if (!((ins.kind.equals("b") || ins.kind.equals("ijump")) && !conditional)) work.add(nxt);
        }
        return seen;
    }

    /**
     * lf_<prefix>_<root>(Core *): the leaf on the caller's registers, with no
     * copies in or out (recomp.py emit_leaf). 0 when it returned, 1 when it
     * left any other way (K->r[15] = where; the caller hands over).
     */
    static void emitLeaf(Function fn, long root, Set<Long> region, List<String> w, String prefix, boolean inline) {
        List<Long> labels = new ArrayList<>(region);
        Collections.sort(labels);
        w.add(inline ? "static inline __attribute__((always_inline)) int " + liname(prefix, root) + "(Core *Kp) {"
                     : "int " + lname(prefix, root) + "(Core *Kp) {");
        w.add("#define K (*Kp)");
        w.add("    const uint32_t entry_lr = K.r[14] & ~1u, entry_sp = K.r[13];");
        w.add("    goto L_" + Long.toHexString(root) + ";");
        for (int i = 0; i < labels.size(); i++) {
            long a = labels.get(i);
            Insn ins = fn.insns.get(a);
            long nxt = a + ins.size;
            boolean always = ins.cond == null || ins.cond == 14;
            w.add("L_" + Long.toHexString(a) + ":");
            if (!ins.kind.equals("it")) {
                w.add("    K.r[15] = " + hx(a + 4) + "; K.npc = " + hx(nxt) + ";");
                String ex = ins.size == 2
                        ? "exec16(&K, " + hx4(ins.hw1) + ", " + hx(a) + ", " + ins.inIt + ");"
                        : "exec32(&K, " + hx4(ins.hw1) + ", " + hx4(ins.hw2) + ", " + hx(a) + ");";
                if (!always) w.add("    if (cond_pass(&K, " + ins.cond + ")) { " + ex + " }");
                else w.add("    " + ex);
                if (ins.kind.equals("b") || ins.kind.equals("bcond")) {
                    String guard = ins.kind.equals("b") && always ? "" : "if (K.npc != " + hx(nxt) + ") ";
                    w.add("    " + guard + "goto L_" + Long.toHexString(ins.target) + ";");
                } else if (ins.kind.equals("ijump") && always) {
                    w.add("    goto dyn;");
                } else {
                    w.add("    if (K.npc != " + hx(nxt) + ") goto dyn;");
                }
            }
            boolean terminator = (ins.kind.equals("b") || ins.kind.equals("ijump")) && always;
            if (!terminator) {
                Long following = i + 1 < labels.size() ? labels.get(i + 1) : null;
                if (following == null || following != nxt) w.add("    goto L_" + Long.toHexString(nxt) + ";");
            }
        }
        w.add("dyn:");
        w.add("    K.r[15] = K.npc;");
        w.add("    return !(K.thumb && K.npc == entry_lr && K.r[13] == entry_sp);");
        w.add("#undef K");
        w.add("}");
    }

    /** One hex address per line, # comments (tools/profiles/*.txt). */
    static TreeSet<Long> readProfile(InputStream in) throws IOException {
        TreeSet<Long> seen = new TreeSet<>();
        BufferedReader r = new BufferedReader(new InputStreamReader(in, UTF8));
        for (String l; (l = r.readLine()) != null; ) {
            int h = l.indexOf('#');
            String s = (h >= 0 ? l.substring(0, h) : l).trim();
            if (s.isEmpty()) continue;
            if (s.startsWith("0x") || s.startsWith("0X")) s = s.substring(2);
            seen.add(Long.parseLong(s, 16));
        }
        return seen;
    }

    /** Recompiles engine into outdir as <prefix>_NNN.c and <prefix>_table.c; returns the files. */
    static List<File> run(byte[] engine, InputStream profile, File outdir, String prefix, int perFile, boolean scanBl)
            throws IOException {
        try {
            return generate(engine, profile, outdir, prefix, perFile, scanBl);
        } catch (BadAddress e) {
            throw new IOException("the engine has no code at 0x" + e.getMessage());
        }
    }

    private static List<File> generate(byte[] engine, InputStream profile, File outdir, String prefix, int perFile,
                                       boolean scanBl) throws IOException, BadAddress {
        MachO mo = new MachO(engine);
        Map<Long, String> hostAt = new HashMap<>();  // guest address -> host code, for this build
        Map<Long, String[]> interruptAt = new HashMap<>();  // guest address -> host code while a stop is pending
        Set<Long> copyFuncs = new HashSet<>();  // COPY_FUNCS' addresses
        if (HOST_FUNC_BUILDS.contains(prefix)) {
            for (Object[] sym : mo.symNames) {
                String code = HOST_FUNCS.get((String) sym[1]);
                if (code != null) hostAt.put((Long) sym[0], code);
                String[] lines = INTERRUPT_FUNCS.get((String) sym[1]);
                if (lines != null) interruptAt.put((Long) sym[0], lines);
                if (COPY_FUNCS.contains((String) sym[1])) copyFuncs.add((Long) sym[0]);
            }
        }
        Set<Long> nocopyFuncs = new HashSet<>();  // unit selection's (recomp.py nocopy_funcs)
        for (Object[] sym : mo.symNames) if (((String) sym[1]).startsWith("_uselect_")) nocopyFuncs.add((Long) sym[0]);
        List<Long> starts = new ArrayList<>();
        for (long x : readProfile(profile)) if (mo.textLo <= x && x < mo.textHi) starts.add(x);

        TreeSet<Long> boundSet = new TreeSet<>(mo.syms);
        boundSet.add(mo.textLo);
        if (scanBl) boundSet.addAll(scanBl(mo));
        long[] bounds = new long[boundSet.size()];
        int bi = 0;
        for (long b : boundSet) bounds[bi++] = b;

        Map<Long, Function> funcs = new LinkedHashMap<>();
        for (long s : starts) {
            int i = Arrays.binarySearch(bounds, s);
            if (i < 0) i = -i - 2;  // the last bound <= s
            long lo = bounds[i], hi = i + 1 < bounds.length ? bounds[i + 1] : mo.textHi;
            Function f = funcs.get(lo);
            if (f == null) funcs.put(lo, f = new Function(lo, hi));
            f.roots.add(s);
        }
        for (Function f : funcs.values()) {
            if (boundSet.contains(f.start)) f.roots.add(f.start);
            explore(mo, f);
            f.roots.retainAll(f.insns.keySet());
        }

        Map<Long, String> entriesOf = new HashMap<>();
        for (Function f : funcs.values()) {
            Set<Long> all = new HashSet<>(f.roots);
            all.addAll(f.branchTargets);
            all.addAll(f.returnSites);
            for (long r : all) if (f.insns.containsKey(r)) entriesOf.put(r, fname(prefix, f.start));
        }

        // Leaves (leafRegion): the targets of direct calls, and entry points no
        // other code of their function reaches (what the dispatcher was given,
        // e.g. the unit-selection distance functions called through pointers).
        Map<Long, String> leafOf = new HashMap<>();  // guest address -> its leaf function's name
        List<Function> leafFns = new ArrayList<>();
        List<Long> leafRoots = new ArrayList<>();
        List<Set<Long>> leafRegions = new ArrayList<>();
        Set<Long> blTargets = new HashSet<>();
        for (Function f : funcs.values())
            for (Insn ins : f.insns.values()) if (ins.kind.equals("bl")) blTargets.add(ins.target);
        for (Function f : funcs.values()) {
            Set<Long> reached = new HashSet<>();
            for (Insn ins : f.insns.values()) {
                if ((ins.kind.equals("b") || ins.kind.equals("bcond")) && ins.target != null) reached.add(ins.target);
                if (!(isTerminatorKind(ins.kind) && (ins.cond == null || ins.cond == 14))) reached.add(ins.addr + ins.size);
            }
            TreeSet<Long> cand = new TreeSet<>(f.roots);
            cand.removeAll(reached);
            cand.add(f.start);
            for (long t : blTargets) if (f.insns.containsKey(t)) cand.add(t);
            for (long e : cand) {
                if (hostAt.containsKey(e)) continue;
                Set<Long> region = leafRegion(f, e, 96);
                if (region != null && !region.isEmpty()) {
                    leafOf.put(e, region.size() <= TINY_LEAF ? liname(prefix, e) : lname(prefix, e));
                    leafFns.add(f);
                    leafRoots.add(e);
                    leafRegions.add(region);
                }
            }
        }

        //noinspection ResultOfMethodCallIgnored
        outdir.mkdirs();
        List<Function> order = new ArrayList<>(funcs.values());
        Collections.sort(order, new java.util.Comparator<Function>() {
            @Override
            public int compare(Function x, Function y) {
                return Long.compare(x.start, y.start);
            }
        });
        List<String> header = Arrays.asList(
                "// Generated by tools/recomp.py — do not edit.",
                "#define VX_OPS_INLINE static inline __attribute__((always_inline))",
                "#include \"thumb.h\"",
                "#include \"recomp_rt.h\"",
                "");
        StringBuilder decl = new StringBuilder();
        for (Function f : order) {
            if (decl.length() > 0) decl.append('\n');
            decl.append("int ").append(fname(prefix, f.start)).append("(Cpu *c, uint32_t start);");
        }
        List<String> leafDecls = new ArrayList<>();
        for (long e : leafRoots) leafDecls.add("int " + lname(prefix, e) + "(Core *Kp);");
        if (!leafRoots.isEmpty()) {
            leafDecls.add("static inline LeafFn " + prefix + "_leaf(uint32_t t) {");
            leafDecls.add("    switch (t) {");
            for (long e : leafRoots) leafDecls.add("    case " + hx(e) + ": return " + lname(prefix, e) + ";");
            leafDecls.add("    default: return 0;");
            leafDecls.add("    }");
            leafDecls.add("}");
        }
        for (int li = 0; li < leafRoots.size(); li++)
            if (leafRegions.get(li).size() <= TINY_LEAF)
                emitLeaf(leafFns.get(li), leafRoots.get(li), leafRegions.get(li), leafDecls, prefix, true);
        for (String l : leafDecls) {
            if (decl.length() > 0) decl.append('\n');
            decl.append(l);
        }
        List<long[]> table = new ArrayList<>();
        List<String> tableNames = new ArrayList<>();
        List<File> written = new ArrayList<>();
        List<List<String>> files = new ArrayList<>();
        List<String> cur = new ArrayList<>();
        int curN = 0;
        for (Function f : order) {
            List<Long> resume = emitFunction(f, entriesOf, cur, prefix, hostAt, leafOf, interruptAt, copyFuncs, nocopyFuncs);
            for (long r : resume) {
                table.add(new long[] {r, tableNames.size()});
                tableNames.add(fname(prefix, f.start));
            }
            cur.add("");
            curN += f.insns.size();
            if (curN >= perFile) {
                files.add(cur);
                cur = new ArrayList<>();
                curN = 0;
            }
        }
        for (int li = 0; li < leafRoots.size(); li++) {
            long e = leafRoots.get(li);
            if (leafRegions.get(li).size() <= TINY_LEAF)
                cur.add("int " + lname(prefix, e) + "(Core *Kp) { return " + liname(prefix, e) + "(Kp); }");
            else
                emitLeaf(leafFns.get(li), e, leafRegions.get(li), cur, prefix, false);
            cur.add("");
            curN += leafRegions.get(li).size();
            if (curN >= perFile) {
                files.add(cur);
                cur = new ArrayList<>();
                curN = 0;
            }
        }
        if (!cur.isEmpty()) files.add(cur);
        for (int i = 0; i < files.size(); i++)
            written.add(writeFile(new File(outdir, String.format("%s_%03d.c", prefix, i)), header, decl, files.get(i), null));
        Collections.sort(table, new java.util.Comparator<long[]>() {
            @Override
            public int compare(long[] x, long[] y) {
                return Long.compare(x[0], y[0]);
            }
        });
        List<String> t = new ArrayList<>();
        t.add("const NativeEntry vx_native_" + prefix + "[] = {");
        for (long[] e : table) t.add("    {" + hx(e[0]) + ", " + tableNames.get((int) e[1]) + "},");
        t.add("};");
        t.add("const size_t vx_native_" + prefix + "_count = " + table.size() + ";");
        t.add("");
        written.add(writeFile(new File(outdir, prefix + "_table.c"), header, decl, null, t));
        return written;
    }

    private static File writeFile(File f, List<String> header, CharSequence decl, List<String> body, List<String> tail)
            throws IOException {
        try (Writer o = new OutputStreamWriter(new FileOutputStream(f), UTF8)) {
            List<String> lines = new ArrayList<>(header);
            lines.add(decl.toString());
            lines.add("");
            if (body != null) lines.addAll(body);
            if (tail != null) lines.addAll(tail);
            for (int i = 0; i < lines.size(); i++) {
                if (i > 0) o.write('\n');
                o.write(lines.get(i));
            }
        }
        return f;
    }

    static byte[] readAll(File f) throws IOException {
        byte[] b = new byte[(int) f.length()];
        try (InputStream in = new FileInputStream(f)) {
            int n = 0;
            while (n < b.length) {
                int r = in.read(b, n, b.length - n);
                if (r < 0) throw new IOException("short read: " + f);
                n += r;
            }
        }
        return b;
    }

    /** Recompiler voiced profile.txt outdir [--prefix ios6] [--per-file N] [--scan-bl] */
    public static void main(String[] args) throws IOException {
        String prefix = "ios6";
        int perFile = 4000;
        boolean scan = false;
        List<String> pos = new ArrayList<>();
        for (int i = 0; i < args.length; i++) {
            if (args[i].equals("--prefix")) prefix = args[++i];
            else if (args[i].equals("--per-file")) perFile = Integer.parseInt(args[++i]);
            else if (args[i].equals("--scan-bl")) scan = true;
            else pos.add(args[i]);
        }
        try (InputStream p = new FileInputStream(pos.get(1))) {
            List<File> out = run(readAll(new File(pos.get(0))), p, new File(pos.get(2)), prefix, perFile, scan);
            System.out.println(out.size() + " files");
        }
    }
}
