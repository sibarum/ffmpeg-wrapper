package sibarum.ffmpeg;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.nio.file.Path;

/**
 * A video being recorded: frames handed in as they are made, encoded to AV1 and written to a Matroska or WebM file.
 * Made by {@link Ffmpeg#record}; finished with {@link #close}, which a try-with-resources does.
 *
 * <pre>{@code
 * try (Recording rec = Ffmpeg.record(Path.of("window.mkv"), 1920, 1080, Recording.Settings.DEFAULT)) {
 *     rec.frame(rgba, presentedMicros);       // as each frame is shown
 * }                                           // the file is whole once this returns
 * }</pre>
 *
 * <p><b>Each frame brings its own time.</b> The container stamps every frame, so frames need not come at a steady
 * rate: a window that draws nothing while idle is recorded as it was, its pauses as long as they were and no frame
 * repeated. Times rise; a frame no later than the last one's millisecond, Matroska's resolution, is dropped.
 *
 * <p><b>A canvas, and frames of any size on it.</b> The recording's size is the canvas, and every frame in the file
 * is that size. A frame handed in may be smaller (a window made smaller while it is recorded): it goes at the top
 * left, unscaled, on black. One that is larger is cut to the canvas. So the canvas is the largest a frame will be,
 * such as the screen's, and a window resized mid-recording keeps every pixel sharp in one stream that every player
 * plays. Black costs almost nothing to encode.
 *
 * <p><b>Real time.</b> {@link Encoder#SOFTWARE} is SVT-AV1 in its real-time mode, which keeps up with a 1080p
 * window at 60 fps on under a core of a laptop CPU; {@link Encoder#HARDWARE} asks for the GPU's encoder and falls
 * back to SVT-AV1 where there is none. {@link #frame} encodes before it returns, so a caller that must not wait
 * (a render loop) hands frames to a thread of its own that calls it.
 *
 * <p><b>One thread at a time</b>, as a {@link Video} is. Pixels are what a {@link Frame} holds: straight RGBA8,
 * sRGB, tightly packed, top row first.
 */
public final class Recording implements AutoCloseable {

    /** Which AV1 encoder records. */
    public enum Encoder {
        /** SVT-AV1, real-time mode: on every machine, and the same file on each. */
        SOFTWARE,
        /** The GPU's AV1 encoder (NVENC, Quick Sync, AMF), whichever opens first; SVT-AV1 where none does. */
        HARDWARE
    }

    /** The byte order of a frame's four bytes a pixel. */
    public enum Pixels {
        /** R, G, B, A: what a {@link Frame} holds. */
        RGBA,
        /** B, G, R, A: what a swapchain or a DXGI back buffer holds, so a presented frame read back as it is. */
        BGRA
    }

    /**
     * How a recording is encoded.
     *
     * @param frameRate about how many frames a second will come: a hint for the encoder's rate control and its
     *                  keyframe interval, never a schedule
     * @param quality   AV1's quantiser scale, 0 (best) to 63 (smallest); {@value #TEXT} keeps an interface's text
     *                  sharp
     * @param encoder   software or the GPU's
     */
    public record Settings(double frameRate, int quality, Encoder encoder) {

        /** The quality that keeps small text legible. */
        public static final int TEXT = 30;

        /** A window at up to 60 fps, text sharp, encoded in software. */
        public static final Settings DEFAULT = new Settings(60, TEXT, Encoder.SOFTWARE);

        public Settings {
            if (!(frameRate > 0 && frameRate <= 1000)) {
                throw new IllegalArgumentException("frame rate " + frameRate + " is not in (0, 1000]");
            }
            if (quality < 0 || quality > 63) {
                throw new IllegalArgumentException("quality " + quality + " is not in [0, 63]");
            }
            if (encoder == null) {
                throw new IllegalArgumentException("no encoder");
            }
        }

        public Settings withEncoder(Encoder encoder) {
            return new Settings(frameRate, quality, encoder);
        }
    }

    private final Path file;
    private final int width;
    private final int height;
    private final String encoder;
    /** Where a frame given as an array is copied for the encoder: the canvas's size, grown for a larger frame. */
    private Arena arena = Arena.ofShared();
    private MemorySegment staging;
    private MemorySegment handle;
    private long frames;
    private long dropped;

    Recording(MemorySegment handle, Path file, int width, int height) {
        this.handle = handle;
        this.file = file;
        this.width = width;
        this.height = height;
        this.encoder = Ffmpeg.string(call(() -> (MemorySegment) CApi.ffw_writer_encoder.invokeExact(handle),
                "ffw_writer_encoder"));
        this.staging = arena.allocate((long) width * height * 4L);
    }

    /** The file being written. */
    public Path file() {
        return file;
    }

    /** The canvas's width: every frame in the file is this wide. */
    public int width() {
        return width;
    }

    /** The canvas's height. */
    public int height() {
        return height;
    }

    /** The encoder that opened: {@code "libsvtav1"}, or a GPU's, such as {@code "av1_nvenc"}. To be shown. */
    public String encoder() {
        return encoder;
    }

    /** Frames encoded so far. */
    public long frames() {
        return frames;
    }

