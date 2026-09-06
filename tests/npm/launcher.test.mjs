import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { chmod, mkdir, mkdtemp, readFile, realpath, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

import { resolvePackageTarget } from "../../scripts/package-target.mjs";

const repositoryRoot = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const launcherSource = await readFile(join(repositoryRoot, "bin", "dbdiff.mjs"), "utf8");
const targetSource = await readFile(
  join(repositoryRoot, "scripts", "package-target.mjs"),
  "utf8",
);
const VERSION = "0.1.0";

const nativeFixture = `#!/usr/bin/env node
const mode = process.argv[2];
if (mode === "io") {
  const chunks = [];
  process.stdin.on("data", (chunk) => chunks.push(chunk));
  process.stdin.on("end", () => {
    process.stdout.write(JSON.stringify({
      args: process.argv.slice(3),
      cwd: process.cwd(),
      environment: process.env.DBDIFF_LAUNCHER_TEST,
      input: Buffer.concat(chunks).toString("utf8"),
    }) + "\\n");
    process.stderr.write("native-stderr\\n");
    process.exitCode = Number(process.env.DBDIFF_LAUNCHER_EXIT || "0");
  });
} else if (mode === "signal") {
  process.kill(process.pid, process.argv[3] || "SIGTERM");
} else {
  process.exitCode = Number(mode || "0");
}
`;

async function launcherFixture(t, { metadata = true, binary = true } = {}) {
  const root = await mkdtemp(join(tmpdir(), "dbdiff-npm-launcher-"));
  t.after(() => rm(root, { recursive: true, force: true }));
  await Promise.all([
    mkdir(join(root, "bin"), { recursive: true }),
    mkdir(join(root, "scripts"), { recursive: true }),
    mkdir(join(root, "vendor"), { recursive: true }),
  ]);
  await Promise.all([
    writeFile(
      join(root, "package.json"),
      `${JSON.stringify({ name: "@zmeyc/dbdiff", version: VERSION, type: "module" })}\n`,
    ),
    writeFile(join(root, "bin", "dbdiff.mjs"), launcherSource, { mode: 0o755 }),
    writeFile(join(root, "scripts", "package-target.mjs"), targetSource),
  ]);
  const target = resolvePackageTarget().target;
  if (binary) {
    await writeFile(join(root, "vendor", "dbdiff"), nativeFixture, { mode: 0o755 });
    await chmod(join(root, "vendor", "dbdiff"), 0o755);
  }
  if (metadata) {
    await writeFile(
      join(root, "vendor", "install.json"),
      `${JSON.stringify({
        schemaVersion: 1,
        package: "@zmeyc/dbdiff",
        version: VERSION,
        target,
        asset: `dbdiff-v${VERSION}-${target}.gz`,
        archiveSha256: "a".repeat(64),
        binarySha256: "b".repeat(64),
      })}\n`,
    );
  }
  return { launcher: join(root, "bin", "dbdiff.mjs"), root };
}

test("launcher preserves arguments, cwd, environment, stdio, and native exit code 2", async (t) => {
  const fixture = await launcherFixture(t);
  const cwd = await mkdtemp(join(tmpdir(), "dbdiff-launcher-cwd-"));
  t.after(() => rm(cwd, { recursive: true, force: true }));
  const result = spawnSync(
    process.execPath,
    [fixture.launcher, "io", "argument with spaces", "--literal=$value"],
    {
      cwd,
      encoding: "utf8",
      env: {
        ...process.env,
        DBDIFF_LAUNCHER_TEST: "preserved",
        DBDIFF_LAUNCHER_EXIT: "2",
      },
      input: "stdin payload\n",
    },
  );
  assert.equal(result.status, 2);
  assert.equal(result.signal, null);
  assert.match(result.stderr, /native-stderr/);
  assert.deepEqual(JSON.parse(result.stdout), {
    args: ["argument with spaces", "--literal=$value"],
    cwd: await realpath(cwd),
    environment: "preserved",
    input: "stdin payload\n",
  });
});

test("launcher preserves arbitrary native exit codes", async (t) => {
  const fixture = await launcherFixture(t);
  const result = spawnSync(process.execPath, [fixture.launcher, "17"], { encoding: "utf8" });
  assert.equal(result.status, 17);
});

test("launcher reproduces a native SIGTERM termination", async (t) => {
  const fixture = await launcherFixture(t);
  const result = spawnSync(process.execPath, [fixture.launcher, "signal", "SIGTERM"], {
    encoding: "utf8",
  });
  assert.equal(result.status, null);
  assert.equal(result.signal, "SIGTERM");
});

test("missing install metadata explains disabled lifecycle scripts and gives rebuild guidance", async (t) => {
  const fixture = await launcherFixture(t, { metadata: false });
  const result = spawnSync(process.execPath, [fixture.launcher, "--version"], { encoding: "utf8" });
  assert.notEqual(result.status, 0);
  assert.match(result.stderr, /lifecycle scripts were disabled/);
  assert.match(result.stderr, /npm rebuild @zmeyc\/dbdiff --ignore-scripts=false/);
});

test("invalid metadata and a missing executable fail before spawning", async (t) => {
  const invalid = await launcherFixture(t);
  await writeFile(join(invalid.root, "vendor", "install.json"), "{}\n");
  const invalidResult = spawnSync(process.execPath, [invalid.launcher], { encoding: "utf8" });
  assert.notEqual(invalidResult.status, 0);
  assert.match(invalidResult.stderr, /metadata does not match/);
  assert.match(invalidResult.stderr, /npm rebuild/);

  const missing = await launcherFixture(t, { binary: false });
  const missingResult = spawnSync(process.execPath, [missing.launcher], { encoding: "utf8" });
  assert.notEqual(missingResult.status, 0);
  assert.match(missingResult.stderr, /cannot be launched/);
  assert.match(missingResult.stderr, /npm rebuild/);
});
