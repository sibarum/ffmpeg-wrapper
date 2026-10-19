package sibarum.ffmpeg;

import java.lang.foreign.MemorySegment;
import java.nio.file.Path;

/**
 * A file's sound, playing out of a speaker. Made by {@link Ffmpeg#openAudio}, paused at the start; closed with
 * {@link #close}, which a try-with-resources does.
 *
 * <pre>{@code
 * try (Audio sound = Ffmpeg.openAudio(path)) {
 *     sound.play();
 *     long now = sound.clockMicros();      // the time being heard: show the frame due then
 *     sound.seek(12_500_000);              // exact, playing or paused
 * }
 * }</pre>
 *
 * <p><b>The sound never comes into Java.</b> It is decoded on a thread of its own, a little ahead, and handed
 * straight to the output device on the native side; this class only steers that and reads the clock back. A
 * {@link Video} on the same file is a separate handle with its own decoder, so the two seek independently.
 *
 * <p><b>The clock is what a player keeps time by.</b> It is the time of the sound being heard now, allowing for
 * what the device holds and has not played yet, and moves smoothly between the device's periods. It never goes
 * back except by a {@link #seek}, holds while paused, and after a seek holds at the place sought until the sound
 * from there starts to be heard.
 *
 * <p><b>Any thread.</b> Unlike a {@code Video}, an {@code Audio} may be steered and read from any thread.
 */
public final class Audio implements AutoCloseable {

    /** Where the sound goes. */
    public enum Output {
        /** The system's default output, following it when that changes (headphones plugged in, say). */
        SPEAKERS,
        /** No device: the sound is decoded and drained in real time, and the clock runs, but nothing is heard. */
        NOWHERE
    }

    private final Path file;
    private final String codec;
    private final int channels;
    private final int sampleRate;
    private MemorySegment handle;

    Audio(MemorySegment handle, Path file) {
        this.handle = handle;
        this.file = file;
        this.codec = Ffmpeg.string(call(() -> (MemorySegment) CApi.ffw_audio_codec.invokeExact(handle), "ffw_audio_codec"));
        this.channels = call(() -> (int) CApi.ffw_audio_channels.invokeExact(handle), "ffw_audio_channels");
        this.sampleRate = call(() -> (int) CApi.ffw_audio_rate.invokeExact(handle), "ffw_audio_rate");
    }

    /** The file this was opened from. */
    public Path file() {
        return file;
    }

    /** The sound's codec's short name ({@code "aac"}, {@code "opus"}), to be shown, never branched on. */
    public String codec() {
        return codec;
    }

    /** Channels as stored: 1, 2, 6 for 5.1. The output gets them mixed to stereo. */
    public int channels() {
        return channels;
    }

    /** Samples per second as stored. The output gets them at its own rate. */
    public int sampleRate() {
        return sampleRate;
    }

    /** Start the sound, from where the clock is. Playing already, nothing changes. */
    public synchronized void play() {
        MemorySegment h = open();
        if (call(() -> (int) CApi.ffw_audio_play.invokeExact(h), "ffw_audio_play") == 0) {
            throw new VideoException("could not play the sound of " + file + ": " + Video.lastError());
        }
    }

    /** Stop the sound and hold the clock, giving the device back. Paused already, nothing changes. */
    public synchronized void pause() {
        MemorySegment h = open();
        if (call(() -> (int) CApi.ffw_audio_pause.invokeExact(h), "ffw_audio_pause") == 0) {
            throw new VideoException("could not pause the sound of " + file + ": " + Video.lastError());
        }
    }

    /**
     * Go to {@code micros} from the stream's start, exactly; playing or paused, it stays so. The clock says
     * {@code micros} at once and holds there until the sound from it is heard.
     */
    public synchronized void seek(long micros) {
        MemorySegment h = open();
        call(() -> (int) CApi.ffw_audio_seek.invokeExact(h, micros), "ffw_audio_seek");
    }

    /** Loudness from 0 (silent) to 1 (as stored); clamped to that. */
    public synchronized void volume(float volume) {
        MemorySegment h = open();
        call(() -> {
            CApi.ffw_audio_volume.invokeExact(h, volume);
            return null;
        }, "ffw_audio_volume");
    }

    /** The time of the sound being heard now, in µs from the stream's start. */
    public synchronized long clockMicros() {
        MemorySegment h = open();
        return call(() -> (long) CApi.ffw_audio_clock_us.invokeExact(h), "ffw_audio_clock_us");
    }

    /** Whether all the sound has been heard; true until the next {@link #seek}. */
    public synchronized boolean ended() {
        MemorySegment h = open();
        return call(() -> (int) CApi.ffw_audio_ended.invokeExact(h), "ffw_audio_ended") != 0;
    }

    public synchronized boolean closed() {
        return handle == null;
    }

    /** Stop the sound, end its thread and free the decoder and the device. Closing twice does nothing. */
    @Override
    public synchronized void close() {
        MemorySegment h = handle;
        if (h == null) {
            return;
        }
        handle = null;
        call(() -> {
            CApi.ffw_audio_close.invokeExact(h);
            return null;
        }, "ffw_audio_close");
    }

    @Override
    public String toString() {
        return "Audio[" + file.getFileName() + ", " + codec + " " + channels + "ch " + sampleRate + " Hz"
                + (closed() ? ", closed" : "") + "]";
    }

    private MemorySegment open() {
        MemorySegment h = handle;
        if (h == null) {
            throw new VideoException("the sound of " + file + " is closed");
        }
        return h;
    }

    private interface Call<T> {
        T get() throws Throwable;
    }

    private static <T> T call(Call<T> call, String name) {
        try {
            return call.get();
        } catch (Throwable t) {
            throw Video.downcallFailed(name, t);
        }
    }
}
