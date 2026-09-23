#!/usr/bin/env node
import { spawnSync } from "node:child_process";
import { binaryPath } from "../index.js";

const result = spawnSync(binaryPath(), process.argv.slice(2), { stdio: "inherit" });
if (result.error) throw result.error;
if (result.signal) process.kill(process.pid, result.signal);
process.exit(result.status ?? 1);
