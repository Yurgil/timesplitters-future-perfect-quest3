package org.tsfp.quest;

import java.io.Closeable;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InterruptedIOException;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

/** Android reader for the XDVDFS layout used by the pinned PC disc importer.
 * GPL-3.0-only. See tools/compiler/disc_image.py in the pinned PC source.
 */
final class XboxDisc implements Closeable {
    private static final int SECTOR = 2048;
    private static final byte[] MAGIC = "MICROSOFT*XBOX*MEDIA".getBytes(StandardCharsets.US_ASCII);
    private static final long[] PARTITIONS = {0, 0x18300000L, 0x0fd90000L, 0x02080000L};
    private static final String SUPPORTED_HASH =
            "3cfd001a84fc3e08175d6c4b2e42ebf49577a089b5d0bd87ac724f61a41816bc";
    private static final long SUPPORTED_SIZE = 6270976;
    private final RandomAccessFile image;
    private final long length;
    private final List<Entry> entries = new ArrayList<>();
    private long base;

    interface Progress {
        void update(long copied, long total, String file);
    }

    private static final class Entry {
        final String path;
        final long offset, size;
        final boolean directory;
        final int depth;

        Entry(String path, long offset, long size, boolean directory, int depth) {
            this.path = path;
            this.offset = offset;
            this.size = size;
            this.directory = directory;
            this.depth = depth;
        }
    }

    XboxDisc(File source) throws IOException {
        image = new RandomAccessFile(source, "r");
        length = image.length();
        try {
            int matches = 0;
            long sector = 0, size = 0;
            for (long candidate : PARTITIONS) {
                long address = candidate + 32L * SECTOR;
                if (address + SECTOR > length) {
                    continue;
                }
                byte[] header = read(address, SECTOR);
                if (Arrays.equals(MAGIC, Arrays.copyOfRange(header, 0, 20)) &&
                        Arrays.equals(MAGIC, Arrays.copyOfRange(header, SECTOR - 20, SECTOR))) {
                    ++matches;
                    base = candidate;
                    sector = uint32(header, 20);
                    size = uint32(header, 24);
                }
            }
            if (matches != 1) {
                throw new IOException("Expected one Xbox XDVDFS partition in the ISO.");
            }
            catalog(sector, size);
        } catch (IOException | RuntimeException error) {
            image.close();
            throw error;
        }
    }

    private void bounds(long offset, long size) throws IOException {
        if (offset < 0 || size < 0 || offset > length || size > length - offset) {
            throw new IOException("A disc entry extends beyond the ISO.");
        }
    }

    private byte[] read(long offset, int size) throws IOException {
        checkCancelled();
        bounds(offset, size);
        byte[] data = new byte[size];
        image.seek(offset);
        image.readFully(data);
        return data;
    }

    private static long uint32(byte[] data, int offset) {
        return (data[offset] & 255L) | ((data[offset + 1] & 255L) << 8) |
                ((data[offset + 2] & 255L) << 16) | ((data[offset + 3] & 255L) << 24);
    }

    private static int uint16(byte[] data, int offset) {
        return (data[offset] & 255) | ((data[offset + 1] & 255) << 8);
    }

    private static String safeName(byte[] data, int offset, int size) throws IOException {
        if (size == 0) {
            throw new IOException("Empty disc filename.");
        }
        for (int i = offset; i < offset + size; ++i) {
            int value = data[i] & 255;
            if (value < 32 || value >= 127 || "/\\:<>\"|?*".indexOf(value) >= 0) {
                throw new IOException("Unsafe disc filename.");
            }
        }
        String name = new String(data, offset, size, StandardCharsets.US_ASCII);
        if (name.equals(".") || name.equals("..") || name.endsWith(".") || name.endsWith(" ")) {
            throw new IOException("Unsafe disc path.");
        }
        return name;
    }

