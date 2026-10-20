package sibarum.ffmpeg;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import java.io.IOException;
import java.io.InputStream;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.List;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

/**
 * Decodes for real, through the bundled FFmpeg, and so fails rather than skips when it is missing: a
 * green run of a bindings suite that never crossed the boundary would say nothing at all.
 *
 * <p>Every fixture is the same three seconds at 64x48 and 10 fps — one second red, one green, one blue,
 * a keyframe every ten frames — in three codecs and three containers, made by the bundled ffmpeg
 * (README, "Tests"). A colour is something to assert that survives any lossy codec.
 */
class FfmpegTest {

    private static final int RED = 0, GREEN = 1, BLUE = 2;

    @Test
    void reportsItsVersion() {
        assertFalse(Ffmpeg.version().isBlank());
    }

    @ParameterizedTest
    @ValueSource(strings = {"rgb.mp4", "rgb.webm", "rgb.mkv"})
    void decodesEveryFrameInOrder(String name) throws IOException {
        try (Video v = Ffmpeg.open(fixture(name))) {
            VideoInfo info = v.info();
            assertEquals(64, info.width());
            assertEquals(48, info.height());
            assertEquals(10.0, info.frameRate(), 0.01);
            assertTrue(Math.abs(info.durationMicros() - 3_000_000) < 150_000, "duration " + info.durationMicros());

            List<Frame> frames = new ArrayList<>();
            for (Frame f; (f = v.next()) != null; ) {
                frames.add(f);
            }
            assertEquals(30, frames.size());
            long last = -1;
            for (Frame f : frames) {
                assertEquals(64 * 48 * 4, f.rgba().length);
                assertTrue(f.ptsMicros() > last, "timestamps rise");
                last = f.ptsMicros();
            }
            assertEquals(0, frames.getFirst().ptsMicros());
            assertDominant(frames.get(5), RED);
            assertDominant(frames.get(15), GREEN);
            assertDominant(frames.get(25), BLUE);
            assertNull(v.next(), "the end stays the end");
        }
    }

    @Test
    void namesCodecContainerAndSound() throws IOException {
        try (Video mp4 = Ffmpeg.open(fixture("rgb.mp4")); Video webm = Ffmpeg.open(fixture("rgb.webm"));
             Video mkv = Ffmpeg.open(fixture("rgb.mkv"))) {
            assertEquals("h264", mp4.info().codec());
            assertTrue(mp4.info().container().contains("mp4"));
            assertTrue(mp4.info().audio());
            assertEquals("vp9", webm.info().codec());
            assertTrue(webm.info().container().contains("webm"));
            assertFalse(webm.info().audio());
            assertEquals("av1", mkv.info().codec());
        }
    }

    @Test
    void scalesToTheOutputAskedFor() throws IOException {
        try (Video v = Ffmpeg.open(fixture("rgb.webm"))) {
            v.output(16, 12);
            Frame f = v.next();
            assertEquals(16, f.width());
            assertEquals(12, f.height());
            assertEquals(16 * 12 * 4, f.rgba().length);
            assertDominant(f, RED);
        }
    }

    @ParameterizedTest
    @ValueSource(strings = {"rgb.mp4", "rgb.webm", "rgb.mkv"})
    void seeksExactlyNotToTheKeyframeBefore(String name) throws IOException {
        try (Video v = Ffmpeg.open(fixture(name))) {
            v.seek(2_450_000);   // frame 24: blue, four frames past the keyframe at 2.0 s
            Frame f = v.next();
            assertEquals(2_400_000, f.ptsMicros());
            assertDominant(f, BLUE);

            v.seek(1_000_000);   // back, onto a keyframe
            f = v.next();
            assertEquals(1_000_000, f.ptsMicros());
            assertDominant(f, GREEN);

            v.seek(0);
            assertEquals(0, v.next().ptsMicros());
        }
    }

    @Test
    void decodesTheSameAfterSeekingBackToTheStart() throws IOException {
        try (Video v = Ffmpeg.open(fixture("rgb.webm"))) {
            byte[] first = v.next().rgba();
            v.next();
            v.seek(0);
            assertArrayEquals(first, v.next().rgba());
        }
    }

    @Test
    void refusesWhatIsNotAVideo() throws IOException {
        Path text = Files.createTempFile("not-a-video", ".mp4");
        Files.writeString(text, "plain text, not a video");
        VideoException e = assertThrows(VideoException.class, () -> Ffmpeg.open(text));
        assertTrue(e.getMessage().contains("moov atom not found"), e.getMessage());
        assertThrows(VideoException.class, () -> Ffmpeg.open(text.resolveSibling("no-such-file.mp4")));
    }

