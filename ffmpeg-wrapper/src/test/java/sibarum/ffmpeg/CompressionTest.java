package sibarum.ffmpeg;

import org.junit.jupiter.api.Test;

import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

/**
 * Compression, measured on what the device is given ({@link Audio#heard}), playing for real to
 * {@link Audio.Output#NOWHERE}. The fixtures are a 440 Hz tone, mono FLAC, three seconds:
 * {@code quiet-loud.flac} at 0.01 for 1.5 s and then 0.7, and {@code bang.flac} at 0.01 throughout but for 20 ms
 * at 0.95 from 1.5 s (README, "Tests").
 *
 * <p>Times are real, so each reading waits for the clock to reach a place well inside the passage it measures.
 */
class CompressionTest {

    /** About 50 ms at the null device's 48 kHz. */
    private static final int WINDOW = 2400;

    @Test
    void bringsLoudAndQuietTogether() throws Exception {
        double plain = range(0f);
        double compressed = range(1f);
        assertTrue(plain > 35, "as stored, 0.01 against 0.7 is 37 dB: " + plain);
        assertTrue(compressed < plain - 15, "compressed, much nearer: " + compressed + " dB against " + plain);
    }

    /** The loud passage against the quiet one, in dB, as heard at compression c. */
    private static double range(float c) throws Exception {
        try (Audio a = Ffmpeg.openAudio(fixture("quiet-loud.flac"), Audio.Output.NOWHERE)) {
            a.compression(c);
            a.seek(800_000);
            a.play();
            double quiet = rmsAt(a, 1_150_000);
            double loud = rmsAt(a, 2_400_000);
            return 20 * Math.log10(loud / quiet);
        }
    }

    /**
     * The point of the lookahead: a quiet scene is raised, and a bang in it is brought down, because the gain has
     * come down by the time the bang is played rather than after.
     */
    @Test
    void raisesTheQuietAndLowersTheBangInIt() throws Exception {
        float[] plain = around(0f);
        float[] compressed = around(1f);
        double plainPeak = peak(plain);
        double peak = peak(compressed);
        assertTrue(plainPeak > 0.6, "the bang is in the window: " + plainPeak);
        double ceiling = Math.pow(10, -6 / 20.0);
        assertTrue(peak <= ceiling * 1.001, "the bang is limited to -6 dBFS: " + peak + " against " + plainPeak);

        double raised = 20 * Math.log10(rms(compressed, 0, 1200) / rms(plain, 0, 1200));
        assertTrue(raised > 6, "the quiet before it is raised: " + raised + " dB");
    }

    /** A quarter of a second heard, ending just after the bang, at compression c. */
    private static float[] around(float c) throws Exception {
        try (Audio a = Ffmpeg.openAudio(fixture("bang.flac"), Audio.Output.NOWHERE)) {
            a.compression(c);
            a.seek(1_000_000);
            a.play();
            waitFor(a, 1_600_000);
            float[] heard = new float[12_000 * 2];
            assertEquals(12_000, a.heard(heard), "a quarter of a second is kept");
            return heard;
        }
    }

    @Test
    void aChangeIsHeardAtOnce() throws Exception {
        try (Audio a = Ffmpeg.openAudio(fixture("quiet-loud.flac"), Audio.Output.NOWHERE)) {
            a.seek(300_000);
            a.play();
            double before = rmsAt(a, 700_000);
            a.compression(1f);
            Thread.sleep(80);
            float[] heard = new float[1200 * 2];
            int got = a.heard(heard);
            double after = rms(heard, 0, got);
            double raised = 20 * Math.log10(after / before);
            assertTrue(raised > 6, "raised within 80 ms of asking: " + raised + " dB");
        }
    }

    /** What the leveller holds back is held in the decoder, which is ahead anyway: the clock is as it was. */
    @Test
    void theClockIsAsItWas() throws Exception {
        try (Audio a = Ffmpeg.openAudio(fixture("quiet-loud.flac"), Audio.Output.NOWHERE)) {
            a.compression(1f);
            a.seek(1_000_000);
            assertEquals(1_000_000, a.clockMicros(), "a seek is the clock at once");
            a.play();
            Thread.sleep(400);
            long on = a.clockMicros();
            assertTrue(on > 1_200_000 && on < 1_550_000, "and plays on from there, without waiting: " + on);

            a.seek(2_500_000);
            long giveUp = System.nanoTime() + 3_000_000_000L;
            while (!a.ended() && System.nanoTime() < giveUp) {
                Thread.sleep(10);
            }
            assertTrue(a.ended(), "every sample played, the leveller's last included");
            long end = a.clockMicros();
            assertTrue(Math.abs(end - 3_000_000) < 100_000, "at the sound's end: " + end);
        }
    }

    @Test
    void compressionIsClamped() throws Exception {
        try (Audio a = Ffmpeg.openAudio(fixture("tone.wav"), Audio.Output.NOWHERE)) {
            a.compression(-1f);
            a.compression(5f);
            a.compression(Float.NaN);
            a.play();
            Thread.sleep(200);
            float[] heard = new float[WINDOW * 2];
            assertTrue(a.heard(heard) > 0);
        }
    }

    /** The loudness of the last WINDOW frames heard once the clock has reached {@code us}. */
    private static double rmsAt(Audio a, long us) throws InterruptedException {
        waitFor(a, us);
        float[] heard = new float[WINDOW * 2];
        int got = a.heard(heard);
        return rms(heard, 0, got);
    }

    private static void waitFor(Audio a, long us) throws InterruptedException {
        long giveUp = System.nanoTime() + 5_000_000_000L;
        while (a.clockMicros() < us) {
            assertTrue(System.nanoTime() < giveUp, "the clock reached " + us + ": " + a.clockMicros());
            Thread.sleep(2);
        }
    }

    private static double rms(float[] stereo, int from, int frames) {
        double s = 0;
        for (int i = from * 2; i < (from + frames) * 2; i++) {
            s += stereo[i] * (double) stereo[i];
        }
        return Math.sqrt(s / (frames * 2.0));
    }

    private static double peak(float[] stereo) {
        double p = 0;
        for (float x : stereo) {
            p = Math.max(p, Math.abs(x));
        }
        return p;
    }

    /** A fixture as a file, since FFmpeg opens paths. */
    private static Path fixture(String name) throws IOException {
        Path out = Files.createTempFile("fixture", "-" + name);
        out.toFile().deleteOnExit();
        try (InputStream in = CompressionTest.class.getResourceAsStream("/" + name)) {
            Files.copy(in, out, StandardCopyOption.REPLACE_EXISTING);
        }
        return out;
    }
}
