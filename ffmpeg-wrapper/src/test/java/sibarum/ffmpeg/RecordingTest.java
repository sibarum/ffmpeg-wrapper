package sibarum.ffmpeg;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import java.io.IOException;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

/**
 * Records for real, through the bundled FFmpeg's AV1 encoders, and reads every file back through {@link Ffmpeg#open}:
 * a recording is only right if the decoder half of this library plays it as it was made.
 */
class RecordingTest {

    @TempDir
    Path dir;

    @ParameterizedTest
    @ValueSource(strings = {"rec.mkv", "rec.webm"})
    void recordsFramesThatPlayBackInColourAndTime(String name) throws IOException {
        Path file = dir.resolve(name);
        // Ten frames of red, a pause of two seconds with nothing drawn, ten of green: times as a window gives them.
        List<Long> times = new ArrayList<>();
        try (Recording rec = Ffmpeg.record(file, 96, 64, new Recording.Settings(10, Recording.Settings.TEXT,
                Recording.Encoder.SOFTWARE))) {
            assertEquals("libsvtav1", rec.encoder());
            for (int i = 0; i < 20; i++) {
                long t = i * 100_000L + (i >= 10 ? 2_000_000L : 0);
                assertTrue(rec.frame(solid(96, 64, i < 10 ? 0xff0000 : 0x00ff00), t));
                times.add(t);
            }
            assertEquals(20, rec.frames());
        }
        assertTrue(Files.size(file) > 0);
        try (Video v = Ffmpeg.open(file)) {
            assertEquals("av1", v.info().codec());
            assertEquals(96, v.info().width());
            assertEquals(64, v.info().height());
            List<Frame> frames = new ArrayList<>();
            for (Frame f; (f = v.next()) != null; ) {
                frames.add(f);
            }
            assertEquals(20, frames.size());
            for (int i = 0; i < 20; i++) {
                assertEquals(times.get(i), frames.get(i).ptsMicros(), 1000, "frame " + i + "'s time");
            }
            assertColour(frames.get(5), 0xff0000);
            assertColour(frames.get(15), 0x00ff00);
        }
    }

    @Test
    void anOddSizeIsKept() {
        Path file = dir.resolve("odd.mkv");
        try (Recording rec = Ffmpeg.record(file, 65, 49, Recording.Settings.DEFAULT)) {
            for (int i = 0; i < 3; i++) {
                rec.frame(solid(65, 49, 0x0000ff), i * 16_667L);
            }
        }
        try (Video v = Ffmpeg.open(file)) {
            assertEquals(65, v.info().width());
            assertEquals(49, v.info().height());
            assertColour(v.next(), 0x0000ff);
        }
    }

    @Test
    void aFrameInTheLastOnesMillisecondIsDropped() {
        try (Recording rec = Ffmpeg.record(dir.resolve("drop.mkv"), 64, 64, Recording.Settings.DEFAULT)) {
            byte[] grey = solid(64, 64, 0x808080);
            assertTrue(rec.frame(grey, 5_000));
            assertFalse(rec.frame(grey, 5_400), "the same millisecond");
            assertFalse(rec.frame(grey, 1_000), "earlier");
            assertTrue(rec.frame(grey, 6_000));
            assertEquals(2, rec.frames());
            assertEquals(2, rec.dropped());
        }
    }

    @Test
    void takesNativeMemoryAsItIs() {
        Path file = dir.resolve("native.mkv");
        try (Arena arena = Arena.ofConfined(); Recording rec = Ffmpeg.record(file, 64, 64, Recording.Settings.DEFAULT)) {
            MemorySegment pixels = arena.allocate(64 * 64 * 4);
            pixels.copyFrom(MemorySegment.ofArray(solid(64, 64, 0xff00ff)));
            assertTrue(rec.frame(pixels, 64, 64, Recording.Pixels.RGBA, 0));
        }
        try (Video v = Ffmpeg.open(file)) {
            assertColour(v.next(), 0xff00ff);
        }
    }

    /** A presented frame comes back B, G, R, A; read as such, its colours are the screen's. */
    @Test
    void takesBgraAsAPresenterHoldsIt() {
        Path file = dir.resolve("bgra.mkv");
        try (Arena arena = Arena.ofConfined(); Recording rec = Ffmpeg.record(file, 64, 64, Recording.Settings.DEFAULT)) {
            // Orange (ff8000) stored B, G, R, A: the bytes of solid()'s 0x0080ff.
            MemorySegment pixels = arena.allocate(64 * 64 * 4);
            pixels.copyFrom(MemorySegment.ofArray(solid(64, 64, 0x0080ff)));
            assertTrue(rec.frame(pixels, 64, 64, Recording.Pixels.BGRA, 0));
        }
        try (Video v = Ffmpeg.open(file)) {
            assertColour(v.next(), 0xff8000);
        }
    }

