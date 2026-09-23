// Compiles a module with a qjsc-kmp binary and checks the bytecode names the engine pinned in native/UPSTREAM.
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const binary = process.argv[2];
if (!binary) throw new Error("usage: node npm/smoke.mjs <qjsc-kmp binary>");
const expected = readFileSync(new URL("../native/UPSTREAM", import.meta.url), "utf8").match(/^commit=([0-9a-f]{40})$/m)?.[1];
if (!expected) throw new Error("native/UPSTREAM has no commit= line");

const dir = mkdtempSync(join(tmpdir(), "qjsc-smoke-"));
try {
    const input = join(dir, "smoke.js");
    const output = join(dir, "smoke.bin");
    writeFileSync(input, "export const answer = 6 * 7;\n");
    execFileSync(binary, ["-m", "-n", "smoke", "--strip-source", "-o", output, input], { stdio: "inherit" });
    const bytes = readFileSync(output);
    // quickjs-kmp bytecode header: magic at 0, the 40-char upstream commit at 12
    if (bytes.subarray(0, 4).toString("latin1") !== "QJKB") throw new Error(`${binary}: output is not quickjs-kmp bytecode`);
    const commit = bytes.subarray(12, 52).toString("latin1");
    if (commit !== expected) throw new Error(`${binary}: bytecode names engine ${commit}, native/UPSTREAM pins ${expected}`);
    console.log(`${binary}: ok (engine ${commit})`);
} finally {
    rmSync(dir, { recursive: true, force: true });
}
