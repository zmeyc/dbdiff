import { createHash, randomUUID } from "node:crypto";
import { execFile } from "node:child_process";
import { constants as fsConstants, createReadStream, createWriteStream } from "node:fs";
import {
  access,
  chmod,
  mkdir,
  readFile,
  rename,
  rm,
  stat,
  writeFile,
} from "node:fs/promises";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { Transform } from "node:stream";
import { pipeline } from "node:stream/promises";
import { createGunzip } from "node:zlib";

import { resolvePackageTarget } from "./package-target.mjs";

const PACKAGE_NAME = "@zmeyc/dbdiff";
const RELEASES_ORIGIN = "https://github.com/zmeyc/dbdiff/releases/download";
const DEFAULT_TIMEOUT_MS = 30_000;
const MAX_CHECKSUM_BYTES = 1024 * 1024;
const MAX_ARCHIVE_BYTES = 256 * 1024 * 1024;
const MAX_EXECUTABLE_BYTES = 512 * 1024 * 1024;
const VERSION_TIMEOUT_MS = 10_000;
const MAX_VERSION_OUTPUT_BYTES = 1024 * 1024;
const modulePath = fileURLToPath(import.meta.url);
const defaultPackageRoot = resolve(dirname(modulePath), "..");

function errorMessage(error) {
  return error instanceof Error ? error.message : String(error);
}

async function readManifest(packageRoot) {
  let manifest;
  try {
    manifest = JSON.parse(await readFile(resolve(packageRoot, "package.json"), "utf8"));
  } catch (error) {
    throw new Error(`cannot read the dbdiff package manifest: ${errorMessage(error)}`);
  }

  if (manifest?.name !== PACKAGE_NAME) {
    throw new Error(`expected package name ${PACKAGE_NAME}, received ${JSON.stringify(manifest?.name)}`);
  }
  if (typeof manifest.version !== "string" || !/^\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.-]+)?$/.test(manifest.version)) {
    throw new Error(`invalid dbdiff package version ${JSON.stringify(manifest?.version)}`);
  }
  return manifest;
}

export function parseChecksums(contents, expectedFilename) {
  if (typeof contents !== "string") {
    throw new TypeError("checksum contents must be a string");
  }

  const matches = [];
  for (const line of contents.split(/\r?\n/)) {
    const match = /^([0-9a-fA-F]{64})[ \t]+\*?(.+)$/.exec(line);
    if (match && match[2].trim() === expectedFilename) {
      matches.push(match[1].toLowerCase());
    }
  }
  if (matches.length !== 1) {
    throw new Error(
      `expected exactly one SHA-256 entry for ${expectedFilename}, found ${matches.length}`,
    );
  }
  return matches[0];
}

function sha256(value) {
  return createHash("sha256").update(value).digest("hex");
}

async function sha256File(path) {
  const hash = createHash("sha256");
  for await (const chunk of createReadStream(path)) {
    hash.update(chunk);
  }
  return hash.digest("hex");
}

async function downloadBuffer(url, { fetchImpl, timeoutMs, maximumBytes, userAgent }) {
  const controller = new AbortController();
  let timeout;
  const timeoutFailure = new Promise((_, reject) => {
    timeout = setTimeout(() => {
      controller.abort();
      reject(new Error(`download timed out after ${timeoutMs} ms: ${url}`));
    }, timeoutMs);
  });

  const operation = (async () => {
    let response;
    try {
      response = await fetchImpl(url, {
        headers: { accept: "application/octet-stream", "user-agent": userAgent },
        redirect: "follow",
        signal: controller.signal,
      });
    } catch (error) {
      throw new Error(`failed to download ${url}: ${errorMessage(error)}`);
    }

    if (!response?.ok) {
      throw new Error(`failed to download ${url}: HTTP ${response?.status ?? "unknown"}`);
    }

    const declaredLength = Number(response.headers?.get?.("content-length"));
    if (Number.isFinite(declaredLength) && declaredLength > maximumBytes) {
      throw new Error(`download exceeds the ${maximumBytes}-byte size limit: ${url}`);
    }

    const chunks = [];
    let receivedBytes = 0;
    if (response.body) {
      try {
        for await (const chunk of response.body) {
          const buffer = Buffer.from(chunk);
          receivedBytes += buffer.length;
          if (receivedBytes > maximumBytes) {
            controller.abort();
            throw new Error(`download exceeds the ${maximumBytes}-byte size limit: ${url}`);
          }
          chunks.push(buffer);
        }
      } catch (error) {
        throw new Error(`failed while downloading ${url}: ${errorMessage(error)}`);
      }
    }
    return Buffer.concat(chunks, receivedBytes);
  })();

  try {
    return await Promise.race([operation, timeoutFailure]);
  } finally {
    clearTimeout(timeout);
  }
}

