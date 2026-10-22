package com.levigobin.classicvoices;

import java.io.IOException;
import java.security.GeneralSecurityException;

import javax.crypto.Cipher;
import javax.crypto.Mac;
import javax.crypto.spec.IvParameterSpec;
import javax.crypto.spec.SecretKeySpec;

/**
 * Plaintext view of an encrcdsa v2 image (iOS 3-7 root filesystems),
 * decrypting only the chunks actually read. Each chunk is AES-128-CBC with
 * an IV of HMAC-SHA1(hmac key, chunk number). Same as firmware.py's
 * EncrcdsaStream.
 */
final class Encrcdsa extends Source {
    private final Source f;
    private final int blockSize;
    private final long size, dataOff;
    private final Cipher aes;
    private final SecretKeySpec key;
    private final Mac hmac;
    private long cachedChunk = -1;
    private byte[] cached;

    static boolean is(Source s) throws IOException {
        byte[] m = s.read(0, 8);
        return new String(m, "ISO-8859-1").equals("encrcdsa");
    }

    Encrcdsa(Source f, String keyHex) throws IOException {
        this.f = f;
        byte[] hdr = f.read(0, 72);
        blockSize = (int) be32(hdr, 52);
        size = be64(hdr, 56);
        dataOff = be64(hdr, 64);
        byte[] k = hex(keyHex);
        if (k.length < 36) throw new IOException("the decryption key is too short");
        try {
            key = new SecretKeySpec(k, 0, 16, "AES");
            aes = Cipher.getInstance("AES/CBC/NoPadding");
            hmac = Mac.getInstance("HmacSHA1");
            hmac.init(new SecretKeySpec(k, 16, 20, "HmacSHA1"));
        } catch (GeneralSecurityException e) {
            throw new IOException("AES unavailable: " + e);
        }
    }

    private static byte[] hex(String s) {
        byte[] b = new byte[s.length() / 2];
        for (int i = 0; i < b.length; i++) b[i] = (byte) Integer.parseInt(s.substring(2 * i, 2 * i + 2), 16);
        return b;
    }

    private byte[] chunk(long n) throws IOException {
        if (n == cachedChunk) return cached;
        byte[] data = f.read(dataOff + n * blockSize, blockSize);
        try {
            byte[] iv = hmac.doFinal(new byte[] {(byte) (n >>> 24), (byte) (n >>> 16), (byte) (n >>> 8), (byte) n});
            aes.init(Cipher.DECRYPT_MODE, key, new IvParameterSpec(iv, 0, 16));
            cached = aes.doFinal(data, 0, blockSize - blockSize % 16);
        } catch (GeneralSecurityException e) {
            throw new IOException("decryption failed: " + e);
        }
        cachedChunk = n;
        return cached;
    }

    @Override
    long size() {
        return size;
    }

    @Override
    void read(long off, byte[] b, int o, int n) throws IOException {
        while (n > 0) {
            long ci = off / blockSize;
            int co = (int) (off % blockSize);
            byte[] c = chunk(ci);
            int take = Math.min(n, c.length - co);
            if (take <= 0) throw new IOException("read past the end of the image");
            System.arraycopy(c, co, b, o, take);
            off += take;
            o += take;
            n -= take;
        }
    }
}