    private void catalog(long rootSector, long rootSize) throws IOException {
        ArrayDeque<Entry> pending = new ArrayDeque<>();
        pending.add(new Entry("", base + rootSector * SECTOR, rootSize, true, 0));
        Set<Long> tables = new HashSet<>();
        Set<String> names = new HashSet<>();
        long metadata = 0;
        while (!pending.isEmpty()) {
            Entry parent = pending.removeLast();
            if (parent.size == 0) {
                continue;
            }
            metadata += parent.size;
            if (parent.size > 16 * 1024 * 1024 || parent.depth >= 64 || metadata > 64 * 1024 * 1024 ||
                    !tables.add(parent.offset)) {
                throw new IOException("Invalid or excessively large disc directory.");
            }
            byte[] table = read(parent.offset, (int) parent.size);
            ArrayDeque<Integer> nodes = new ArrayDeque<>();
            Set<Integer> seen = new HashSet<>();
            List<int[]> spans = new ArrayList<>();
            nodes.add(0);
            while (!nodes.isEmpty()) {
                int node = nodes.removeLast();
                if (!seen.add(node) || node + 14 > table.length) {
                    throw new IOException("Invalid disc directory link.");
                }
                boolean empty = true, unused = true;
                for (int i = node; i < node + 14; ++i) {
                    empty &= table[i] == 0;
                    unused &= (table[i] & 255) == 255;
                }
                if (node == 0 && (empty || unused)) {
                    break;
                }
                int nameSize = table[node + 13] & 255;
                int end = node + 14 + nameSize;
                if (end > table.length) {
                    throw new IOException("Truncated disc filename.");
                }
                String name = safeName(table, node + 14, nameSize);
                String path = parent.path.isEmpty() ? name : parent.path + "/" + name;
                if (!names.add(path.toLowerCase(Locale.ROOT)) || names.size() > 100000) {
                    throw new IOException("Duplicate or excessive disc entries.");
                }
                spans.add(new int[] {node, end});
                long offset = base + uint32(table, node + 4) * SECTOR;
                long size = uint32(table, node + 8);
                bounds(offset, size);
                Entry entry = new Entry(path, offset, size, (table[node + 12] & 16) != 0,
                        parent.depth + 1);
                entries.add(entry);
                if (entry.directory) {
                    pending.add(entry);
                }
                for (int pointerOffset : new int[] {node, node + 2}) {
                    int pointer = uint16(table, pointerOffset);
                    if (pointer != 0 && pointer != 65535) {
                        nodes.add(pointer * 4);
                    }
                }
            }
            spans.sort(Comparator.comparingInt(span -> span[0]));
            for (int i = 1; i < spans.size(); ++i) {
                if (spans.get(i - 1)[1] > spans.get(i)[0]) {
                    throw new IOException("Overlapping disc directory records.");
                }
            }
        }
        entries.sort(Comparator.comparing(entry -> entry.path));
    }

    private static MessageDigest sha256() {
        try {
            return MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException error) {
            throw new AssertionError(error);
        }
    }

    void verifySupported() throws IOException {
        Entry xbe = null;
        boolean pak = false, xmv = false;
        for (Entry entry : entries) {
            if (entry.path.equals("default.xbe") && !entry.directory) {
                xbe = entry;
            }
            pak |= entry.path.equals("pak") && entry.directory;
            xmv |= entry.path.equals("xmv") && entry.directory;
        }
        if (xbe == null || xbe.size != SUPPORTED_SIZE || !pak || !xmv) {
            throw new IOException("This ISO must contain the supported US Xbox game.");
        }
        MessageDigest hash = sha256();
        image.seek(xbe.offset);
        byte[] buffer = new byte[1024 * 1024];
        long remaining = xbe.size;
        while (remaining > 0) {
            checkCancelled();
            int count = (int) Math.min(buffer.length, remaining);
            image.readFully(buffer, 0, count);
            hash.update(buffer, 0, count);
            remaining -= count;
        }
        verifyHash(hash);
    }

    static void verifyExecutable(File xbe) throws IOException {
        if (!xbe.isFile() || xbe.length() != SUPPORTED_SIZE) {
            throw new IOException("default.xbe must be the supported US Xbox executable.");
        }
        MessageDigest hash = sha256();
        byte[] buffer = new byte[1024 * 1024];
        try (FileInputStream input = new FileInputStream(xbe)) {
            int count;
            while ((count = input.read(buffer)) != -1) {
                checkCancelled();
                hash.update(buffer, 0, count);
            }
        }
        verifyHash(hash);
    }

    private static void verifyHash(MessageDigest hash) throws IOException {
        StringBuilder actual = new StringBuilder();
        for (byte value : hash.digest()) {
            actual.append(String.format(Locale.ROOT, "%02x", value & 255));
        }
        if (!SUPPORTED_HASH.contentEquals(actual)) {
            throw new IOException("Unsupported default.xbe revision. Use the supported US Xbox dump.");
        }
    }

    long requiredBytes() {
        long total = 0;
        for (Entry entry : entries) {
            if (!entry.directory) {
                total += entry.size;
            }
        }
        return total;
    }

    /** The caller owns a newly created staging directory and commits it after success. */
    void extract(File staging, Progress progress) throws IOException {
        byte[] buffer = new byte[1024 * 1024];
        long copied = 0, total = requiredBytes();
        for (Entry entry : entries) {
            checkCancelled();
            File target = new File(staging, entry.path);
            if (!target.getCanonicalPath().startsWith(staging.getCanonicalPath() + File.separator)) {
                throw new IOException("Disc path escapes the staging directory.");
            }
            File directory = entry.directory ? target : target.getParentFile();
            if (!directory.isDirectory() && !directory.mkdirs()) {
                throw new IOException("Could not create the game directory.");
            }
            if (entry.directory) {
                continue;
            }
            if (!target.createNewFile()) {
                throw new IOException("Conflicting disc file: " + entry.path);
            }
            image.seek(entry.offset);
            try (FileOutputStream output = new FileOutputStream(target)) {
                long remaining = entry.size;
                while (remaining > 0) {
                    checkCancelled();
                    int count = (int) Math.min(buffer.length, remaining);
                    image.readFully(buffer, 0, count);
                    output.write(buffer, 0, count);
                    remaining -= count;
                    copied += count;
                    progress.update(copied, total, entry.path);
                }
                output.getFD().sync();
            }
        }
    }

    private static void checkCancelled() throws InterruptedIOException {
        if (Thread.currentThread().isInterrupted()) {
            throw new InterruptedIOException("Import cancelled. You can try again.");
        }
    }

    @Override
    public void close() throws IOException {
        image.close();
    }
}
