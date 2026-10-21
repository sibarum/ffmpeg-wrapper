package sibarum.ffmpeg;

import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.SymbolLookup;
import java.lang.invoke.MethodHandle;

import static java.lang.foreign.ValueLayout.ADDRESS;
import static java.lang.foreign.ValueLayout.JAVA_DOUBLE;
import static java.lang.foreign.ValueLayout.JAVA_FLOAT;
import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_LONG;

/**
 * Raw FFM downcalls onto {@code native/include/ffwrap.h}. Hand-written, one line per function, in the
 * header's order — thirty-three of them, which is the whole of the boundary.
 *
 * <p>{@code int64_t} is bound as {@code JAVA_LONG} and every {@code uint32_t} as {@code JAVA_INT}; a
 * frame side or sample rate that overflows a signed int is one no decoder would have produced. Lifetimes
 * belong to {@link Video}, {@link Audio} and {@link Recording}, not here: nothing in this class frees anything.
 */
final class CApi {

    private static final Linker LINKER = Linker.nativeLinker();
    private static final SymbolLookup LOOKUP;

    static {
        LOOKUP = NativeLib.load();
    }

    static final MethodHandle ffw_version = h("ffw_version", FunctionDescriptor.of(ADDRESS));

    // opening, and what was opened
    static final MethodHandle ffw_open = h("ffw_open", FunctionDescriptor.of(ADDRESS, ADDRESS));
    static final MethodHandle ffw_width = h("ffw_width", FunctionDescriptor.of(JAVA_INT, ADDRESS));
    static final MethodHandle ffw_height = h("ffw_height", FunctionDescriptor.of(JAVA_INT, ADDRESS));
    static final MethodHandle ffw_duration_us = h("ffw_duration_us", FunctionDescriptor.of(JAVA_LONG, ADDRESS));
    static final MethodHandle ffw_frame_rate = h("ffw_frame_rate", FunctionDescriptor.of(JAVA_DOUBLE, ADDRESS));
    static final MethodHandle ffw_codec = h("ffw_codec", FunctionDescriptor.of(ADDRESS, ADDRESS));
    static final MethodHandle ffw_container = h("ffw_container", FunctionDescriptor.of(ADDRESS, ADDRESS));
    static final MethodHandle ffw_has_audio = h("ffw_has_audio", FunctionDescriptor.of(JAVA_INT, ADDRESS));

    // decoding
    static final MethodHandle ffw_output =
            h("ffw_output", FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT, JAVA_INT));
    static final MethodHandle ffw_next = h("ffw_next", FunctionDescriptor.of(JAVA_INT, ADDRESS));
    static final MethodHandle ffw_pts_us = h("ffw_pts_us", FunctionDescriptor.of(JAVA_LONG, ADDRESS));
    static final MethodHandle ffw_pixels = h("ffw_pixels", FunctionDescriptor.of(ADDRESS, ADDRESS));
    static final MethodHandle ffw_seek = h("ffw_seek", FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_LONG));
    static final MethodHandle ffw_close = h("ffw_close", FunctionDescriptor.ofVoid(ADDRESS));

    // sound
    static final MethodHandle ffw_audio_open =
            h("ffw_audio_open", FunctionDescriptor.of(ADDRESS, ADDRESS, JAVA_INT));
    static final MethodHandle ffw_audio_codec = h("ffw_audio_codec", FunctionDescriptor.of(ADDRESS, ADDRESS));
    static final MethodHandle ffw_audio_channels = h("ffw_audio_channels", FunctionDescriptor.of(JAVA_INT, ADDRESS));
    static final MethodHandle ffw_audio_rate = h("ffw_audio_rate", FunctionDescriptor.of(JAVA_INT, ADDRESS));
    static final MethodHandle ffw_audio_play = h("ffw_audio_play", FunctionDescriptor.of(JAVA_INT, ADDRESS));
    static final MethodHandle ffw_audio_pause = h("ffw_audio_pause", FunctionDescriptor.of(JAVA_INT, ADDRESS));
    static final MethodHandle ffw_audio_seek =
            h("ffw_audio_seek", FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_LONG));
    static final MethodHandle ffw_audio_volume =
            h("ffw_audio_volume", FunctionDescriptor.ofVoid(ADDRESS, JAVA_FLOAT));
    static final MethodHandle ffw_audio_clock_us = h("ffw_audio_clock_us", FunctionDescriptor.of(JAVA_LONG, ADDRESS));
    static final MethodHandle ffw_audio_ended = h("ffw_audio_ended", FunctionDescriptor.of(JAVA_INT, ADDRESS));
    static final MethodHandle ffw_audio_close = h("ffw_audio_close", FunctionDescriptor.ofVoid(ADDRESS));

    // recording
    static final MethodHandle ffw_writer_open = h("ffw_writer_open",
            FunctionDescriptor.of(ADDRESS, ADDRESS, JAVA_INT, JAVA_INT, JAVA_DOUBLE, JAVA_INT, JAVA_INT));
    static final MethodHandle ffw_writer_encoder = h("ffw_writer_encoder", FunctionDescriptor.of(ADDRESS, ADDRESS));
    static final MethodHandle ffw_writer_frame =
            h("ffw_writer_frame", FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, JAVA_INT, JAVA_INT, JAVA_INT, JAVA_LONG));
    static final MethodHandle ffw_writer_repeat =
            h("ffw_writer_repeat", FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_LONG));
    static final MethodHandle ffw_writer_bytes = h("ffw_writer_bytes", FunctionDescriptor.of(JAVA_LONG, ADDRESS));
    static final MethodHandle ffw_writer_close = h("ffw_writer_close", FunctionDescriptor.of(JAVA_INT, ADDRESS));

    static final MethodHandle ffw_last_error = h("ffw_last_error", FunctionDescriptor.of(ADDRESS));

    private static MethodHandle h(String name, FunctionDescriptor desc) {
        return LINKER.downcallHandle(
                LOOKUP.find(name).orElseThrow(() -> new VideoException("symbol not found: " + name)),
                desc);
    }

    private CApi() {
    }
}