    /** Frames dropped for coming no later than the last one's millisecond. */
    public long dropped() {
        return dropped;
    }

    /** Bytes written to the file so far; what the encoder still holds is written by {@link #close}. */
    public long bytes() {
        MemorySegment h = open();
        return call(() -> (long) CApi.ffw_writer_bytes.invokeExact(h), "ffw_writer_bytes");
    }

    /** Encode {@code frame} at its own size, as {@link #frame(byte[], int, int, long)}. */
    public boolean frame(Frame frame) {
        return frame(frame.rgba(), frame.width(), frame.height(), frame.ptsMicros());
    }

    /** Encode a frame the canvas's size, as {@link #frame(byte[], int, int, long)}. */
    public boolean frame(byte[] rgba, long ptsMicros) {
        return frame(rgba, width, height, ptsMicros);
    }

    /**
     * Encode {@code rgba}, a {@code frameWidth x frameHeight} frame ({@code frameWidth * frameHeight * 4} bytes),
     * shown at {@code ptsMicros} from the recording's start: at the canvas's top left, the rest black, the part
     * outside cut off. True if it was encoded; false if it was dropped for coming no later than the last frame's
     * millisecond. Blocking: it encodes.
     */
    public boolean frame(byte[] rgba, int frameWidth, int frameHeight, long ptsMicros) {
        long size = bytes(frameWidth, frameHeight);
        if (rgba == null || rgba.length != size) {
            throw new VideoException("a frame of " + (rgba == null ? 0 : rgba.length) + " bytes, where "
                    + frameWidth + "x" + frameHeight + " is " + size);
        }
        open();
        if (size > staging.byteSize()) {
            Arena larger = Arena.ofShared();
            MemorySegment grown = larger.allocate(size);
            arena.close();
            arena = larger;
            staging = grown;
        }
        MemorySegment.copy(rgba, 0, staging, ValueLayout.JAVA_BYTE, 0, rgba.length);
        return frame(staging, frameWidth, frameHeight, Pixels.RGBA, ptsMicros);
    }

    /**
     * Encode the {@code frameWidth * frameHeight * 4} bytes at {@code pixels}, native memory such as a mapped readback
     * buffer, in {@code order}, shown at {@code ptsMicros}; read before this returns and never kept, so a presented
     * frame is encoded where the GPU left it, with no copy. As {@link #frame(byte[], int, int, long)}.
     */
    public boolean frame(MemorySegment pixels, int frameWidth, int frameHeight, Pixels order, long ptsMicros) {
        MemorySegment h = open();
        long size = bytes(frameWidth, frameHeight);
        if (pixels == null || !pixels.isNative() || pixels.byteSize() < size) {
            throw new VideoException("a " + frameWidth + "x" + frameHeight + " frame must be native memory of at least "
                    + size + " bytes");
        }
        int bgra = order == Pixels.BGRA ? 1 : 0;
        int got = call(() -> (int) CApi.ffw_writer_frame.invokeExact(h, pixels, frameWidth, frameHeight, bgra,
                ptsMicros), "ffw_writer_frame");
        return counted(got);
    }

    /**
     * Encode the last frame again, shown at {@code ptsMicros}: a picture that stayed on the screen, so the file lasts
     * until then instead of ending at the last change. What a recording stopped while its window was still does at the
     * end. True if it was encoded; false if there is no frame yet, or the time is no later than the last frame's
     * millisecond. It encodes to almost nothing.
     */
    public boolean repeat(long ptsMicros) {
        MemorySegment h = open();
        return counted(call(() -> (int) CApi.ffw_writer_repeat.invokeExact(h, ptsMicros), "ffw_writer_repeat"));
    }

    private boolean counted(int got) {
        if (got < 0) {
            throw new VideoException("could not record to " + file + ": " + Video.lastError());
        }
        if (got == 0) {
            dropped++;
            return false;
        }
        frames++;
        return true;
    }

    public boolean closed() {
        return handle == null;
    }

    /**
     * Encode what the encoder still holds and finish the file. Throws {@link VideoException} if it could not be
     * finished (a full disk), when what was written may not play; everything is freed all the same. Closing twice
     * does nothing.
     */
    @Override
    public void close() {
        MemorySegment h = handle;
        if (h == null) {
            return;
        }
        handle = null;
        int ok;
        try {
            ok = Ffmpeg.closeRecording(h);
        } finally {
            arena.close();
        }
        if (ok == 0) {
            throw new VideoException("could not finish " + file + ": " + Video.lastError());
        }
    }

    @Override
    public String toString() {
        return "Recording[" + file.getFileName() + ", " + encoder + " " + width + "x" + height + ", " + frames
                + " frames" + (closed() ? ", closed" : "") + "]";
    }

    private static long bytes(int frameWidth, int frameHeight) {
        if (frameWidth <= 0 || frameHeight <= 0 || frameWidth > 16384 || frameHeight > 16384) {
            throw new VideoException("a " + frameWidth + "x" + frameHeight + " frame");
        }
        return (long) frameWidth * frameHeight * 4L;
    }

    private MemorySegment open() {
        MemorySegment h = handle;
        if (h == null) {
            throw new VideoException(file + " is closed");
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