    /**
     * A process killed mid-recording never finishes its file, so what is on the disk while it is still open is what a
     * crash leaves: every second but the last must already be there, and play.
     */
    @Test
    void anUnfinishedRecordingPlaysUpToItsLastSecond() {
        Path file = dir.resolve("killed.mkv");
        try (Recording rec = Ffmpeg.record(file, 64, 64, Recording.Settings.DEFAULT)) {
            for (int i = 0; i < 40; i++) {     // four seconds at 10 fps
                rec.frame(solid(64, 64, i < 20 ? 0xff0000 : 0x0000ff), i * 100_000L);
            }
            try (Video v = Ffmpeg.open(file)) {     // read while the writer is still open: nothing finished it
                int n = 0;
                Frame last = null;
                for (Frame f; (f = v.next()) != null; ) {
                    last = f;
                    n++;
                }
                assertTrue(n >= 25, "only " + n + " of 40 frames on the disk before the finish");
                assertColour(last, 0x0000ff);
            }
        }
    }

    /** A window left still: the last frame lasts until the recording ends, not until it last changed. */
    @Test
    void theLastFrameIsHeldUntilTheEnd() {
        Path file = dir.resolve("held.mkv");
        try (Recording rec = Ffmpeg.record(file, 64, 64, Recording.Settings.DEFAULT)) {
            assertFalse(rec.repeat(1_000_000), "nothing to hold before a frame");
            rec.frame(solid(64, 64, 0xff0000), 0);
            rec.frame(solid(64, 64, 0x00ff00), 100_000);
            assertTrue(rec.repeat(5_000_000));
            assertFalse(rec.repeat(5_000_000), "not twice in one millisecond");
        }
        try (Video v = Ffmpeg.open(file)) {
            assertTrue(Math.abs(v.info().durationMicros() - 5_000_000) < 100_000, "duration " + v.info().durationMicros());
            v.next();
            v.next();
            Frame held = v.next();
            assertEquals(5_000_000, held.ptsMicros(), 1000);
            assertColour(held, 0x00ff00);
        }
    }

    @Test
    void hardwareFallsBackToSoftwareAndPlaysEitherWay() {
        Path file = dir.resolve("hw.mkv");
        try (Recording rec = Ffmpeg.record(file, 128, 128,
                Recording.Settings.DEFAULT.withEncoder(Recording.Encoder.HARDWARE))) {
            assertFalse(rec.encoder().isBlank());
            for (int i = 0; i < 10; i++) {
                rec.frame(solid(128, 128, 0xff0000), i * 16_667L);
            }
        }
        try (Video v = Ffmpeg.open(file)) {
            assertEquals("av1", v.info().codec());
            int n = 0;
            for (Frame f; (f = v.next()) != null; ) {
                assertColour(f, 0xff0000);
                n++;
            }
            assertEquals(10, n);
        }
    }

    /** Real time is the point: 1080p must encode well ahead of 60 frames a second on the machine the build runs on. */
    @ParameterizedTest
    @ValueSource(strings = {"SOFTWARE", "HARDWARE"})
    void keepsUpWithA1080pWindow(String encoder) {
        int w = 1920, h = 1080, n = 120;
        byte[][] frames = new byte[4][];
        for (int i = 0; i < frames.length; i++) {
            frames[i] = interfaceLike(w, h, i);
        }
        try (Recording rec = Ffmpeg.record(dir.resolve("hd.mkv"), w, h,
                Recording.Settings.DEFAULT.withEncoder(Recording.Encoder.valueOf(encoder)))) {
            rec.frame(frames[0], 0);    // the encoder's start is not a frame's cost
            long start = System.nanoTime();
            for (int i = 1; i <= n; i++) {
                rec.frame(frames[i % frames.length], i * 16_667L);
            }
            double perFrameMs = (System.nanoTime() - start) / 1e6 / n;
            System.out.printf("1080p %s: %.2f ms a frame%n", rec.encoder(), perFrameMs);
            assertTrue(perFrameMs < 16.7, "1080p at " + perFrameMs + " ms a frame cannot keep up with 60 fps");
        }
    }

    @Test
    void refusesWhatItCannotRecord() {
        assertThrows(VideoException.class, () -> Ffmpeg.record(dir.resolve("x.gif"), 64, 64, Recording.Settings.DEFAULT));
        assertThrows(VideoException.class, () -> Ffmpeg.record(dir.resolve("x.mkv"), 0, 64, Recording.Settings.DEFAULT));
        assertThrows(IllegalArgumentException.class, () -> new Recording.Settings(60, 64, Recording.Encoder.SOFTWARE));
        try (Recording rec = Ffmpeg.record(dir.resolve("size.mkv"), 64, 64, Recording.Settings.DEFAULT)) {
            assertThrows(VideoException.class, () -> rec.frame(new byte[10], 0));
            assertThrows(VideoException.class, () -> rec.frame(new byte[16], 2, 3, 0));
            assertThrows(VideoException.class, () -> rec.frame(new byte[0], 0, 0, 0));
        }
    }

