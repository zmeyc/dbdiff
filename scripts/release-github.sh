#!/usr/bin/env bash

set -euo pipefail

readonly ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly VCPKG_COMMIT="c5a15727ee70fddf0296f0d8aafc3f58916fefac"
readonly RELEASE_ROOT="${ROOT}/dist/release"
readonly STAGE_ROOT="${ROOT}/build/release-package"

fail() {
  printf 'dbdiff release: %s\n' "$*" >&2
  exit 1
}

require_tool() {
  command -v "$1" >/dev/null 2>&1 || fail "required tool '$1' was not found"
}

[[ $# -eq 1 ]] || fail "usage: npm run release:github -- vX.Y.Z"
readonly TAG="$1"
[[ "${TAG}" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] || fail "tag must use vX.Y.Z"

cd "${ROOT}"

for tool in arch bash cmake ctest docker file gh git gzip ninja node npm otool shasum; do
  require_tool "${tool}"
done

[[ "$(uname -s)" == "Darwin" ]] || fail "local releases must run on macOS"
[[ "$(uname -m)" == "arm64" ]] || fail "local releases must run on Apple Silicon"
arch -x86_64 /usr/bin/true >/dev/null 2>&1 || fail "Rosetta 2 is required for x64 validation"

readonly PACKAGE_VERSION="$(node -p "require('./package.json').version")"
[[ "${TAG}" == "v${PACKAGE_VERSION}" ]] \
  || fail "tag ${TAG} does not match package version ${PACKAGE_VERSION}"
[[ "$(node -p "process.versions.node.split('.')[0]")" -ge 22 ]] \
  || fail "Node.js 22 or newer is required"

[[ -z "$(git status --porcelain)" ]] || fail "the working tree must be clean"
readonly HEAD_COMMIT="$(git rev-parse HEAD)"
[[ "$(git rev-parse "${TAG}^{commit}")" == "${HEAD_COMMIT}" ]] \
  || fail "${TAG} does not point at HEAD"

remote_tag_commit="$(git ls-remote origin "refs/tags/${TAG}^{}" | awk 'NR == 1 {print $1}')"
if [[ -z "${remote_tag_commit}" ]]; then
  remote_tag_commit="$(git ls-remote origin "refs/tags/${TAG}" | awk 'NR == 1 {print $1}')"
fi
[[ "${remote_tag_commit}" == "${HEAD_COMMIT}" ]] \
  || fail "${TAG} must already be pushed to origin and point at HEAD"

gh auth status >/dev/null
if gh release view "${TAG}" >/dev/null 2>&1; then
  fail "GitHub release ${TAG} already exists; release assets are never overwritten"
fi

readonly VCPKG_ROOT="${DBDIFF_VCPKG_ROOT:-${ROOT}/.cache/vcpkg-${VCPKG_COMMIT}}"
if [[ ! -d "${VCPKG_ROOT}/.git" ]]; then
  mkdir -p "$(dirname "${VCPKG_ROOT}")"
  git init "${VCPKG_ROOT}"
  git -C "${VCPKG_ROOT}" remote add origin https://github.com/microsoft/vcpkg.git
  git -C "${VCPKG_ROOT}" fetch origin "${VCPKG_COMMIT}"
  git -C "${VCPKG_ROOT}" checkout --detach FETCH_HEAD
fi
[[ "$(git -C "${VCPKG_ROOT}" rev-parse HEAD)" == "${VCPKG_COMMIT}" ]] \
  || fail "cached vcpkg checkout is not at ${VCPKG_COMMIT}"
if [[ "$(git -C "${VCPKG_ROOT}" rev-parse --is-shallow-repository)" == "true" ]]; then
  git -C "${VCPKG_ROOT}" fetch --unshallow origin "${VCPKG_COMMIT}"
fi
if [[ ! -x "${VCPKG_ROOT}/vcpkg" ]]; then
  "${VCPKG_ROOT}/bootstrap-vcpkg.sh" -disableMetrics
fi

rm -rf "${RELEASE_ROOT}" "${STAGE_ROOT}"
mkdir -p "${RELEASE_ROOT}" "${STAGE_ROOT}"

build_macos() {
  local target="$1"
  local cmake_arch triplet architecture_pattern
  case "${target}" in
    arm64)
      cmake_arch="arm64"
      triplet="arm64-osx-release"
      architecture_pattern='Mach-O 64-bit executable arm64'
      ;;
    x64)
      cmake_arch="x86_64"
      triplet="x64-osx-release"
      architecture_pattern='Mach-O 64-bit executable x86_64'
      ;;
    *)
      fail "unknown macOS target ${target}"
      ;;
  esac

  local build_dir="${STAGE_ROOT}/build-darwin-${target}"
  local install_dir="${STAGE_ROOT}/install-darwin-${target}"
  cmake -S "${ROOT}" -B "${build_dir}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=ON \
    -DDBDIFF_BUILD_INTEGRATION_TESTS=ON \
    -DDBDIFF_DISTRIBUTION_BUILD=ON \
    -DCMAKE_OSX_ARCHITECTURES="${cmake_arch}" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
    -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" \
    -DVCPKG_HOST_TRIPLET=arm64-osx-release \
    -DVCPKG_OVERLAY_TRIPLETS="${ROOT}/cmake/triplets" \
    -DVCPKG_TARGET_TRIPLET="${triplet}"
  cmake --build "${build_dir}" --parallel
  ctest --test-dir "${build_dir}" --no-tests=error --output-on-failure -L '^unit$'
  bash "${ROOT}/tests/integration/cli_lifecycle.sh" "${build_dir}/dbdiff"
  cmake --install "${build_dir}" --prefix "${install_dir}" --strip

  local binary="${install_dir}/bin/dbdiff"
  file "${binary}" | grep -Eq "${architecture_pattern}" \
    || fail "${binary} has the wrong architecture"
  if otool -L "${binary}" \
    | tail -n +2 \
    | awk '{print $1}' \
    | grep -Ev '^(/usr/lib/|/System/Library/)'; then
    fail "${binary} contains a non-system dynamic dependency"
  fi
  [[ "$("${binary}" --version)" == "${PACKAGE_VERSION}" ]] \
    || fail "${binary} reports the wrong version"
  cp "${binary}" "${STAGE_ROOT}/dbdiff-darwin-${target}"
}

