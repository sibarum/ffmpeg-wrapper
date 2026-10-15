package sibarum.ffmpeg;

import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.nio.file.Path;

/**
 * An open video file, decoded forward one frame at a time from wherever it was last {@linkplain #seek
 * sought}. Made by {@link Ffmpeg#open}; closed with {@link #close}, which a try-with-resources does.
 *
 * <p><b>One thread at a time.</b> A decoder carries state from frame to frame, so a {@code Video} is
 * used by one thread at a time (a player's decoding thread, say) and handed between threads with the
 * usual happens-before. Different videos on different threads do not interact.
 *
 * <p>Every frame is copied out of native memory as it is returned, so nothing this class hands back
 * depends on the file staying open.
 */
public final class Video implements AutoCloseable {

    private final Path file;
    private final VideoInfo info;
    private MemorySegment handle;
    private int outWidth;
    private int outHeight;

    Video(MemorySegment handle, Path file) {
        this.handle = handle;
        this.file = file;
        this.info = new VideoInfo(
                call(() -> (int) CApi.ffw_width.invokeExact(handle), "ffw_width"),
                call(() -> (int) CApi.ffw_height.invokeExact(handle), "ffw_height"),
                call(() -> (long) CApi.ffw_duration_us.invokeExact(handle), "ffw_duration_us"),
                call(() -> (double) CApi.ffw_frame_rate.invokeExact(handle), "ffw_frame_rate"),
                Ffmpeg.string(call(() -> (MemorySegment) CApi.ffw_codec.invokeExact(handle), "ffw_codec")),
                Ffmpeg.string(call(() -> (MemorySegment) CApi.ffw_container.invokeExact(handle), "ffw_container")),
                call(() -> (int) CApi.ffw_has_audio.invokeExact(handle), "ffw_has_audio") != 0);
        this.outWidth = info.width();
        this.outHeight = info.height();
    }

    /** What the file said about itself when it was opened. */
    public VideoInfo info() {
        return info;
    }

    /** The file this was opened from. */
    public Path file() {
        return file;
    }

    /** The size frames come out at: {@link VideoInfo#width} x {@link VideoInfo#height} until {@link #output}. */
    public int outputWidth() {
        return outWidth;
    }

    public int outputHeight() {
        return outHeight;
    }

    /**
     * Scale every frame from the next one on to {@code width x height} px, filtered as it shrinks. Asking for
     * the size a frame is shown at costs nothing more than any size does, but a thumbnail that asks for its
     * own size saves a copy of every frame at full size.
     */
    public Video output(int width, int height) {
        MemorySegment h = open();
        if (width <= 0 || height <= 0) {
            throw new VideoException("a zero-sized frame was asked for: " + width + "x" + height);
        }
        int ok = call(() -> (int) CApi.ffw_output.invokeExact(h, width, height), "ffw_output");
        if (ok == 0) {
            throw new VideoException("could not set the output to " + width + "x" + height + ": " + lastError());
        }
        outWidth = width;
        outHeight = height;
        return this;
    }

    /** The next frame, or null at the end of the stream. Blocking: it decodes. */
    public Frame next() {
        MemorySegment h = open();
        int got = call(() -> (int) CApi.ffw_next.invokeExact(h), "ffw_next");
        if (got == 0) {
            return null;
        }
        if (got < 0) {
            throw new VideoException("could not decode " + file + ": " + lastError());
        }
        long pts = call(() -> (long) CApi.ffw_pts_us.invokeExact(h), "ffw_pts_us");
        MemorySegment pixels = call(() -> (MemorySegment) CApi.ffw_pixels.invokeExact(h), "ffw_pixels");
        byte[] rgba = pixels.reinterpret((long) outWidth * outHeight * 4L).toArray(ValueLayout.JAVA_BYTE);
        return new Frame(outWidth, outHeight, pts, rgba);
    }

    /**
     * Go to {@code micros} from the stream's start: the next {@link #next} gives the frame showing then.
     * Exact, not to the keyframe before it — the frames between are decoded and dropped, so a seek costs
     * up to one keyframe interval of decoding.
     */
    public void seek(long micros) {
        MemorySegment h = open();
        int ok = call(() -> (int) CApi.ffw_seek.invokeExact(h, micros), "ffw_seek");
        if (ok == 0) {
            throw new VideoException("could not seek " + file + " to " + micros + "us: " + lastError());
        }
    }

    public boolean closed() {
        return handle == null;
    }

    /** Close the file and free the decoder. Closing twice does nothing. */
    @Override
    public void close() {
        MemorySegment h = handle;
        if (h == null) {
            return;
        }
        handle = null;
        try {
            CApi.ffw_close.invokeExact(h);
        } catch (Throwable t) {
            throw downcallFailed("ffw_close", t);
        }
    }

    @Override
    public String toString() {
        return "Video[" + file.getFileName() + ", " + info.codec() + " " + info.width() + "x" + info.height()
                + (closed() ? ", closed" : "") + "]";
    }

    private MemorySegment open() {
        MemorySegment h = handle;
        if (h == null) {
            throw new VideoException(file + " is closed");
        }
        return h;
    }

    /** FFmpeg's own account of the last failure on this thread, or a stand-in. */
    static String lastError() {
        MemorySegment message;
        try {
            message = (MemorySegment) CApi.ffw_last_error.invokeExact();
        } catch (Throwable t) {
            return "(the error string could not be read: " + t + ")";
        }
        if (message == null || message.address() == 0) {
            return "(FFmpeg gave no reason)";
        }
        return Ffmpeg.string(message);
    }

    private interface Call<T> {
        T get() throws Throwable;
    }

    private static <T> T call(Call<T> call, String name) {
        try {
            return call.get();
        } catch (Throwable t) {
            throw downcallFailed(name, t);
        }
    }

    /**
     * What a failed downcall throws. A {@link LinkageError} (the libraries would not load, so {@code CApi}
     * never initialised) is rethrown as it is: it says the decoder is missing, not that the file is bad,
     * and a caller tells the two apart by it.
     */
    static VideoException downcallFailed(String name, Throwable cause) {
        if (cause instanceof LinkageError e) {
            throw e;
        }
        if (cause instanceof VideoException e) {
            return e;
        }
        return new VideoException("the downcall to " + name + " failed", cause);
    }
}
