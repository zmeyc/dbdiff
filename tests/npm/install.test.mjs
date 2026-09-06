import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { chmod, mkdtemp, readFile, readdir, rm, stat, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";
import { gzipSync } from "node:zlib";

import { installForTesting, parseChecksums } from "../../scripts/install.mjs";
import {
  parseVersion,
  resolvePackageTarget,
  versionAtLeast,
} from "../../scripts/package-target.mjs";

const VERSION = "0.1.0";
const TARGET = "darwin-arm64";
const ASSET = `dbdiff-v${VERSION}-${TARGET}.gz`;
const CHECKSUM_ASSET = `dbdiff-v${VERSION}-SHA256SUMS`;

function digest(value) {
  return createHash("sha256").update(value).digest("hex");
}

async function temporaryPackage(t) {
  const root = await mkdtemp(join(tmpdir(), "dbdiff-npm-install-"));
  t.after(() => rm(root, { recursive: true, force: true }));
  await writeFile(
    join(root, "package.json"),
    `${JSON.stringify({ name: "@zmeyc/dbdiff", version: VERSION })}\n`,
  );
  return root;
}

function releaseFixture(executable = Buffer.from("native dbdiff fixture\n")) {
  const archive = gzipSync(executable, { mtime: 0 });
  const archiveSha256 = digest(archive);
  const checksum = Buffer.from(`${archiveSha256}  ${ASSET}\n`);
  const requested = [];
  const fetchImpl = async (url) => {
    requested.push(url);
    if (url.endsWith(`/${CHECKSUM_ASSET}`)) {
      return new Response(checksum);
    }
    if (url.endsWith(`/${ASSET}`)) {
      return new Response(archive);
    }
    return new Response("missing", { status: 404 });
  };
  return { archive, archiveSha256, checksum, executable, fetchImpl, requested };
}

function installOptions(root, fixture, overrides = {}) {
  return {
    packageRoot: root,
    platform: "darwin",
    arch: "arm64",
    fetchImpl: fixture.fetchImpl,
    runBinary: async () => ({ stdout: `${VERSION}\n`, stderr: "" }),
    ...overrides,
  };
}

async function vendorFiles(root) {
  try {
    return await readdir(join(root, "vendor"));
  } catch {
    return [];
  }

}

test("target selection accepts supported systems and enforces the glibc baseline", () => {
  assert.deepEqual(parseVersion("2.35"), [2, 35, 0]);
  assert.equal(versionAtLeast([2, 35], [2, 35]), true);
  assert.equal(versionAtLeast([2, 34, 9], [2, 35]), false);
  assert.equal(resolvePackageTarget({ platform: "darwin", arch: "x64" }).target, "darwin-x64");
  assert.equal(
    resolvePackageTarget({
      platform: "linux",
      arch: "arm64",
      report: { header: { glibcVersionRuntime: "2.39" } },
    }).target,
    "linux-arm64-gnu",
  );
  assert.throws(
    () =>
      resolvePackageTarget({
        platform: "linux",
        arch: "x64",
        report: { header: { glibcVersionRuntime: "2.34" } },
      }),
    /glibc 2\.35 or newer/,
  );
  assert.throws(
    () => resolvePackageTarget({ platform: "linux", arch: "x64", report: { header: {} } }),
    /musl-based distributions/,
  );
  assert.throws(() => resolvePackageTarget({ platform: "win32", arch: "x64" }), /platform/);
  assert.throws(() => resolvePackageTarget({ platform: "darwin", arch: "ia32" }), /architecture/);
});

test("checksum parsing requires one exact filename", () => {
  const hash = "a".repeat(64);
  assert.equal(parseChecksums(`${hash}  ${ASSET}\n`, ASSET), hash);
  assert.throws(() => parseChecksums(`${hash}  other.gz\n`, ASSET), /found 0/);
  assert.throws(
    () => parseChecksums(`${hash}  ${ASSET}\n${hash} *${ASSET}\n`, ASSET),
    /found 2/,
  );
});

test("installer downloads fixed GitHub release assets and commits verified metadata atomically", async (t) => {
  const root = await temporaryPackage(t);
  const fixture = releaseFixture();
  const result = await installForTesting(installOptions(root, fixture));

  assert.equal(result.installed, true);
  assert.deepEqual(await readFile(join(root, "vendor", "dbdiff")), fixture.executable);
  assert.notEqual((await stat(join(root, "vendor", "dbdiff"))).mode & 0o111, 0);
  const metadata = JSON.parse(await readFile(join(root, "vendor", "install.json"), "utf8"));
  assert.deepEqual(metadata, {
    schemaVersion: 1,
    package: "@zmeyc/dbdiff",
    version: VERSION,
    target: TARGET,
    asset: ASSET,
    archiveSha256: fixture.archiveSha256,
    binarySha256: digest(fixture.executable),
  });
  assert.deepEqual(fixture.requested, [
    `https://github.com/zmeyc/dbdiff/releases/download/v${VERSION}/${CHECKSUM_ASSET}`,
    `https://github.com/zmeyc/dbdiff/releases/download/v${VERSION}/${ASSET}`,
  ]);
  assert.equal((await vendorFiles(root)).some((name) => name.endsWith(".tmp")), false);
});

test("a valid installation is idempotent without network access", async (t) => {
  const root = await temporaryPackage(t);
  const fixture = releaseFixture();
  await installForTesting(installOptions(root, fixture));
  let versionChecks = 0;
  const result = await installForTesting(
    installOptions(root, fixture, {
      fetchImpl: async () => {
        throw new Error("network must not be used");
      },
      runBinary: async () => {
        versionChecks += 1;
        return { stdout: `${VERSION}\n`, stderr: "" };
      },
    }),
  );
  assert.equal(result.installed, false);
  assert.equal(versionChecks, 1);
});

test("corrupt installed bytes trigger a fresh verified download", async (t) => {
  const root = await temporaryPackage(t);
  const fixture = releaseFixture();
  await installForTesting(installOptions(root, fixture));
  await writeFile(join(root, "vendor", "dbdiff"), "corrupt");
  await chmod(join(root, "vendor", "dbdiff"), 0o755);
  fixture.requested.length = 0;
  const result = await installForTesting(installOptions(root, fixture));
  assert.equal(result.installed, true);
  assert.equal(fixture.requested.length, 2);
  assert.deepEqual(await readFile(join(root, "vendor", "dbdiff")), fixture.executable);
});

test("checksum failures leave no executable, metadata, or temporary files", async (t) => {
  const root = await temporaryPackage(t);
  const fixture = releaseFixture();
  fixture.fetchImpl = async (url) =>
    url.endsWith(`/${CHECKSUM_ASSET}`)
      ? new Response(`${"0".repeat(64)}  ${ASSET}\n`)
      : new Response(fixture.archive);
  await assert.rejects(installForTesting(installOptions(root, fixture)), /SHA-256 mismatch/);
  assert.deepEqual(await vendorFiles(root), []);
});

test("malformed gzip and version mismatches are rejected before commit", async (t) => {
  const gzipRoot = await temporaryPackage(t);
  const invalidArchive = Buffer.from("not gzip");
  const invalidFixture = releaseFixture();
  invalidFixture.fetchImpl = async (url) =>
    url.endsWith(`/${CHECKSUM_ASSET}`)
      ? new Response(`${digest(invalidArchive)}  ${ASSET}\n`)
      : new Response(invalidArchive);
  await assert.rejects(installForTesting(installOptions(gzipRoot, invalidFixture)), /gzip|header|archive/i);
  assert.deepEqual(await vendorFiles(gzipRoot), []);

  const versionRoot = await temporaryPackage(t);
  const fixture = releaseFixture();
  await assert.rejects(
    installForTesting(
      installOptions(versionRoot, fixture, {
        runBinary: async () => ({ stdout: "9.9.9\n", stderr: "" }),
      }),
    ),
    /expected exactly "0\.1\.0"/,
  );
  assert.deepEqual(await vendorFiles(versionRoot), []);
});

test("declared and streamed oversized downloads are rejected", async (t) => {
  const declaredRoot = await temporaryPackage(t);
  const fixture = releaseFixture();
  fixture.fetchImpl = async (url) => {
    if (url.endsWith(`/${CHECKSUM_ASSET}`)) {
      return new Response(fixture.checksum);
    }
    return new Response(fixture.archive, { headers: { "content-length": "999" } });
  };
  await assert.rejects(
    installForTesting(installOptions(declaredRoot, fixture, { maximumArchiveBytes: 10 })),
    /size limit/,
  );

  const streamedRoot = await temporaryPackage(t);
  const streamedFixture = releaseFixture();
  streamedFixture.fetchImpl = async (url) => {
    if (url.endsWith(`/${CHECKSUM_ASSET}`)) {
      return new Response(streamedFixture.checksum);
    }
    return {
      ok: true,
      status: 200,
      headers: new Headers(),
      body: {
        async *[Symbol.asyncIterator]() {
          yield Buffer.alloc(6);
          yield Buffer.alloc(6);
        },
      },
    };
  };
  await assert.rejects(
    installForTesting(installOptions(streamedRoot, streamedFixture, { maximumArchiveBytes: 10 })),
    /size limit/,
  );
});

test("interrupted and timed-out downloads fail cleanly", async (t) => {
  const interruptedRoot = await temporaryPackage(t);
  const fixture = releaseFixture();
  fixture.fetchImpl = async (url) => {
    if (url.endsWith(`/${CHECKSUM_ASSET}`)) {
      return new Response(fixture.checksum);
    }
    return {
      ok: true,
      status: 200,
      headers: new Headers(),
      body: {
        async *[Symbol.asyncIterator]() {
          yield Buffer.from("partial");
          throw new Error("socket reset");
        },
      },
    };
  };
  await assert.rejects(installForTesting(installOptions(interruptedRoot, fixture)), /socket reset/);
  assert.deepEqual(await vendorFiles(interruptedRoot), []);

  const timeoutRoot = await temporaryPackage(t);
  const timeoutFixture = releaseFixture();
  timeoutFixture.fetchImpl = (_url, { signal }) =>
    new Promise((_resolve, reject) => {
      signal.addEventListener("abort", () => reject(new Error("aborted")), { once: true });
    });
  await assert.rejects(
    installForTesting(installOptions(timeoutRoot, timeoutFixture, { timeoutMs: 5 })),
    /timed out/,
  );
  assert.deepEqual(await vendorFiles(timeoutRoot), []);
});
