package sibarum.ffmpeg;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.nio.file.Path;

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
        return new Video(handle, file);
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
        return new Audio(handle, file);
    }

    static String string(MemorySegment s) {
        return s == null || s.address() == 0 ? "" : s.reinterpret(Long.MAX_VALUE).getString(0);
    }

    private Ffmpeg() {
    }
}
