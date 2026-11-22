package org.tsfp.quest;

import java.io.File;
import java.io.IOException;
import java.io.InterruptedIOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;

/** Synthetic XDVDFS tests. The optional real ISO argument is only read and verified. */
public final class XboxDiscTest {
    private static final int SECTOR = 2048;

    private static void require(boolean condition) {
        if (!condition) {
            throw new AssertionError();
        }
    }

    private static void node(byte[] bytes, int table, int offset, int left, int right,
            int block, int size, int flags, String name) {
        ByteBuffer data = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN);
        int at = table * SECTOR + offset;
        data.position(at);
        data.putShort((short) left).putShort((short) right).putInt(block).putInt(size);
        data.put((byte) flags).put((byte) name.length());
        data.put(name.getBytes(StandardCharsets.US_ASCII));
    }

    private static byte[] fixture() {
        byte[] bytes = new byte[47 * SECTOR];
        byte[] magic = "MICROSOFT*XBOX*MEDIA".getBytes(StandardCharsets.US_ASCII);
        System.arraycopy(magic, 0, bytes, 32 * SECTOR, magic.length);
        System.arraycopy(magic, 0, bytes, 33 * SECTOR - 20, magic.length);
        ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
                .putInt(32 * SECTOR + 20, 40).putInt(32 * SECTOR + 24, SECTOR);
        node(bytes, 40, 0, 8, 16, 41, SECTOR, 16, "pak");
        node(bytes, 40, 32, 0, 65535, 43, 4, 32, "default.xbe");
        node(bytes, 40, 64, 0, 24, 42, SECTOR, 16, "xmv");
        node(bytes, 40, 96, 0, 0, 0, 0, 32, "empty.bin");
        node(bytes, 41, 0, 0, 0, 44, 3, 32, "asset.pak");
        node(bytes, 42, 0, 0, 0, 45, 2, 32, "intro.xmv");
        bytes[43 * SECTOR] = 17;
        bytes[44 * SECTOR] = 29;
        bytes[45 * SECTOR] = 41;
        return bytes;
    }

    private static void reject(Path root, String name, byte[] bytes) throws Exception {
        Path file = root.resolve(name + ".iso");
        Files.write(file, bytes);
        try (XboxDisc invalid = new XboxDisc(file.toFile())) {
            throw new AssertionError("Accepted malformed disc: " + name + ", bytes " + invalid.requiredBytes());
        } catch (IOException expected) {
            require(!expected.getMessage().isEmpty());
        }
    }

    public static void main(String[] args) throws Exception {
        Path root = Files.createTempDirectory("tsfp-disc-test-");
        Path iso = root.resolve("fixture.iso");
        byte[] bytes = fixture();
        Files.write(iso, bytes);
        File output = Files.createDirectory(root.resolve("game")).toFile();
        try (XboxDisc disc = new XboxDisc(iso.toFile())) {
            require(disc.requiredBytes() == 9);
            try {
                disc.verifySupported();
                throw new AssertionError("Accepted unsupported executable");
            } catch (IOException expected) {
                require(output.list().length == 0);
            }
            long[] last = {0};
            disc.extract(output, (copied, total, file) -> {
                require(copied > last[0] && copied <= total && total == 9);
                last[0] = copied;
            });
            require(last[0] == 9);
            require(Arrays.equals(Files.readAllBytes(output.toPath().resolve("default.xbe")),
                    new byte[] {17, 0, 0, 0}));
            require(Arrays.equals(Files.readAllBytes(output.toPath().resolve("pak/asset.pak")),
                    new byte[] {29, 0, 0}));
            require(Files.size(output.toPath().resolve("xmv/intro.xmv")) == 2);
            require(Files.size(output.toPath().resolve("empty.bin")) == 0);
            try {
                disc.extract(output, (copied, total, file) -> {});
                throw new AssertionError("Overwrote an existing file");
            } catch (IOException expected) {
                require(Files.readAllBytes(output.toPath().resolve("default.xbe"))[0] == 17);
            }
        }
        require(Arrays.equals(Files.readAllBytes(iso), bytes));
        byte[] bad = fixture();
        node(bad, 41, 0, 0, 0, 44, 3, 32, "../escape");
        reject(root, "traversal", bad);
        bad = fixture();
        node(bad, 40, 32, 0, 0, 43, 4, 32, "PAK");
        reject(root, "alias", bad);
        bad = fixture();
        node(bad, 40, 32, 8, 0, 43, 4, 32, "default.xbe");
        reject(root, "cycle", bad);
        bad = fixture();
        node(bad, 41, 0, 0, 0, 90, 3, 32, "asset.pak");
        reject(root, "extent", bad);
        reject(root, "truncated", Arrays.copyOf(fixture(), 43 * SECTOR));
        try (XboxDisc disc = new XboxDisc(iso.toFile())) {
            File cancelled = Files.createDirectory(root.resolve("cancelled")).toFile();
            Thread.currentThread().interrupt();
            try {
                disc.extract(cancelled, (copied, total, file) -> {});
                throw new AssertionError("Ignored cancellation");
            } catch (InterruptedIOException expected) {
                require(cancelled.list().length == 0);
            } finally {
                Thread.interrupted();
            }
        }
        if (args.length == 1) {
            try (XboxDisc real = new XboxDisc(new File(args[0]))) {
                real.verifySupported();
                System.out.println("Real ISO verified read-only; extraction size " + real.requiredBytes());
            }
        }
        try (java.util.stream.Stream<Path> paths = Files.walk(root)) {
            for (Path path : (Iterable<Path>) paths.sorted(java.util.Comparator.reverseOrder())::iterator) {
                Files.delete(path);
            }
        }
        System.out.println("XboxDisc synthetic tests passed");
    }
}
