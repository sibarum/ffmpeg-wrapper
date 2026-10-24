package sibarum.ffmpeg.spi;

import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.List;
import java.util.ServiceLoader;

/**
 * Locates and loads a shim and the FFmpeg libraries under it: the decoding half's ({@code ffwrap}) or the recording
 * half's ({@code ffrecord}), each told apart by the service that says where its OS module bundles it.
 *
 * <p>Search order: a system property naming a directory laid out as {@code native/build.sh} stages one, then a folder
 * beside the running executable laid out the same way (how a native image ships them), then the classpath resources
 * the OS module on the class path names, extracted together to one temp directory. This class holds no platform
 * knowledge of its own.
 *
 * <p><b>Libraries, in an order.</b> FFmpeg's own shared libraries and a shim over them, and a library loaded by path
 * finds the ones it links only if they are already loaded. {@code libs.txt}, written by the build beside them, lists
 * each after the ones it needs, so the order and the version numbers in the file names are the build's to know and
 * never this class's.
 *
 * @param service  the service the OS module provides: {@link FfmpegNatives} or {@link FfmpegRecordNatives}
 * @param property the system property that names a directory to load from, such as {@code ffmpeg.native.dir}
 * @param beside   the folder beside an executable to look in, such as {@code ffmpeg}
 * @param module   the facade's artifact, for messages, such as {@code ffmpeg-wrapper}
 */
public record NativeLibraries(Class<? extends FfmpegNatives> service, String property, String beside, String module) {

    /**
     * Load every library, in {@code libs.txt}'s order, through {@link System#load}, so they are found afterwards by
     * {@code SymbolLookup.loaderLookup()}. Throws {@link IllegalStateException} saying what is missing when they
     * cannot be loaded.
     */
    public void load() {
        String dir = System.getProperty(property);
        try {
            Path besideExe = dir == null ? besideExecutable() : null;
            Path base = dir != null ? Path.of(dir) : besideExe != null ? besideExe : extractBundled();
            for (String name : libraries(Files.readString(base.resolve("libs.txt"), StandardCharsets.UTF_8))) {
                System.load(base.resolve(name).toAbsolutePath().toString());
            }
        } catch (IOException | UnsatisfiedLinkError e) {
            throw new IllegalStateException("failed to load the FFmpeg native libraries for " + platform(), e);
        }
    }

    /** What the OS module calls this platform, for messages; {@code unknown} when there is no OS module. */
    public String platform() {
        FfmpegNatives natives = natives();
        return natives == null ? "unknown (no " + module + " OS module on the class path)" : natives.platform();
    }

    /** The one OS module on the class path, or null when there is none. */
    private FfmpegNatives natives() {
        return ServiceLoader.load(service).findFirst().orElse(null);
    }

    /** The file names in {@code libs.txt}, in its order: blank lines and surrounding space ignored. */
    public static List<String> libraries(String list) {
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
     * A {@link #beside} folder next to the running executable, holding a {@code libs.txt}: where a native image's
     * installer puts the libraries, since they are not inside it. Null when there is none, which on a JVM is always
     * (the executable is {@code java}).
     */
    private Path besideExecutable() {
        try {
            Path exe = ProcessHandle.current().info().command().map(Path::of).orElse(null);
            Path folder = exe == null || exe.getParent() == null ? null : exe.getParent().resolve(beside);
            return folder != null && Files.isRegularFile(folder.resolve("libs.txt")) ? folder : null;
        } catch (RuntimeException e) {
            return null;
        }
    }

    private Path extractBundled() throws IOException {
        FfmpegNatives natives = natives();
        String root = natives == null ? null : natives.resourceRoot();
        if (root == null) {
            throw new IOException("no bundled FFmpeg for " + platform()
                    + " (add the " + module + " OS module for it, or set -D" + property + ")");
        }
        Class<?> owner = natives.getClass();
        Path tmp = Files.createTempDirectory(beside);
        tmp.toFile().deleteOnExit();
        String list = new String(resource(owner, root + "libs.txt"), StandardCharsets.UTF_8);
        Files.writeString(tmp.resolve("libs.txt"), list, StandardCharsets.UTF_8);
        tmp.resolve("libs.txt").toFile().deleteOnExit();
        for (String name : libraries(list)) {
            try (InputStream in = owner.getResourceAsStream(root + name)) {
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

    private byte[] resource(Class<?> owner, String name) throws IOException {
        try (InputStream in = owner.getResourceAsStream(name)) {
            if (in == null) {
                throw new IOException("bundled native missing: " + name
                        + " (build it first with the OS module's native/build.sh, or set -D" + property + ")");
            }
            return in.readAllBytes();
        }
    }
}
