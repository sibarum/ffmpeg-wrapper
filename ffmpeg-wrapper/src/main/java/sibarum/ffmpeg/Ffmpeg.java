package sibarum.ffmpeg;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.nio.file.Path;
import java.util.function.Consumer;
import java.util.function.Function;

/**
 * Frames, from any video FFmpeg can decode: H.264, HEVC, VP8, VP9, AV1, MPEG-4, ProRes and the rest, in
 * MP4, MKV, WebM, MOV, AVI and the rest.
 *
 * <pre>{@code
 * try (Video video = Ffmpeg.open(path)) {
 *     VideoInfo info = video.info();           // size, length, rate, codec
 *     video.output(640, 360);                  // every frame scaled to this from now on
 *     for (Frame f; (f = video.next()) != null; ) {
 *         upload(f.rgba(), f.width(), f.height(), f.ptsMicros());
 *     }
 * }
 * }</pre>
 *
 * <p><b>The caller never learns a codec.</b> Every file opens the same way and every frame comes out as
 * RGBA8 at the size asked for, already converted from whatever YUV layout and matrix the stream used.
 * The codec's name is in {@link VideoInfo} to be shown, never to be branched on.
 *
 * <p><b>Sound is a second handle.</b> A {@code Video} reports that its file has sound ({@link VideoInfo#audio})
 * and does not decode it; {@link #openAudio} opens the same file's sound as an {@link Audio}, which plays it
 * out of a speaker and keeps the clock a player shows its frames by.
 *
 * <p><b>And the other way: frames into a file.</b> {@link #record} takes RGBA8 frames as they are made, each with
 * its own time, and writes AV1 in Matroska or WebM, fast enough to record a window while it runs.
 *
 * <p>Callers need {@code --enable-native-access=ALL-UNNAMED} (or their module) on the command line.
 */
public final class Ffmpeg {

    /** FFmpeg's own version string, for a log line. */
    public static String version() {
        try {
            return string((MemorySegment) CApi.ffw_version.invokeExact());
        } catch (Throwable t) {
            throw Video.downcallFailed("ffw_version", t);
        }
    }

    /**
     * Open {@code file} and its best video stream, ready to decode from the start. Throws
     * {@link VideoException} if it will not open, holds no video, or holds one nothing here decodes.
     * Blocking: it reads enough of the file to know its streams.
     */
    public static Video open(Path file) {
        if (file == null) {
            throw new VideoException("no file to open");
        }
        MemorySegment handle;
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment path = arena.allocateFrom(file.toAbsolutePath().toString());
            try {
                handle = (MemorySegment) CApi.ffw_open.invokeExact(path);
            } catch (Throwable t) {
                throw Video.downcallFailed("ffw_open", t);
            }
        }
        if (handle.address() == 0) {
            throw new VideoException("could not open " + file + ": " + Video.lastError());
        }
        return adopt(handle, h -> new Video(h, file), Ffmpeg::closeVideo);
    }

    /** {@link #openAudio(Path, Audio.Output) Open} {@code file}'s sound, to play out of the system's speakers. */
    public static Audio openAudio(Path file) {
        return openAudio(file, Audio.Output.SPEAKERS);
    }

    /**
     * Open {@code file}'s sound (the stream that goes with its best video, if it has one) and {@code output},
     * paused at the start. Throws {@link VideoException} if the file will not open, holds no sound or none
     * anything here decodes, or the output will not open (no sound device). Blocking: it reads enough of the
     * file to know its streams, and opens the device.
     */
    public static Audio openAudio(Path file, Audio.Output output) {
        if (file == null) {
            throw new VideoException("no file to open");
        }
        int where = output == Audio.Output.NOWHERE ? 1 : 0;
        MemorySegment handle;
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment path = arena.allocateFrom(file.toAbsolutePath().toString());
            try {
                handle = (MemorySegment) CApi.ffw_audio_open.invokeExact(path, where);
            } catch (Throwable t) {
                throw Video.downcallFailed("ffw_audio_open", t);
            }
        }
        if (handle.address() == 0) {
            throw new VideoException("could not open the sound of " + file + ": " + Video.lastError());
        }
        return adopt(handle, h -> new Audio(h, file), Ffmpeg::closeAudio);
    }

    /**
     * Create {@code file} and record into it: frames handed to the {@link Recording} as they are made, encoded to AV1
     * in the container the file's name asks for ({@code .mkv} or {@code .webm}), on a canvas of
     * {@code width x height}, the largest a frame will be (a smaller frame goes at its top left, on black). An
     * existing file is replaced. Throws {@link VideoException} if no encoder opens or the file cannot be created.
     */
    public static Recording record(Path file, int width, int height, Recording.Settings settings) {
        if (file == null) {
            throw new VideoException("no file to record to");
        }
        if (width <= 0 || height <= 0) {
            throw new VideoException("a zero-sized recording was asked for: " + width + "x" + height);
        }
        int encoder = settings.encoder() == Recording.Encoder.HARDWARE ? 1 : 0;
        MemorySegment handle;
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment path = arena.allocateFrom(file.toAbsolutePath().toString());
            try {
                handle = (MemorySegment) CApi.ffw_writer_open.invokeExact(path, width, height,
                        settings.frameRate(), settings.quality(), encoder);
            } catch (Throwable t) {
                throw Video.downcallFailed("ffw_writer_open", t);
            }
        }
        if (handle.address() == 0) {
            throw new VideoException("could not record to " + file + ": " + Video.lastError());
        }
        return adopt(handle, h -> new Recording(h, file, width, height), Ffmpeg::closeRecording);
    }

    /**
     * {@code wrap} a handle that has just opened, or, if wrapping it throws anything (a downcall failing while
     * the stream's facts are read, no memory left), {@code free} it before the throw goes on. The caller has
     * nothing to close until the wrapper exists, so until then this is the only place that can.
     */
    static <T> T adopt(MemorySegment handle, Function<MemorySegment, T> wrap, Consumer<MemorySegment> free) {
        try {
            return wrap.apply(handle);
        } catch (Throwable t) {
            try {
                free.accept(handle);
            } catch (Throwable u) {
                t.addSuppressed(u);
            }
            throw t;
        }
    }

    static void closeVideo(MemorySegment handle) {
        try {
            CApi.ffw_close.invokeExact(handle);
        } catch (Throwable t) {
            throw Video.downcallFailed("ffw_close", t);
        }
    }

    /** Finish and free a recording: 1 if its file is whole, else 0. */
    static int closeRecording(MemorySegment handle) {
        try {
            return (int) CApi.ffw_writer_close.invokeExact(handle);
        } catch (Throwable t) {
            throw Video.downcallFailed("ffw_writer_close", t);
        }
    }

    static void closeAudio(MemorySegment handle) {
        try {
            CApi.ffw_audio_close.invokeExact(handle);
        } catch (Throwable t) {
            throw Video.downcallFailed("ffw_audio_close", t);
        }
    }

    static String string(MemorySegment s) {
        return s == null || s.address() == 0 ? "" : s.reinterpret(Long.MAX_VALUE).getString(0);
    }

    private Ffmpeg() {
    }
}