build_linux() {
  local target="$1"
  local platform triplet architecture_pattern
  case "${target}" in
    arm64)
      platform="linux/arm64"
      triplet="arm64-linux-release"
      architecture_pattern='ARM aarch64'
      ;;
    x64)
      platform="linux/amd64"
      triplet="x64-linux-release"
      architecture_pattern='x86-64'
      ;;
    *)
      fail "unknown Linux target ${target}"
      ;;
  esac

  local output_dir="${STAGE_ROOT}/docker-linux-${target}"
  docker buildx build \
    --platform "${platform}" \
    --build-arg "TARGET_TRIPLET=${triplet}" \
    --build-arg "VCPKG_COMMIT=${VCPKG_COMMIT}" \
    --file "${ROOT}/packaging/Dockerfile.release" \
    --target artifact \
    --output "type=local,dest=${output_dir}" \
    --progress plain \
    "${ROOT}"

  local binary="${output_dir}/dbdiff"
  chmod 0755 "${binary}"
  file "${binary}" | grep -Eq "${architecture_pattern}" \
    || fail "${binary} has the wrong architecture"
  local reported_version
  reported_version="$(docker run --rm --platform "${platform}" \
    --volume "${binary}:/dbdiff:ro" ubuntu:22.04 /dbdiff --version)"
  [[ "${reported_version}" == "${PACKAGE_VERSION}" ]] \
    || fail "${binary} reports the wrong version"
  cp "${binary}" "${STAGE_ROOT}/dbdiff-linux-${target}-gnu"
}