    @Test
    void aClosedVideoSaysSo() throws IOException {
        Video v = Ffmpeg.open(fixture("rgb.webm"));
        v.close();
        v.close();
        assertTrue(v.closed());
        assertThrows(VideoException.class, v::next);
    }

    /**
     * Once the native open has succeeded, nothing else can close the handle if the {@code Video} around it then
     * fails to be made, so the open path must. The failure is put in by hand: a file that opens never fails the
     * reads that follow.
     */
    @Test
    void aHandleWhoseVideoFailsToBeMadeIsClosed() throws Throwable {
        for (RuntimeException failure : List.of(new VideoException("the stream's facts would not read"),
                new IllegalStateException("anything else"))) {
            MemorySegment handle = rawOpen(fixture("rgb.mp4"));
            List<MemorySegment> freed = new ArrayList<>();
            RuntimeException e = assertThrows(RuntimeException.class, () -> Ffmpeg.adopt(handle, h -> {
                throw failure;
            }, h -> {
                freed.add(h);
                Ffmpeg.closeVideo(h);
            }));
            assertSame(failure, e, "the failure goes on as it was");
            assertEquals(List.of(handle), freed);
        }
    }

    @Test
    void anErrorWhileMakingTheVideoStillClosesTheHandle() throws Throwable {
        MemorySegment handle = rawOpen(fixture("rgb.mp4"));
        List<MemorySegment> freed = new ArrayList<>();
        OutOfMemoryError oom = new OutOfMemoryError("no room for the frame");
        assertSame(oom, assertThrows(OutOfMemoryError.class, () -> Ffmpeg.adopt(handle, h -> {
            throw oom;
        }, h -> {
            freed.add(h);
            Ffmpeg.closeVideo(h);
        })));
        assertEquals(List.of(handle), freed);
    }

    @Test
    void aCloseThatFailsTooIsKeptBesideTheFirstFailure() throws Throwable {
        MemorySegment handle = rawOpen(fixture("rgb.mp4"));
        VideoException first = new VideoException("first");
        VideoException e = assertThrows(VideoException.class, () -> Ffmpeg.adopt(handle, h -> {
            throw first;
        }, h -> {
            Ffmpeg.closeVideo(h);
            throw new VideoException("the close failed too");
        }));
        assertSame(first, e);
        assertEquals("the close failed too", e.getSuppressed()[0].getMessage());
    }

    @Test
    void aVideoThatIsMadeKeepsItsHandle() throws Throwable {
        MemorySegment handle = rawOpen(fixture("rgb.mp4"));
        List<MemorySegment> freed = new ArrayList<>();
        try (Video v = Ffmpeg.adopt(handle, h -> new Video(h, Path.of("rgb.mp4")), freed::add)) {
            assertEquals(64, v.info().width());
            assertTrue(freed.isEmpty());
        }
    }

    @Test
    void readsLibsTxtInOrder() {
        assertEquals(List.of("avutil-60.dll", "ffwrap.dll"), NativeLib.libraries("avutil-60.dll\r\n\n  ffwrap.dll \n"));
    }

    /** The channel {@code c} is far above the other two, at the frame's centre. */
    private static void assertDominant(Frame f, int c) {
        int p = ((f.height() / 2) * f.width() + f.width() / 2) * 4;
        int[] rgb = {f.rgba()[p] & 0xFF, f.rgba()[p + 1] & 0xFF, f.rgba()[p + 2] & 0xFF};
        assertEquals(255, f.rgba()[p + 3] & 0xFF, "opaque");
        for (int other = 0; other < 3; other++) {
            if (other != c) {
                assertTrue(rgb[c] > 200 && rgb[other] < 60,
                        "expected channel " + c + " at " + f.ptsMicros() + "us, was " + rgb[0] + "," + rgb[1] + "," + rgb[2]);
            }
        }
    }

    /** The native open alone, with no {@code Video} made around it. */
    private static MemorySegment rawOpen(Path file) throws Throwable {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment handle = (MemorySegment) CApi.ffw_open.invokeExact(arena.allocateFrom(file.toString()));
            assertNotEquals(0, handle.address(), "the fixture opens");
            return handle;
        }
    }

    /** A fixture as a file, since FFmpeg opens paths. */
    private static Path fixture(String name) throws IOException {
        Path out = Files.createTempFile("fixture", "-" + name);
        out.toFile().deleteOnExit();
        try (InputStream in = FfmpegTest.class.getResourceAsStream("/" + name)) {
            Files.copy(in, out, StandardCopyOption.REPLACE_EXISTING);
        }
        return out;
    }
}
