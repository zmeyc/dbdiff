#!/usr/bin/env node

import { spawn } from "node:child_process";
import { constants as fsConstants, readFileSync, statSync, accessSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

import { resolvePackageTarget } from "../scripts/package-target.mjs";

const PACKAGE_NAME = "@zmeyc/dbdiff";
const packageRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const binaryPath = resolve(packageRoot, "vendor", "dbdiff");
const metadataPath = resolve(packageRoot, "vendor", "install.json");
const rebuildCommand = "npm rebuild @zmeyc/dbdiff --ignore-scripts=false";

function fail(message) {
  console.error(`dbdiff: ${message}`);
  console.error(`dbdiff: reinstall the native executable with: ${rebuildCommand}`);
  process.exit(1);
}

let manifest;
let metadata;
let targetInfo;
try {
  manifest = JSON.parse(readFileSync(resolve(packageRoot, "package.json"), "utf8"));
  metadata = JSON.parse(readFileSync(metadataPath, "utf8"));
  targetInfo = resolvePackageTarget();
} catch (error) {
  fail(
    `the native executable is not installed or its metadata is unreadable (${error instanceof Error ? error.message : String(error)}). ` +
      "This usually means npm lifecycle scripts were disabled.",
  );
}

const expectedAsset = `dbdiff-v${manifest.version}-${targetInfo.target}.gz`;
const validMetadata =
  manifest.name === PACKAGE_NAME &&
  metadata?.schemaVersion === 1 &&
  metadata.package === PACKAGE_NAME &&
  metadata.version === manifest.version &&
  metadata.target === targetInfo.target &&
  metadata.asset === expectedAsset &&
  /^[0-9a-f]{64}$/.test(metadata.archiveSha256 ?? "") &&
  /^[0-9a-f]{64}$/.test(metadata.binarySha256 ?? "");
if (!validMetadata) {
  fail("the installed native executable metadata does not match this package or platform.");
}

try {
  const binaryStat = statSync(binaryPath);
  if (!binaryStat.isFile() || (binaryStat.mode & 0o111) === 0) {
    throw new Error("native executable is missing or is not executable");
  }
  accessSync(binaryPath, fsConstants.X_OK);
} catch (error) {
  fail(`the installed native executable cannot be launched (${error instanceof Error ? error.message : String(error)}).`);
}

const child = spawn(binaryPath, process.argv.slice(2), {
  cwd: process.cwd(),
  env: process.env,
  stdio: "inherit",
});

const forwardedSignals = ["SIGINT", "SIGTERM", "SIGHUP"];
const signalHandlers = new Map();
for (const signal of forwardedSignals) {
  const handler = () => {
    if (child.exitCode === null && child.signalCode === null) {
      child.kill(signal);
    }
  };
  signalHandlers.set(signal, handler);
  process.on(signal, handler);
}

function removeSignalHandlers() {
  for (const [signal, handler] of signalHandlers) {
    process.off(signal, handler);
  }
}

let finished = false;
child.once("error", (error) => {
  if (finished) {
    return;
  }
  finished = true;
  removeSignalHandlers();
  console.error(`dbdiff: failed to start the native executable: ${error.message}`);
  console.error(`dbdiff: reinstall it with: ${rebuildCommand}`);
  process.exitCode = 1;
});

child.once("close", (code, signal) => {
  if (finished) {
    return;
  }
  finished = true;
  removeSignalHandlers();
  if (signal) {
    process.kill(process.pid, signal);
    return;
  }
  process.exitCode = code ?? 1;
});
