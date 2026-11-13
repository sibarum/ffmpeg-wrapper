package sibarum.ffmpeg;

import java.lang.foreign.MemorySegment;

/**
 * A decoded frame as the decoder made it, before any conversion: what {@link Video#planes} gives, for a
 * converter that is not FFmpeg's (a GPU's, which samples the planes and applies the matrix itself).
 *
 * @param format    the pixel format by FFmpeg's name: {@code "yuv420p"}, {@code "nv12"}, {@code "yuv420p10le"}
 * @param width     the coded width in px, before any sample aspect
 * @param height    the coded height in px
 * @param matrix    the YUV matrix, as {@link Video#convert(byte[])} would apply it: 601, 709 or 2020, 0 for RGB
 *                  or another
 * @param fullRange whether the values span the full range, rather than the limited one most video uses
 * @param data      each plane's bytes: native memory owned by the {@link Video}, valid only until its next
 *                  {@code decode}, {@code next}, {@code seek} or {@code close}
 * @param strides   each plane's row stride in bytes
 */
public record Planes(String format, int width, int height, int matrix, boolean fullRange,
                     MemorySegment[] data, int[] strides) {

    /** How many planes: 3 for {@code yuv420p}, 2 for {@code nv12}. */
    public int count() {
        return data.length;
    }

    @Override
    public String toString() {
        return "Planes[" + format + " " + width + "x" + height + ", BT." + matrix + (fullRange ? " full" : "") + "]";
    }
}
