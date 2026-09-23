/** Platforms with a prebuilt binary, as `<process.platform>-<process.arch>`; each ships as `@qjsc-kmp/<platform>`. */
export declare const PLATFORMS: readonly string[];

/** Absolute path of the qjsc-kmp binary for this machine; throws when there is none. */
export declare function binaryPath(): string;
