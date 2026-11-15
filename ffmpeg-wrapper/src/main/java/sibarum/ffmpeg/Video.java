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
     * Decode the next frame and hold it as decoded, converting nothing: its time from the stream's start in
     * µs, or -1 at the end of the stream. A frame that will be dropped costs only its decoding this way, a
     * fraction of what {@link #next} costs, which also scales it and copies it out. One that will be shown is
     * then {@linkplain #convert(byte[]) converted}, or read as it is ({@link #planes}). Held until the next
     * {@code decode}, {@code next} or {@code seek}. Blocking: it decodes.
     */
    public long decode() {
        MemorySegment h = open();
        int got = call(() -> (int) CApi.ffw_decode.invokeExact(h), "ffw_decode");
        if (got == 0) {
            return -1;
        }
        if (got < 0) {
            throw new VideoException("could not decode " + file + ": " + lastError());
        }
        return call(() -> (long) CApi.ffw_pts_us.invokeExact(h), "ffw_pts_us");
    }

    /**
     * Scale and convert the frame {@link #decode} holds to RGBA at the {@linkplain #output output size}, in one
     * threaded pass, straight into {@code rgba}: at least {@code outputWidth * outputHeight * 4} bytes, rows
     * packed. Nothing is copied after. The array is reached in place, so no garbage collection runs on the
     * calling thread meanwhile (a few ms at 1080p).
     */
    public void convert(byte[] rgba) {
        MemorySegment h = open();
        if (rgba.length < (long) outWidth * outHeight * 4L) {
            throw new IllegalArgumentException(rgba.length + " bytes for a " + outWidth + "x" + outHeight + " frame");
        }
        MemorySegment dst = MemorySegment.ofArray(rgba);
        int stride = outWidth * 4;
        int ok = call(() -> (int) CApi.ffw_convert_heap.invokeExact(h, dst, stride), "ffw_convert");
        if (ok == 0) {
            throw new VideoException("could not convert a frame of " + file + ": " + lastError());
        }
    }

    /**
     * {@link #convert(byte[])} into native memory, rows {@code stride} bytes apart (at least
     * {@code outputWidth * 4}): a mapped staging buffer's, say.
     */
    public void convert(MemorySegment dst, int stride) {
        MemorySegment h = open();
        if (stride < outWidth * 4 || dst.byteSize() < (long) stride * (outHeight - 1) + outWidth * 4L) {
            throw new IllegalArgumentException(dst.byteSize() + " bytes at a stride of " + stride + " for a "
                    + outWidth + "x" + outHeight + " frame");
        }
        int ok = call(() -> (int) CApi.ffw_convert.invokeExact(h, dst, stride), "ffw_convert");
        if (ok == 0) {
            throw new VideoException("could not convert a frame of " + file + ": " + lastError());
        }
    }

    /**
     * The frame {@link #decode} holds, as the decoder made it: for a converter elsewhere, a GPU's. Its planes
     * are native memory owned by this video and valid only until the next {@code decode}, {@code next},
     * {@code seek} or {@code close}: copy or upload them before. Null when no frame is held.
     */
    public Planes planes() {
        MemorySegment h = open();
        MemorySegment name = call(() -> (MemorySegment) CApi.ffw_frame_format.invokeExact(h), "ffw_frame_format");
        if (name == null || name.address() == 0) {
            return null;
        }
        int n = call(() -> (int) CApi.ffw_frame_planes.invokeExact(h), "ffw_frame_planes");
        MemorySegment[] data = new MemorySegment[n];
        int[] strides = new int[n];
        for (int i = 0; i < n; i++) {
            int plane = i;
            int stride = call(() -> (int) CApi.ffw_plane_stride.invokeExact(h, plane), "ffw_plane_stride");
            int rows = call(() -> (int) CApi.ffw_plane_rows.invokeExact(h, plane), "ffw_plane_rows");
            MemorySegment p = call(() -> (MemorySegment) CApi.ffw_plane.invokeExact(h, plane), "ffw_plane");
            strides[i] = stride;
            data[i] = p.reinterpret((long) Math.abs(stride) * rows);
        }
        return new Planes(Ffmpeg.string(name),
                call(() -> (int) CApi.ffw_frame_width.invokeExact(h), "ffw_frame_width"),
                call(() -> (int) CApi.ffw_frame_height.invokeExact(h), "ffw_frame_height"),
                call(() -> (int) CApi.ffw_frame_matrix.invokeExact(h), "ffw_frame_matrix"),
                call(() -> (int) CApi.ffw_frame_full_range.invokeExact(h), "ffw_frame_full_range") != 0,
                data, strides);
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

    /**
     * Lend {@code chunk} to the decoder, named {@code id}: native memory, 64-byte aligned, that frames are decoded
     * straight into from now (a GPU's mapped staging, say, so a decoded frame is already where the GPU copies it
     * from). Each frame takes a block of a chunk, laid out as the decoder wants; {@link #stageBlock} says how large
     * once the first frame is decoded, and {@link #stageRoom} how many can still be had. The chunk must stay mapped
     * until this video is closed and every frame {@linkplain #keep kept} from it is {@linkplain Kept#release
     * released}. At most 64 chunks.
     */
    public void stageInto(MemorySegment chunk, int id) {
        MemorySegment h = open();
        long bytes = chunk.byteSize();
        int ok = call(() -> (int) CApi.ffw_stage_into.invokeExact(h, chunk, bytes, id), "ffw_stage_into");
        if (ok == 0) {
            throw new VideoException("could not stage " + file + " into " + bytes + " bytes: " + lastError());
        }
    }

    /** The bytes one frame's block takes in the staging, or 0 until the frames' layout is known. */
    public long stageBlock() {
        MemorySegment h = open();
        return call(() -> (long) CApi.ffw_stage_block.invokeExact(h), "ffw_stage_block");
    }

    /** How many more blocks the staging can give, free or not yet cut; -1 until the frames' layout is known. */
    public int stageRoom() {
        MemorySegment h = open();
        return call(() -> (int) CApi.ffw_stage_room.invokeExact(h), "ffw_stage_room");
    }

    /**
     * How many frames were kept by copying them into a block rather than being decoded there: every frame of a
     * decoder that cannot decode into memory it is given (dav1d's AV1), and any decoded while no block was free.
     */
    public long stageCopies() {
        MemorySegment h = open();
        return call(() -> (long) CApi.ffw_stage_copies.invokeExact(h), "ffw_stage_copies");
    }

    /**
     * The frame {@link #decode} holds, kept in the staging ({@link #stageInto}): a handle to it ({@link Kept}),
     * which stays valid past the next decode and past this video's close, until released. No copy when the frame was
     * decoded into a block; otherwise copied into one now. 0 when no block is free, or the frame does not fit one
     * ({@link #lastError} says which).
     */
    public long keep() {
        MemorySegment h = open();
        MemorySegment k = call(() -> (MemorySegment) CApi.ffw_keep.invokeExact(h), "ffw_keep");
        return k == null ? 0 : k.address();
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
        Ffmpeg.closeVideo(h);
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
    public static String lastError() {
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
