package sibarum.ffmpeg;

import sibarum.ffmpeg.spi.FfmpegNatives;

import java.io.IOException;
import java.io.InputStream;
import java.lang.foreign.SymbolLookup;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.List;
import java.util.ServiceLoader;

/**
 * Locates and loads the {@code ffwrap} shim and the FFmpeg libraries under it.
 *
 * <p>Search order: the {@code ffmpeg.native.dir} system property (a directory laid out as
 * {@code native/build.sh} stages one), then an {@code ffmpeg} folder beside the running executable laid
 * out the same way (how a native image ships them), then the classpath resources the OS module on the
 * class path names ({@link FfmpegNatives}), extracted together to one temp directory. Loading is
 * idempotent and thread-safe. This class holds no platform knowledge of its own.
 *
 * <p><b>Six libraries, in an order.</b> Unlike imagelib, which is one library because its decoders are
 * Rust, this is FFmpeg's own shared libraries and a shim over them, and a library loaded by path finds
 * the ones it links only if they are already loaded. {@code libs.txt}, written by the build beside them,
 * lists each after the ones it needs, so the order and the version numbers in the file names are the
 * build's to know and never this class's.
 */
final class NativeLib {

    /** The folder beside an executable that {@link #besideExecutable} looks in. */
    static final String BESIDE = "ffmpeg";

    private static volatile SymbolLookup lookup;

    static synchronized SymbolLookup load() {
        if (lookup != null) {
            return lookup;
        }
        String dir = System.getProperty("ffmpeg.native.dir");
        try {
            Path beside = dir == null ? besideExecutable() : null;
            Path base = dir != null ? Path.of(dir) : beside != null ? beside : extractBundled();
            for (String name : libraries(Files.readString(base.resolve("libs.txt"), StandardCharsets.UTF_8))) {
                System.load(base.resolve(name).toAbsolutePath().toString());
            }
        } catch (IOException | UnsatisfiedLinkError e) {
            throw new VideoException("failed to load the FFmpeg native libraries for " + platform(), e);
        }
        lookup = SymbolLookup.loaderLookup();
        return lookup;
    }

    /** What the OS module calls this platform, for messages; {@code unknown} when there is no OS module. */
    static String platform() {
        FfmpegNatives natives = natives();
        return natives == null ? "unknown (no ffmpeg-wrapper OS module on the class path)" : natives.platform();
    }

    /** The one OS module on the class path, or null when there is none. */
    private static FfmpegNatives natives() {
        return ServiceLoader.load(FfmpegNatives.class).findFirst().orElse(null);
    }

    /** The file names in {@code libs.txt}, in its order: blank lines and surrounding space ignored. */
    static List<String> libraries(String list) {
        List<String> names = new ArrayList<>();
        for (String line : list.split("\\R")) {
            String name = line.strip();
            if (!name.isEmpty()) {
                names.add(name);
            }
        }
        return names;
    }

    /**
     * An {@value #BESIDE} folder next to the running executable, holding a {@code libs.txt}: where a native
     * image's installer puts the libraries, since they are not inside it. Null when there is none, which
     * on a JVM is always (the executable is {@code java}).
     */
    static Path besideExecutable() {
        try {
            Path exe = ProcessHandle.current().info().command().map(Path::of).orElse(null);
            Path folder = exe == null || exe.getParent() == null ? null : exe.getParent().resolve(BESIDE);
            return folder != null && Files.isRegularFile(folder.resolve("libs.txt")) ? folder : null;
        } catch (RuntimeException e) {
            return null;
        }
    }

    private static Path extractBundled() throws IOException {
        FfmpegNatives natives = natives();
        String root = natives == null ? null : natives.resourceRoot();
        if (root == null) {
            throw new IOException("no bundled FFmpeg for " + platform()
                    + " (add the ffmpeg-wrapper OS module for it, or set -Dffmpeg.native.dir)");
        }
        Path tmp = Files.createTempDirectory("ffmpeg");
        tmp.toFile().deleteOnExit();
        String list = new String(resource(root + "libs.txt"), StandardCharsets.UTF_8);
        Files.writeString(tmp.resolve("libs.txt"), list, StandardCharsets.UTF_8);
        tmp.resolve("libs.txt").toFile().deleteOnExit();
        for (String name : libraries(list)) {
            try (InputStream in = NativeLib.class.getResourceAsStream(root + name)) {
                if (in == null) {
                    throw new IOException("bundled native missing: " + root + name);
                }
                Path out = tmp.resolve(name);
                Files.copy(in, out, StandardCopyOption.REPLACE_EXISTING);
                out.toFile().deleteOnExit();
            }
        }
        return tmp;
    }

    private static byte[] resource(String name) throws IOException {
        try (InputStream in = NativeLib.class.getResourceAsStream(name)) {
            if (in == null) {
                throw new IOException("bundled native missing: " + name
                        + " (build it first with the OS module's native/build.sh, or set -Dffmpeg.native.dir)");
            }
            return in.readAllBytes();
        }
    }

    private NativeLib() {
    }
}