    /** A window resized while it is recorded: one stream on one canvas, every frame at its own size, never scaled. */
    @ParameterizedTest
    @ValueSource(strings = {"SOFTWARE", "HARDWARE"})   // planar 4:2:0 and, on a GPU, NV12: each blackened its own way
    void framesOfAnySizeGoOnTheCanvasUnscaled(String encoder) {
        Path file = dir.resolve("resized.mkv");
        try (Recording rec = Ffmpeg.record(file, 128, 96,
                Recording.Settings.DEFAULT.withEncoder(Recording.Encoder.valueOf(encoder)))) {
            assertTrue(rec.frame(solid(128, 96, 0xff0000), 128, 96, 0));
            assertTrue(rec.frame(new Frame(65, 49, 100_000, solid(65, 49, 0x00ff00))));   // smaller, and odd
            assertTrue(rec.frame(solid(160, 120, 0x0000ff), 160, 120, 200_000));            // larger than the canvas
            assertTrue(rec.frame(solid(128, 96, 0xff0000), 300_000));                       // the canvas's own size
        }
        try (Video v = Ffmpeg.open(file)) {
            assertEquals(128, v.info().width());
            assertEquals(96, v.info().height());
            assertColour(v.next(), 0xff0000);
            Frame small = v.next();
            assertEquals(128, small.width());
            assertColourAt(small, 30, 22, 0x00ff00);      // inside the frame, at the top left
            assertColourAt(small, 100, 80, 0x000000);     // the canvas around it, black
            assertColourAt(small, 30, 80, 0x000000);
            assertColourAt(small, 100, 22, 0x000000);
            Frame large = v.next();
            assertColourAt(large, 4, 4, 0x0000ff);        // cut to the canvas: blue to every edge
            assertColourAt(large, 123, 91, 0x0000ff);
            Frame full = v.next();                        // no black left behind once a frame fills it again
            assertColourAt(full, 100, 80, 0xff0000);
            assertColourAt(full, 30, 22, 0xff0000);
        }
    }

    @Test
    void closingTwiceDoesNothingAndAClosedRecordingRefuses() {
        Recording rec = Ffmpeg.record(dir.resolve("closed.mkv"), 64, 64, Recording.Settings.DEFAULT);
        rec.close();
        rec.close();
        assertTrue(rec.closed());
        assertThrows(VideoException.class, () -> rec.frame(solid(64, 64, 0), 0));
    }

    static byte[] solid(int w, int h, int rgb) {
        byte[] px = new byte[w * h * 4];
        for (int i = 0; i < px.length; i += 4) {
            px[i] = (byte) (rgb >> 16);
            px[i + 1] = (byte) (rgb >> 8);
            px[i + 2] = (byte) rgb;
            px[i + 3] = (byte) 0xff;
        }
        return px;
    }

    /** Flat panels, rules and rows of small glyph-like marks, moved a little each frame: what a window is. */
    static byte[] interfaceLike(int w, int h, int shift) {
        byte[] px = new byte[w * h * 4];
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                int v = x < 300 ? 0x20 : 0x30;
                if (y % 24 == 0) {
                    v = 0x50;
                } else if (x >= 320 && (y % 24) > 6 && (y % 24) < 18 && ((x + shift * 3) * 7 + y * 13) % 11 < 4) {
                    v = 0xe0;
                }
                int i = (y * w + x) * 4;
                px[i] = (byte) v;
                px[i + 1] = (byte) v;
                px[i + 2] = (byte) (v + 8);
                px[i + 3] = (byte) 0xff;
            }
        }
        return px;
    }

    /** The frame's centre is {@code rgb}, within what a lossy codec and 4:2:0 leave of it. */
    static void assertColour(Frame f, int rgb) {
        assertColourAt(f, f.width() / 2, f.height() / 2, rgb);
    }

    static void assertColourAt(Frame f, int x, int y, int rgb) {
        MemorySegment px = MemorySegment.ofArray(f.rgba());
        int i = (y * f.width() + x) * 4;
        int[] want = {rgb >> 16 & 0xff, rgb >> 8 & 0xff, rgb & 0xff};
        for (int c = 0; c < 3; c++) {
            int got = Byte.toUnsignedInt(px.get(ValueLayout.JAVA_BYTE, i + c));
            assertEquals(want[c], got, 12, "channel " + c + " of " + f);
        }
    }
}
