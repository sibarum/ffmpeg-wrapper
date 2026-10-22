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
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

/**
 * Plays for real, through the bundled FFmpeg and miniaudio, to {@link Audio.Output#NOWHERE}: miniaudio's null
 * device, which drains the sound in real time as a speaker would, so the clock is a real clock on a machine with
 * no sound card. The fixture is {@code rgb.mp4}'s three seconds of a 440 Hz tone in AAC (README, "Tests").
 *
 * <p>Times are real, so the bounds are loose: a busy machine runs late, never early.
 */
class AudioTest {

    @Test
    void namesTheSound() throws IOException {
        try (Audio a = Ffmpeg.openAudio(fixture("rgb.mp4"), Audio.Output.NOWHERE)) {
            assertEquals("aac", a.codec());
            assertEquals(1, a.channels());
            assertEquals(44_100, a.sampleRate());
        }
    }

    /**
     * A file that is only sound opens as a video's sound does, in every format people keep sound in. The MP3 carries
     * a cover picture, which FFmpeg counts as a video stream: the sound must still be found, and the picture left.
     */
    @ParameterizedTest
    @CsvSource({
            "tone.wav,  pcm_s16le, 16000",
            "tone.flac, flac,      48000",
            "tone.mp3,  mp3,       48000",
            "tone.ogg,  vorbis,    48000",
            "tone.opus, opus,      48000",
            "tone.m4a,  aac,       48000",
    })
    void opensAFileThatIsOnlySound(String name, String codec, int rate) throws Exception {
        try (Audio a = Ffmpeg.openAudio(fixture(name), Audio.Output.NOWHERE)) {
            assertEquals(codec, a.codec());
            assertEquals(1, a.channels());
            assertEquals(rate, a.sampleRate());

            a.seek(2_700_000);
            a.play();
            long give_up = System.nanoTime() + 3_000_000_000L;
            while (!a.ended() && System.nanoTime() < give_up) {
                Thread.sleep(10);
            }
            assertTrue(a.ended(), "played to its end, clock " + a.clockMicros());
            long end = a.clockMicros();
            assertTrue(Math.abs(end - 3_000_000) < 100_000, "at the sound's end: " + end);
        }
    }

    @Test
    void refusesAFileWithoutSound() throws IOException {
        VideoException e = assertThrows(VideoException.class,
                () -> Ffmpeg.openAudio(fixture("rgb.webm"), Audio.Output.NOWHERE));
        assertTrue(e.getMessage().contains("no sound"), e.getMessage());
    }

    @Test
    void theClockHoldsUntilPlayedAndWhilePaused() throws Exception {
        try (Audio a = Ffmpeg.openAudio(fixture("rgb.mp4"), Audio.Output.NOWHERE)) {
            Thread.sleep(200);
            assertEquals(0, a.clockMicros(), "paused at the start");

            a.play();
            Thread.sleep(600);
            long played = a.clockMicros();
            assertTrue(played > 300_000 && played < 900_000, "after 0.6 s of playing: " + played);

            a.pause();
            long held = a.clockMicros();
            Thread.sleep(300);
            assertEquals(held, a.clockMicros(), "paused, the clock holds");

            a.play();
            Thread.sleep(300);
            assertTrue(a.clockMicros() > held, "and runs on when played");
        }
    }

    @Test
    void theClockNeverGoesBack() throws Exception {
        try (Audio a = Ffmpeg.openAudio(fixture("rgb.mp4"), Audio.Output.NOWHERE)) {
            a.play();
            long last = 0;
            long end = System.nanoTime() + 1_000_000_000L;
            while (System.nanoTime() < end) {
                long now = a.clockMicros();
                assertTrue(now >= last, now + " after " + last);
                last = now;
                Thread.sleep(1);
            }
            assertTrue(last > 500_000, "it ran: " + last);
        }
    }

    @Test
    void aSeekIsTheClockAtOnce() throws Exception {
        try (Audio a = Ffmpeg.openAudio(fixture("rgb.mp4"), Audio.Output.NOWHERE)) {
            a.seek(2_000_000);
            assertEquals(2_000_000, a.clockMicros(), "paused");

            a.play();
            Thread.sleep(400);
            a.seek(1_000_000);
            assertEquals(1_000_000, a.clockMicros(), "playing");
            Thread.sleep(400);
            long on = a.clockMicros();
            assertTrue(on > 1_100_000 && on < 1_700_000, "and plays on from there: " + on);
        }
    }

    @Test
    void endsAtTheEndOfTheSound() throws Exception {
        try (Audio a = Ffmpeg.openAudio(fixture("rgb.mp4"), Audio.Output.NOWHERE)) {
            a.seek(2_500_000);
            a.play();
            long give_up = System.nanoTime() + 3_000_000_000L;
            while (!a.ended() && System.nanoTime() < give_up) {
                Thread.sleep(10);
            }
            assertTrue(a.ended(), "ended by now, clock " + a.clockMicros());
            long end = a.clockMicros();
            assertTrue(Math.abs(end - 3_000_000) < 100_000, "at the sound's end: " + end);
            Thread.sleep(200);
            assertEquals(end, a.clockMicros(), "and stays there");

            a.seek(0);
            assertFalse(a.ended(), "a seek starts it again");
        }
    }

    @Test
    void aClosedAudioSaysSo() throws IOException {
        Audio a = Ffmpeg.openAudio(fixture("rgb.mp4"), Audio.Output.NOWHERE);
        a.play();
        a.close();
        a.close();
        assertTrue(a.closed());
        assertThrows(VideoException.class, a::clockMicros);
    }

    /**
     * As for a {@code Video}: once the sound's handle is open, with its thread started and its device held, an
     * {@code Audio} that then fails to be made closes it on the way out. The failure is put in by hand.
     */
    @Test
    void aHandleWhoseAudioFailsToBeMadeIsClosed() throws Throwable {
        MemorySegment handle;
        try (Arena arena = Arena.ofConfined()) {
            handle = (MemorySegment) CApi.ffw_audio_open.invokeExact(
                    arena.allocateFrom(fixture("rgb.mp4").toString()), 1);
        }
        assertNotEquals(0, handle.address(), "the fixture opens");
        List<MemorySegment> freed = new ArrayList<>();
        VideoException failure = new VideoException("the sound's facts would not read");
        assertSame(failure, assertThrows(VideoException.class, () -> Ffmpeg.adopt(handle, h -> {
            throw failure;
        }, h -> {
            freed.add(h);
            Ffmpeg.closeAudio(h);
        })));
        assertEquals(List.of(handle), freed);
    }

    /** A fixture as a file, since FFmpeg opens paths. */
    private static Path fixture(String name) throws IOException {
        Path out = Files.createTempFile("fixture", "-" + name);
        out.toFile().deleteOnExit();
        try (InputStream in = AudioTest.class.getResourceAsStream("/" + name)) {
            Files.copy(in, out, StandardCopyOption.REPLACE_EXISTING);
        }
        return out;
    }
}
