package com.levigobin.classicvoices;

import java.io.EOFException;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;

/** Random access to bytes: a file, a file descriptor, or a URL (HTTP ranges). */
abstract class Source {
    abstract long size() throws IOException;

    abstract void read(long off, byte[] b, int o, int n) throws IOException;

    byte[] read(long off, int n) throws IOException {
        byte[] b = new byte[n];
        read(off, b, 0, n);
        return b;
    }

    /** A forward stream from off (for a long sequential read). */
    InputStream stream(final long off, final long len) throws IOException {
        return new InputStream() {
            long pos = off, left = len;
            final byte[] one = new byte[1];

            @Override
            public int read() throws IOException {
                return read(one, 0, 1) < 0 ? -1 : one[0] & 0xFF;
            }

            @Override
            public int read(byte[] b, int o, int n) throws IOException {
                if (left <= 0) return -1;
                int take = (int) Math.min(n, Math.min(left, 1 << 20));
                Source.this.read(pos, b, o, take);
                pos += take;
                left -= take;
                return take;
            }
        };
    }

    void close() throws IOException {}

    static final String UA = "classicvoices-android/1.0";

    // ---------------------------------------------------------------- files

    static final class FileSrc extends Source {
        private final FileChannel ch;
        private final java.io.Closeable owner;

        FileSrc(RandomAccessFile f) {
            ch = f.getChannel();
            owner = f;
        }

        FileSrc(FileInputStream in) {
            ch = in.getChannel();
            owner = in;
        }

        @Override
        long size() throws IOException {
            return ch.size();
        }

        @Override
        void read(long off, byte[] b, int o, int n) throws IOException {
            ByteBuffer bb = ByteBuffer.wrap(b, o, n);
            long p = off;
            while (bb.hasRemaining()) {
                int got = ch.read(bb, p);
                if (got < 0) throw new EOFException("unexpected end of file");
                p += got;
            }
        }

        @Override
        void close() throws IOException {
            owner.close();
        }
    }

    // ---------------------------------------------------------------- HTTP

    static HttpURLConnection open(String url, long start, long end) throws IOException {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        c.setRequestProperty("User-Agent", UA);
        c.setConnectTimeout(30000);
        c.setReadTimeout(120000);
        if (start >= 0) c.setRequestProperty("Range", "bytes=" + start + "-" + (end >= 0 ? String.valueOf(end) : ""));
        int code = c.getResponseCode();
        if (code != 200 && code != 206) {
            c.disconnect();
            throw new IOException("HTTP " + code + " for " + url);
        }
        if (start > 0 && code != 206) {
            c.disconnect();
            throw new IOException("the server ignored the byte range for " + url);
        }
        return c;
    }

    static final class Http extends Source {
        private final String url;
        private long size = -1;

        Http(String url) {
            this.url = url;
        }

        @Override
        long size() throws IOException {
            if (size < 0) {
                HttpURLConnection c = open(url, 0, 0);
                try {
                    String cr = c.getHeaderField("Content-Range");
                    if (cr == null || cr.indexOf('/') < 0) throw new IOException("no Content-Range from " + url);
                    size = Long.parseLong(cr.substring(cr.indexOf('/') + 1).trim());
                } finally {
                    c.disconnect();
                }
            }
            return size;
        }

        @Override
        void read(long off, byte[] b, int o, int n) throws IOException {
            if (n == 0) return;
            HttpURLConnection c = open(url, off, off + n - 1);
            try {
                InputStream in = c.getInputStream();
                int got = 0;
                while (got < n) {
                    int r = in.read(b, o + got, n - got);
                    if (r < 0) throw new EOFException("short read from " + url);
                    got += r;
                }
            } finally {
                c.disconnect();
            }
        }

        @Override
        InputStream stream(long off, long len) throws IOException {
            final HttpURLConnection c = open(url, off, off + len - 1);
            final InputStream in = c.getInputStream();
            return new java.io.FilterInputStream(in) {
                @Override
                public void close() throws IOException {
                    try {
                        super.close();
                    } finally {
                        c.disconnect();
                    }
                }
            };
        }
    }

    // ---------------------------------------------------------------- helpers

    static int be16(byte[] b, int o) { return ((b[o] & 0xFF) << 8) | (b[o + 1] & 0xFF); }

    static long be32(byte[] b, int o) {
        return ((long) (b[o] & 0xFF) << 24) | ((b[o + 1] & 0xFF) << 16) | ((b[o + 2] & 0xFF) << 8) | (b[o + 3] & 0xFF);
    }

    static long be64(byte[] b, int o) { return (be32(b, o) << 32) | be32(b, o + 4); }

    static int le16(byte[] b, int o) { return (b[o] & 0xFF) | ((b[o + 1] & 0xFF) << 8); }

    static long le32(byte[] b, int o) { return (le16(b, o) & 0xFFFFL) | ((long) le16(b, o + 2) << 16); }

    static long le64(byte[] b, int o) { return (le32(b, o) & 0xFFFFFFFFL) | (le32(b, o + 4) << 32); }

    static void readFully(InputStream in, byte[] b, int o, int n) throws IOException {
        int got = 0;
        while (got < n) {
            int r = in.read(b, o + got, n - got);
            if (r < 0) throw new EOFException("the download ended early");
            got += r;
        }
    }

    static void skipFully(InputStream in, long n) throws IOException {
        byte[] buf = new byte[65536];
        while (n > 0) {
            int r = in.read(buf, 0, (int) Math.min(buf.length, n));
            if (r < 0) throw new EOFException("the download ended early");
            n -= r;
        }
    }
}