function byteLimit(maximumBytes, label) {
  let receivedBytes = 0;
  return new Transform({
    transform(chunk, _encoding, callback) {
      receivedBytes += chunk.length;
      if (receivedBytes > maximumBytes) {
        callback(new Error(`${label} exceeds the ${maximumBytes}-byte size limit`));
        return;
      }
      callback(null, chunk);
    },
  });
}

async function extractArchive(archivePath, executablePath, maximumBytes) {
  await pipeline(
    createReadStream(archivePath),
    createGunzip(),
    byteLimit(maximumBytes, "decompressed executable"),
    createWriteStream(executablePath, { flags: "wx", mode: 0o700 }),
  );
  await chmod(executablePath, 0o755);
}

function executeVersion(binaryPath, { cwd }) {
  return new Promise((resolvePromise, reject) => {
    execFile(
      binaryPath,
      ["--version"],
      {
        cwd,
        encoding: "utf8",
        env: process.env,
        maxBuffer: MAX_VERSION_OUTPUT_BYTES,
        timeout: VERSION_TIMEOUT_MS,
      },
      (error, stdout, stderr) => {
        if (error) {
          reject(
            new Error(
              `downloaded dbdiff executable failed its version check: ${errorMessage(error)}` +
                (stderr ? `\n${stderr.trim()}` : ""),
            ),
          );
          return;
        }
        resolvePromise({ stdout, stderr });
      },
    );
  });
}

function normalizedVersionOutput(stdout) {
  if (stdout.endsWith("\r\n")) {
    return stdout.slice(0, -2);
  }
  if (stdout.endsWith("\n")) {
    return stdout.slice(0, -1);
  }
  return stdout;
}

async function verifyExecutable(binaryPath, version, runBinary, packageRoot) {
  const result = await runBinary(binaryPath, { cwd: packageRoot });
  const stdout = typeof result === "string" ? result : result?.stdout;
  const stderr = typeof result === "string" ? "" : result?.stderr ?? "";
  if (typeof stdout !== "string") {
    throw new Error("downloaded dbdiff executable returned invalid version output");
  }
  const actualVersion = normalizedVersionOutput(stdout);
  if (actualVersion !== version || stderr !== "") {
    throw new Error(
      `downloaded dbdiff executable reported ${JSON.stringify(actualVersion)}; ` +
        `expected exactly ${JSON.stringify(version)}`,
    );
  }
}

function metadataMatches(metadata, { version, target, asset }) {
  return (
    metadata?.schemaVersion === 1 &&
    metadata.package === PACKAGE_NAME &&
    metadata.version === version &&
    metadata.target === target &&
    metadata.asset === asset &&
    /^[0-9a-f]{64}$/.test(metadata.archiveSha256 ?? "") &&
    /^[0-9a-f]{64}$/.test(metadata.binarySha256 ?? "")
  );
}

async function existingInstallIsValid({
  metadataPath,
  binaryPath,
  version,
  target,
  asset,
  runBinary,
  packageRoot,
}) {
  try {
    const metadata = JSON.parse(await readFile(metadataPath, "utf8"));
    if (!metadataMatches(metadata, { version, target, asset })) {
      return false;
    }
    const binaryStat = await stat(binaryPath);
    if (!binaryStat.isFile() || (binaryStat.mode & 0o111) === 0) {
      return false;
    }
    await access(binaryPath, fsConstants.X_OK);
    if ((await sha256File(binaryPath)) !== metadata.binarySha256) {
      return false;
    }
    await verifyExecutable(binaryPath, version, runBinary, packageRoot);
    return true;
  } catch {
    return false;
  }
}