build_macos arm64
build_macos x64
build_linux arm64
build_linux x64

create_asset() {
  local source="$1"
  local asset="$2"
  chmod 0755 "${source}"
  gzip -9 -n -c "${source}" >"${RELEASE_ROOT}/${asset}"
}

create_asset "${STAGE_ROOT}/dbdiff-darwin-arm64" \
  "dbdiff-${TAG}-darwin-arm64.gz"
create_asset "${STAGE_ROOT}/dbdiff-darwin-x64" \
  "dbdiff-${TAG}-darwin-x64.gz"
create_asset "${STAGE_ROOT}/dbdiff-linux-arm64-gnu" \
  "dbdiff-${TAG}-linux-arm64-gnu.gz"
create_asset "${STAGE_ROOT}/dbdiff-linux-x64-gnu" \
  "dbdiff-${TAG}-linux-x64-gnu.gz"

readonly CHECKSUM_FILE="dbdiff-${TAG}-SHA256SUMS"
(
  cd "${RELEASE_ROOT}"
  for asset in dbdiff-"${TAG}"-*.gz; do
    printf '%s  %s\n' "$(shasum -a 256 "${asset}" | awk '{print $1}')" "${asset}"
  done | LC_ALL=C sort -k2
) >"${RELEASE_ROOT}/${CHECKSUM_FILE}"

cp "${ROOT}/LICENSE" "${RELEASE_ROOT}/LICENSE"
cp "${ROOT}/THIRD_PARTY_NOTICES.md" "${RELEASE_ROOT}/THIRD_PARTY_NOTICES.md"

uploads=(
  "${RELEASE_ROOT}/dbdiff-${TAG}-darwin-arm64.gz"
  "${RELEASE_ROOT}/dbdiff-${TAG}-darwin-x64.gz"
  "${RELEASE_ROOT}/dbdiff-${TAG}-linux-arm64-gnu.gz"
  "${RELEASE_ROOT}/dbdiff-${TAG}-linux-x64-gnu.gz"
  "${RELEASE_ROOT}/${CHECKSUM_FILE}"
  "${RELEASE_ROOT}/LICENSE"
  "${RELEASE_ROOT}/THIRD_PARTY_NOTICES.md"
)

gh release create "${TAG}" \
  --draft \
  --generate-notes \
  --title "dbdiff ${PACKAGE_VERSION}" \
  --verify-tag \
  "${uploads[@]}"

readonly EXPECTED_ASSETS="${STAGE_ROOT}/expected-assets.txt"
readonly ACTUAL_ASSETS="${STAGE_ROOT}/actual-assets.txt"
printf '%s\n' \
  "LICENSE" \
  "THIRD_PARTY_NOTICES.md" \
  "${CHECKSUM_FILE}" \
  "dbdiff-${TAG}-darwin-arm64.gz" \
  "dbdiff-${TAG}-darwin-x64.gz" \
  "dbdiff-${TAG}-linux-arm64-gnu.gz" \
  "dbdiff-${TAG}-linux-x64-gnu.gz" \
  | LC_ALL=C sort >"${EXPECTED_ASSETS}"
gh release view "${TAG}" --json assets --jq '.assets[].name' \
  | LC_ALL=C sort >"${ACTUAL_ASSETS}"
diff -u "${EXPECTED_ASSETS}" "${ACTUAL_ASSETS}" \
  || fail "draft release asset set is incomplete"

gh release edit "${TAG}" --draft=false

readonly SMOKE_DIR="$(mktemp -d)"
trap 'rm -rf "${SMOKE_DIR}"' EXIT
(
  cd "${SMOKE_DIR}"
  npm init --yes >/dev/null
  npm install --save-dev "github:zmeyc/dbdiff#${TAG}"
  [[ "$(./node_modules/.bin/dbdiff --version)" == "${PACKAGE_VERSION}" ]]
)

printf 'Published and smoke-tested GitHub release %s\n' "${TAG}"
