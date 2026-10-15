package sibarum.ffmpeg;

import java.io.IOException;
import java.io.InputStream;
import java.lang.foreign.SymbolLookup;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * Locates and loads the {@code ffwrap} shim and the FFmpeg libraries under it.
 *
 * <p>Search order: the {@code ffmpeg.native.dir} system property (a directory laid out as
 * {@code native/build.sh} stages one), then the bundled classpath resources under
 * {@code /natives/<platform>/}, extracted together to one temp directory. Loading is idempotent and
 * thread-safe.
 *
 * <p><b>Six libraries, in an order.</b> Unlike imagelib, which is one library because its decoders are
 * Rust, this is FFmpeg's own shared libraries and a shim over them, and a library loaded by path finds
 * the ones it links only if they are already loaded. {@code libs.txt}, written by the build beside them,
 * lists each after the ones it needs, so the order and the version numbers in the file names are the
 * build's to know and never this class's.
 */
final class NativeLib {

    private static volatile SymbolLookup lookup;

    static synchronized SymbolLookup load() {
        if (lookup != null) {
            return lookup;
        }
        String dir = System.getProperty("ffmpeg.native.dir");
        try {
            Path base = dir != null ? Path.of(dir) : extractBundled();
            for (String name : libraries(Files.readString(base.resolve("libs.txt"), StandardCharsets.UTF_8))) {
                System.load(base.resolve(name).toAbsolutePath().toString());
            }
        } catch (IOException | UnsatisfiedLinkError e) {
            throw new VideoException("failed to load the FFmpeg native libraries for " + platform(), e);
        }
        lookup = SymbolLookup.loaderLookup();
        return lookup;
    }

    /**
     * The directory name a build stages into and this loader reads back — {@code native/build.sh}
     * writes exactly these, so a new target is added in both places or in neither.
     */
    static String platform() {
        String os = System.getProperty("os.name", "").toLowerCase(Locale.ROOT);
        String arch = System.getProperty("os.arch", "").toLowerCase(Locale.ROOT);
        String family = os.contains("win") ? "windows"
                : os.contains("mac") || os.contains("darwin") ? "macos"
                : os.contains("linux") ? "linux"
                : os.isEmpty() ? "unknown" : os.split("\\s+")[0];
        String cpu = switch (arch) {
            case "amd64", "x86_64" -> "x86_64";
            case "aarch64", "arm64" -> "aarch64";
            default -> arch;
        };
        return family + "-" + cpu;
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

    private static Path extractBundled() throws IOException {
        String root = "/natives/" + platform() + "/";
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
                        + " (build it first — see native/build.sh — or set -Dffmpeg.native.dir)");
            }
            return in.readAllBytes();
        }
    }

    private NativeLib() {
    }
}
