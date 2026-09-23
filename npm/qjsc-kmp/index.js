import { createRequire } from "node:module";

/** Platforms with a prebuilt binary, as `<process.platform>-<process.arch>`; each ships as `@qjsc-kmp/<platform>`. */
export const PLATFORMS = ["darwin-arm64", "darwin-x64", "linux-arm64", "linux-x64"];

const require = createRequire(import.meta.url);

/** Absolute path of the qjsc-kmp binary for this machine. */
export function binaryPath() {
    const platform = `${process.platform}-${process.arch}`;
    if (!PLATFORMS.includes(platform)) {
        throw new Error(`qjsc-kmp has no prebuilt binary for ${platform}; build it from source with quickjs-kmp's ./gradlew :library:buildHostTools`);
    }
    try {
        return require.resolve(`@qjsc-kmp/${platform}/bin/qjsc-kmp`);
    } catch {
        throw new Error(`@qjsc-kmp/${platform} is not installed; it is an optional dependency of qjsc-kmp, so install without --no-optional / --omit=optional`);
    }
}
