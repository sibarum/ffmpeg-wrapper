package sibarum.ffmpeg.record;

import sibarum.ffmpeg.spi.FfmpegRecordNatives;
import sibarum.ffmpeg.spi.NativeLibraries;

import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.SymbolLookup;
import java.lang.invoke.MethodHandle;

import static java.lang.foreign.ValueLayout.ADDRESS;
import static java.lang.foreign.ValueLayout.JAVA_DOUBLE;
import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_LONG;

/**
 * Raw FFM downcalls onto {@code native/record/ffrecord.h}. Hand-written, one line per function, in the header's
 * order — eight of them, which is the whole of the boundary. Lifetimes belong to {@link Recording}: nothing in this
 * class frees anything.
 *
 * <p>The shim and the encoding FFmpeg under it are loaded on first use, through {@link NativeLibraries}: from
 * {@code -Dffmpeg.record.dir}, then an {@code ffmpeg-record} folder beside the running executable (how a native image
 * ships them), then the classpath resources the OS module ({@link FfmpegRecordNatives}) names.
 */
final class RecordApi {

    private static final Linker LINKER = Linker.nativeLinker();
    private static final SymbolLookup LOOKUP;

    static {
        try {
            new NativeLibraries(FfmpegRecordNatives.class, "ffmpeg.record.dir", "ffmpeg-record", "ffmpeg-wrapper-record")
                    .load();
        } catch (IllegalStateException e) {
            throw new RecordingException(e.getMessage(), e.getCause());
        }
        LOOKUP = SymbolLookup.loaderLookup();
    }

    static final MethodHandle ffr_version = h("ffr_version", FunctionDescriptor.of(ADDRESS));
    static final MethodHandle ffr_open = h("ffr_open",
            FunctionDescriptor.of(ADDRESS, ADDRESS, JAVA_INT, JAVA_INT, JAVA_DOUBLE, JAVA_INT, JAVA_INT));
    static final MethodHandle ffr_encoder = h("ffr_encoder", FunctionDescriptor.of(ADDRESS, ADDRESS));
    static final MethodHandle ffr_frame =
            h("ffr_frame", FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, JAVA_INT, JAVA_INT, JAVA_INT, JAVA_LONG));
    static final MethodHandle ffr_repeat = h("ffr_repeat", FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_LONG));
    static final MethodHandle ffr_bytes = h("ffr_bytes", FunctionDescriptor.of(JAVA_LONG, ADDRESS));
    static final MethodHandle ffr_close = h("ffr_close", FunctionDescriptor.of(JAVA_INT, ADDRESS));
    static final MethodHandle ffr_last_error = h("ffr_last_error", FunctionDescriptor.of(ADDRESS));

    private static MethodHandle h(String name, FunctionDescriptor desc) {
        return LINKER.downcallHandle(
                LOOKUP.find(name).orElseThrow(() -> new RecordingException("symbol not found: " + name)),
                desc);
    }

    private RecordApi() {
    }
}
