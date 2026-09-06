const SUPPORTED_PLATFORMS = new Set(["darwin", "linux"]);
const SUPPORTED_ARCHITECTURES = new Set(["x64", "arm64"]);
const MINIMUM_GLIBC = Object.freeze([2, 35]);

export function parseVersion(value) {
  if (typeof value !== "string") {
    return null;
  }

  const match = /^(\d+)\.(\d+)(?:\.(\d+))?/.exec(value.trim());
  if (!match) {
    return null;
  }

  return [Number(match[1]), Number(match[2]), Number(match[3] ?? 0)];
}

export function versionAtLeast(actual, minimum) {
  const length = Math.max(actual.length, minimum.length);
  for (let index = 0; index < length; index += 1) {
    const actualPart = actual[index] ?? 0;
    const minimumPart = minimum[index] ?? 0;
    if (actualPart !== minimumPart) {
      return actualPart > minimumPart;
    }
  }
  return true;
}

function runtimeReport() {
  try {
    return process.report?.getReport?.();
  } catch {
    return undefined;
  }
}

export function resolvePackageTarget({
  platform = process.platform,
  arch = process.arch,
  report = runtimeReport(),
} = {}) {
  if (!SUPPORTED_PLATFORMS.has(platform)) {
    throw new Error(
      `dbdiff does not provide a prebuilt executable for platform ${JSON.stringify(platform)}; ` +
        "supported platforms are macOS and GNU/Linux.",
    );
  }
  if (!SUPPORTED_ARCHITECTURES.has(arch)) {
    throw new Error(
      `dbdiff does not provide a prebuilt executable for architecture ${JSON.stringify(arch)}; ` +
        "supported architectures are x64 and arm64.",
    );
  }

  if (platform === "darwin") {
    return Object.freeze({ platform, arch, libc: null, target: `darwin-${arch}` });
  }

  const glibcVersion = report?.header?.glibcVersionRuntime;
  const parsedGlibcVersion = parseVersion(glibcVersion);
  if (!parsedGlibcVersion) {
    throw new Error(
      "dbdiff requires GNU/Linux with glibc 2.35 or newer; musl-based distributions " +
        "such as Alpine Linux are not supported.",
    );
  }
  if (!versionAtLeast(parsedGlibcVersion, MINIMUM_GLIBC)) {
    throw new Error(
      `dbdiff requires glibc 2.35 or newer, but this system reports glibc ${glibcVersion}.`,
    );
  }

  return Object.freeze({
    platform,
    arch,
    libc: "gnu",
    glibcVersion,
    target: `linux-${arch}-gnu`,
  });
}
