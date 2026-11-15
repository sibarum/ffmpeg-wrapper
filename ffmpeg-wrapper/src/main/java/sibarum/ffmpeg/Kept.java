package sibarum.ffmpeg;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;

import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_LONG;

/**
 * A decoded frame kept in memory its video was lent ({@link Video#stageInto}), by handle: a reference to the frame,
 * which keeps its block from being decoded into again until it is {@linkplain #release released}. A handle is a
 * plain {@code long}, so it can be passed on (to whatever reads the block, as a token) and given back without an
 * object to carry it. Every method may be called from any thread, and a handle outlives its video: releasing one
 * after the video is closed is how the last of its blocks come free.
 */
public final class Kept {

    private Kept() {
    }

    /** Another reference to the frame {@code kept} refers to, released on its own; 0 when memory ran out. */
    public static long ref(long kept) {
        MemorySegment k = segment(kept);
        try {
            return ((MemorySegment) CApi.ffw_kept_ref.invokeExact(k)).address();
        } catch (Throwable t) {
            throw Video.downcallFailed("ffw_kept_ref", t);
        }
    }

    /** Let go of {@code kept}: once nothing else refers to its frame, its block is free to decode into. */
    public static void release(long kept) {
        MemorySegment k = segment(kept);
        try {
            CApi.ffw_kept_release.invokeExact(k);
        } catch (Throwable t) {
            throw Video.downcallFailed("ffw_kept_release", t);
        }
    }

    /**
     * Where {@code kept} is: the id of the chunk it was decoded or copied into, as {@link Video#stageInto} named it,
     * with each plane's offset into that chunk and row stride, in bytes, written into {@code offsets} and
     * {@code strides} (four each; 0 past the last plane).
     */
    public static int place(long kept, long[] offsets, int[] strides) {
        if (offsets.length < 4 || strides.length < 4) {
            throw new IllegalArgumentException("a frame has up to four planes");
        }
        MemorySegment k = segment(kept);
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment o = arena.allocate(JAVA_LONG, 4);
            MemorySegment s = arena.allocate(JAVA_INT, 4);
            int chunk = (int) CApi.ffw_kept_place.invokeExact(k, o, s);
            if (chunk < 0) {
                throw new VideoException(Video.lastError());
            }
            for (int i = 0; i < 4; i++) {
                offsets[i] = o.getAtIndex(JAVA_LONG, i);
                strides[i] = s.getAtIndex(JAVA_INT, i);
            }
            return chunk;
        } catch (Throwable t) {
            throw Video.downcallFailed("ffw_kept_place", t);
        }
    }

    private static MemorySegment segment(long kept) {
        if (kept == 0) {
            throw new IllegalArgumentException("no kept frame");
        }
        return MemorySegment.ofAddress(kept);
    }
}
