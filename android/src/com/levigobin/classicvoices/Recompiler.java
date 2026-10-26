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

    private Recompiler() {}

    static final class BadAddress extends Exception {
        BadAddress(long a) { super(Long.toHexString(a)); }
    }

    static final class MachO {
        final byte[] d;
        final List<long[]> segs = new ArrayList<>();  // vmaddr, fileoff, filesize
        final TreeSet<Long> syms = new TreeSet<>();
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
                names.add(new String(d, st, end - st, Charset.forName("ISO-8859-1")));
                if ((ntype & 0x0E) == 0x0E && nsect != 0 && hasText && textLo <= value && value < textHi)
                    syms.add(value & ~1L);
            }
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

    private static String exitTo(long t, String indent, Set<Long> labelset, Map<Long, String> entriesOf) {
        if (labelset.contains(t)) return indent + "goto L_" + Long.toHexString(t) + ";";
        if (entriesOf.containsKey(t))
            return indent + "{ c->k = K; c->k.r[15] = " + hx(t) + "; c->k.thumb = 1; return " + entriesOf.get(t)
                    + "(c, " + hx(t) + "); }";
        return indent + "{ c->k = K; c->k.r[15] = " + hx(t) + "; c->k.thumb = 1; return 1; }";
    }

    static List<Long> emitFunction(Function fn, Map<Long, String> entriesOf, List<String> w, String prefix) {
        String name = fname(prefix, fn.start);
        List<Long> labels = new ArrayList<>(fn.insns.keySet());
        Collections.sort(labels);
        Set<Long> labelset = fn.insns.keySet();
        TreeSet<Long> rs = new TreeSet<>(fn.roots);
        rs.addAll(fn.returnSites);
        rs.addAll(fn.branchTargets);
        rs.retainAll(labelset);
        List<Long> resume = new ArrayList<>(rs);
        w.add("int " + name + "(Cpu *c, uint32_t start) {");
        w.add("    Core K = c->k;");
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
            boolean always = ins.cond == null || ins.cond == 14;
            if (!ins.kind.equals("it")) {
                String ind = "    ";
                List<String> body = new ArrayList<>();
                if (ins.kind.equals("bl")) {
                    long t = ins.target;
                    body.add("K.r[14] = " + hx(nxt | 1) + ";");
                    body.add("c->k = K;");
                    if (entriesOf.containsKey(t)) {
                        body.add("rc = " + entriesOf.get(t) + "(c, " + hx(t) + ");");
                    } else {
                        body.add("c->k.npc = " + hx(t) + "; c->k.thumb = 1; rc = rt_call(c, " + hx(nxt | 1) + ");");
                    }
                    body.add("K = c->k; if (rc) return 1;");
                    body.add("if (K.r[15] != " + hx(nxt) + ") { K.npc = K.r[15]; goto dyn; }");
                } else if (ins.kind.equals("blx_stub")) {
                    body.add("K.r[14] = " + hx(nxt | 1) + ";");
                    String helper = ins.stub != null ? DIV_HELPERS.get(ins.stub) : null;
                    if (helper != null) {
                        body.add(helper);
                    } else {
                        body.add("c->k = K; rc = rt_import(c, " + hx(ins.target) + ", " + hx(nxt | 1) + "); K = c->k; if (rc) return 1;");
                        body.add("if (K.r[15] != " + hx(nxt) + ") { K.npc = K.r[15]; goto dyn; }");
                    }
                } else if (ins.kind.equals("icall")) {
                    body.add("K.r[15] = " + hx(a + 4) + "; K.npc = " + hx(nxt) + ";");
                    body.add(ins.size == 2
                            ? "exec16(&K, " + hx4(ins.hw1) + ", " + hx(a) + ", " + ins.inIt + ");"
                            : "exec32(&K, " + hx4(ins.hw1) + ", " + hx4(ins.hw2) + ", " + hx(a) + ");");
                    body.add("c->k = K; rc = rt_call(c, " + hx(nxt | 1) + "); K = c->k; if (rc) return 1;");
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
                        w.add("    if (K.npc != " + hx(nxt) + ") " + exitTo(ins.target, "", labelset, entriesOf).trim());
                    } else {
                        w.add(exitTo(ins.target, "    ", labelset, entriesOf));
                    }
                } else if ((k.equals("ijump") || k.equals("tb")) && always) {
                    // Unconditional indirect jump: always dispatch on the target, even
                    // when it happens to be the next address (e.g. jump table entry 0).
                    w.add("    goto dyn;");
                } else if (k.equals("ijump") || k.equals("tb") || k.equals("op")) {
                    w.add("    if (K.npc != " + hx(nxt) + ") goto dyn;");
                } else if (k.equals("stop")) {
                    w.add("    c->k = K; c->k.r[15] = " + hx(a) + "; return 1;");
                }
            }
            boolean terminator = isTerminatorKind(ins.kind) && always;
            if (!terminator) {
                Long following = i + 1 < labels.size() ? labels.get(i + 1) : null;
                if (following == null || following != nxt) w.add(exitTo(nxt, "    ", labelset, entriesOf));
            }
        }
        w.add("dyn: {");
        w.add("    uint32_t t = K.npc;");
        w.add("    if (K.thumb && t == entry_lr && K.r[13] == entry_sp) { c->k = K; c->k.r[15] = t; return 0; }");
        w.add("    if (K.thumb) switch (t) {");
        for (long a : resume) w.add("    case " + hx(a) + ": goto L_" + Long.toHexString(a) + ";");
        w.add("    default: break;");
        w.add("    }");
        w.add("    c->k = K; c->k.r[15] = t; return 1;");
        w.add("}");
        w.add("}");
        return resume;
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
        List<long[]> table = new ArrayList<>();
        List<String> tableNames = new ArrayList<>();
        List<File> written = new ArrayList<>();
        List<String> cur = new ArrayList<>();
        int curN = 0, nfile = 0;
        for (int fi = 0; fi < order.size(); fi++) {
            Function f = order.get(fi);
            List<Long> resume = emitFunction(f, entriesOf, cur, prefix);
            for (long r : resume) {
                table.add(new long[] {r, tableNames.size()});
                tableNames.add(fname(prefix, f.start));
            }
            cur.add("");
            curN += f.insns.size();
            if (curN >= perFile || fi == order.size() - 1) {
                written.add(writeFile(new File(outdir, String.format("%s_%03d.c", prefix, nfile++)), header, decl, cur, null));
                cur.clear();
                curN = 0;
            }
        }
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
