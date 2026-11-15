package sibarum.ffmpeg;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.CsvSource;

import java.io.IOException;
import java.io.InputStream;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.List;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

/**
 * Frames decoded straight into memory the caller lends ({@link Video#stageInto}), as a GPU's mapped staging would be:
 * in place by a decoder that can decode into memory it is given, copied in once by one that cannot, and the same
 * pixels either way as the decoder's own frame.
 */
class StagingTest {

    /**
     * H.264 and VP9 (FFmpeg's own decoders) decode in place, and so does AV1 (dav1d) where FFmpeg is built with
     * native/patches/libdav1d-get-buffer.patch, which is Windows; Linux's prebuilt FFmpeg still copies AV1 in once.
     */
    @ParameterizedTest
    @CsvSource({"rgb.mp4, false", "rgb.webm, false", "rgb.mkv, true"})
    void framesAreDecodedIntoTheStagingOrCopiedThereOnce(String name, boolean av1) throws IOException {
        boolean copies = av1 && !System.getProperty("os.name").startsWith("Windows");
        try (Arena arena = Arena.ofShared(); Video v = Ffmpeg.open(fixture(name))) {
            MemorySegment chunk = arena.allocate(16L << 20, 4096);
            v.stageInto(chunk, 7);
            List<Long> kept = new ArrayList<>();
            long[] offsets = new long[4];
            int[] strides = new int[4];
            for (int i = 0; i < 12 && v.decode() >= 0; i++) {
                Planes p = v.planes();
                long k = v.keep();
                assertNotEquals(0, k, Video.lastError());
                kept.add(k);
                assertEquals(7, Kept.place(k, offsets, strides));
                for (int plane = 0; plane < p.count(); plane++) {
                    assertTrue(offsets[plane] >= 0 && offsets[plane] < chunk.byteSize(), "inside the chunk");
                    assertEquals(0, offsets[plane] % 64, "aligned");
                    assertTrue(strides[plane] > 0);
                    int rows = plane == 0 ? p.height() : (p.height() + 1) / 2;
                    int row = plane == 0 ? p.width() : (p.width() + 1) / 2;   // 8-bit 4:2:0: the visible samples
                    for (int r = 0; r < rows; r++) {
                        MemorySegment mine = chunk.asSlice(offsets[plane] + (long) r * strides[plane], row);
                        MemorySegment theirs = p.data()[plane].asSlice((long) r * p.strides()[plane], row);
                        assertEquals(-1, mine.mismatch(theirs), name + " plane " + plane + " row " + r);
                    }
                }
            }
            assertEquals(12, kept.size());
            assertTrue(v.stageBlock() > 0);
            if (copies) {
                assertEquals(12, v.stageCopies(), "every kept frame copied in once");
            } else {
                assertTrue(v.stageCopies() <= 1, "decoded in place (the first, decoded before the chunk was lent, "
                        + "may be copied): " + v.stageCopies());
            }
            int roomBefore = v.stageRoom();
            for (long k : kept) {
                Kept.release(k);
            }
            assertTrue(v.stageRoom() > roomBefore, "released blocks come free (those the decoder still refers to stay)");
        }
    }

    /** A kept frame outlives its video: released after the close, which is how the last block comes free. */
    @Test
    void aKeptFrameIsReleasedAfterItsVideoCloses() throws IOException {
        try (Arena arena = Arena.ofShared()) {
            MemorySegment chunk = arena.allocate(8L << 20, 4096);
            long k;
            long again;
            try (Video v = Ffmpeg.open(fixture("rgb.mp4"))) {
                v.stageInto(chunk, 0);
                v.decode();
                k = v.keep();
                assertNotEquals(0, k, Video.lastError());
                again = Kept.ref(k);
            }
            long[] offsets = new long[4];
            int[] strides = new int[4];
            assertEquals(0, Kept.place(again, offsets, strides));
            Kept.release(k);
            Kept.release(again);
        }
    }

    /** No room left: the decoder goes on into its own memory, and a frame that cannot be kept says so. */
    @Test
    void aFullStagingKeepsNothingAndTheDecoderGoesOn() throws IOException {
        long block;
        try (Arena arena = Arena.ofShared(); Video v = Ffmpeg.open(fixture("rgb.mp4"))) {
            v.stageInto(arena.allocate(1L << 20, 4096), 0);
            v.decode();
            Kept.release(v.keep());
            block = v.stageBlock();
        }
        assertTrue(block > 0);
        try (Arena arena = Arena.ofShared(); Video v = Ffmpeg.open(fixture("rgb.mp4"))) {
            v.stageInto(arena.allocate(2 * block, 4096), 0);   // room for two frames, where the decoder keeps more
            List<Long> kept = new ArrayList<>();
            int frames = 0;
            for (; v.decode() >= 0; frames++) {
                long k = v.keep();
                if (k != 0) {
                    kept.add(k);
                }
            }
            assertEquals(30, frames, "every frame decoded, kept or not");
            assertTrue(kept.size() >= 1 && kept.size() <= 2, kept.size() + " kept in room for two");
            for (long k : kept) {
                Kept.release(k);
            }
        }
    }

    private static Path fixture(String name) throws IOException {
        Path out = Files.createTempFile("fixture", "-" + name);
        out.toFile().deleteOnExit();
        try (InputStream in = StagingTest.class.getResourceAsStream("/" + name)) {
            Files.copy(in, out, StandardCopyOption.REPLACE_EXISTING);
        }
        return out;
    }
}