async function installCore({
  packageRoot,
  platform,
  arch,
  report,
  fetchImpl,
  runBinary,
  releaseBaseUrl,
  timeoutMs,
  maximumChecksumBytes,
  maximumArchiveBytes,
  maximumExecutableBytes,
}) {
  const manifest = await readManifest(packageRoot);
  const targetInfo = resolvePackageTarget({ platform, arch, report });
  const version = manifest.version;
  const asset = `dbdiff-v${version}-${targetInfo.target}.gz`;
  const checksumAsset = `dbdiff-v${version}-SHA256SUMS`;
  const vendorDirectory = resolve(packageRoot, "vendor");
  const binaryPath = resolve(vendorDirectory, "dbdiff");
  const metadataPath = resolve(vendorDirectory, "install.json");
  await mkdir(vendorDirectory, { recursive: true });

  if (
    await existingInstallIsValid({
      metadataPath,
      binaryPath,
      version,
      target: targetInfo.target,
      asset,
      runBinary,
      packageRoot,
    })
  ) {
    return { installed: false, binaryPath, metadataPath, target: targetInfo.target };
  }

  const unique = `${process.pid}-${randomUUID()}`;
  const archiveTemporaryPath = resolve(vendorDirectory, `.dbdiff-${unique}.gz.tmp`);
  const binaryTemporaryPath = resolve(vendorDirectory, `.dbdiff-${unique}.tmp`);
  const metadataTemporaryPath = resolve(vendorDirectory, `.install-${unique}.json.tmp`);
  const checksumUrl = `${releaseBaseUrl}/${encodeURIComponent(checksumAsset)}`;
  const archiveUrl = `${releaseBaseUrl}/${encodeURIComponent(asset)}`;
  const userAgent = `${PACKAGE_NAME}/${version} node/${process.versions.node}`;

  try {
    const checksumContents = await downloadBuffer(checksumUrl, {
      fetchImpl,
      timeoutMs,
      maximumBytes: maximumChecksumBytes,
      userAgent,
    });
    const expectedArchiveSha256 = parseChecksums(checksumContents.toString("utf8"), asset);
    const archive = await downloadBuffer(archiveUrl, {
      fetchImpl,
      timeoutMs,
      maximumBytes: maximumArchiveBytes,
      userAgent,
    });
    const actualArchiveSha256 = sha256(archive);
    if (actualArchiveSha256 !== expectedArchiveSha256) {
      throw new Error(
        `SHA-256 mismatch for ${asset}: expected ${expectedArchiveSha256}, ` +
          `received ${actualArchiveSha256}`,
      );
    }

    await writeFile(archiveTemporaryPath, archive, { flag: "wx", mode: 0o600 });
    await extractArchive(archiveTemporaryPath, binaryTemporaryPath, maximumExecutableBytes);
    await verifyExecutable(binaryTemporaryPath, version, runBinary, packageRoot);
    const binarySha256 = await sha256File(binaryTemporaryPath);
    const metadata = {
      schemaVersion: 1,
      package: PACKAGE_NAME,
      version,
      target: targetInfo.target,
      asset,
      archiveSha256: expectedArchiveSha256,
      binarySha256,
    };

    await rename(binaryTemporaryPath, binaryPath);
    await writeFile(metadataTemporaryPath, `${JSON.stringify(metadata, null, 2)}\n`, {
      flag: "wx",
      mode: 0o644,
    });
    await rename(metadataTemporaryPath, metadataPath);
    return { installed: true, binaryPath, metadataPath, target: targetInfo.target };
  } finally {
    await Promise.all([
      rm(archiveTemporaryPath, { force: true }),
      rm(binaryTemporaryPath, { force: true }),
      rm(metadataTemporaryPath, { force: true }),
    ]);
  }
}

export async function install() {
  const manifest = await readManifest(defaultPackageRoot);
  return installCore({
    packageRoot: defaultPackageRoot,
    platform: process.platform,
    arch: process.arch,
    report: undefined,
    fetchImpl: globalThis.fetch,
    runBinary: executeVersion,
    releaseBaseUrl: `${RELEASES_ORIGIN}/v${manifest.version}`,
    timeoutMs: DEFAULT_TIMEOUT_MS,
    maximumChecksumBytes: MAX_CHECKSUM_BYTES,
    maximumArchiveBytes: MAX_ARCHIVE_BYTES,
    maximumExecutableBytes: MAX_EXECUTABLE_BYTES,
  });
}

export async function installForTesting({
  packageRoot,
  platform = process.platform,
  arch = process.arch,
  report,
  fetchImpl = globalThis.fetch,
  runBinary = executeVersion,
  releaseBaseUrl,
  timeoutMs = DEFAULT_TIMEOUT_MS,
  maximumChecksumBytes = MAX_CHECKSUM_BYTES,
  maximumArchiveBytes = MAX_ARCHIVE_BYTES,
  maximumExecutableBytes = MAX_EXECUTABLE_BYTES,
} = {}) {
  if (!packageRoot) {
    throw new TypeError("installForTesting requires packageRoot");
  }
  const manifest = await readManifest(packageRoot);
  return installCore({
    packageRoot,
    platform,
    arch,
    report,
    fetchImpl,
    runBinary,
    releaseBaseUrl: releaseBaseUrl ?? `${RELEASES_ORIGIN}/v${manifest.version}`,
    timeoutMs,
    maximumChecksumBytes,
    maximumArchiveBytes,
    maximumExecutableBytes,
  });
}

const invokedDirectly = process.argv[1] && resolve(process.argv[1]) === modulePath;
if (invokedDirectly) {
  install().catch((error) => {
    console.error(`dbdiff postinstall failed: ${errorMessage(error)}`);
    process.exitCode = 1;
  });
}
