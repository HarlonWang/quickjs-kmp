// Lays out the npm packages of one release: node npm/assemble.mjs <version> <binaries-dir> <out-dir>
// <binaries-dir>/<platform>/qjsc-kmp for every platform in PLATFORMS → <out-dir>/<platform>/ and <out-dir>/qjsc-kmp/
import { chmodSync, copyFileSync, cpSync, existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { PLATFORMS } from "./qjsc-kmp/index.js";

const [version, binaries, out] = process.argv.slice(2);
if (!version || !binaries || !out) throw new Error("usage: node npm/assemble.mjs <version> <binaries-dir> <out-dir>");
if (!/^\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?$/.test(version)) throw new Error(`not a release version: ${version}`);
const entry = fileURLToPath(new URL("./qjsc-kmp/", import.meta.url));
const template = JSON.parse(readFileSync(join(entry, "package.json"), "utf8"));
const shared = { license: template.license, repository: { ...template.repository, directory: "npm" } };
const json = (value) => JSON.stringify(value, null, 2) + "\n";

// a release ships every platform or none: a missing binary would leave that platform without a working install
const missing = PLATFORMS.filter((p) => !existsSync(join(binaries, p, "qjsc-kmp")));
if (missing.length) throw new Error(`no binary for ${missing.join(", ")} under ${binaries}`);

rmSync(out, { recursive: true, force: true });
for (const platform of PLATFORMS) {
    const [os, cpu] = platform.split("-");
    const dir = join(out, platform);
    mkdirSync(join(dir, "bin"), { recursive: true });
    copyFileSync(join(binaries, platform, "qjsc-kmp"), join(dir, "bin", "qjsc-kmp"));
    chmodSync(join(dir, "bin", "qjsc-kmp"), 0o755);
    writeFileSync(join(dir, "package.json"), json({
        name: `@qjsc-kmp/${platform}`,
        version,
        description: `The ${platform} binary of qjsc-kmp; install qjsc-kmp instead.`,
        ...shared,
        os: [os],
        cpu: [cpu],
        files: ["bin"],
        preferUnplugged: true,
    }));
}
cpSync(entry, join(out, "qjsc-kmp"), { recursive: true });
writeFileSync(join(out, "qjsc-kmp", "package.json"), json({
    ...template,
    version,
    optionalDependencies: Object.fromEntries(PLATFORMS.map((p) => [`@qjsc-kmp/${p}`, version])),
}));
console.log(`${out}: qjsc-kmp ${version} + ${PLATFORMS.map((p) => `@qjsc-kmp/${p}`).join(", ")}`);
