#!/usr/bin/env -S bash -Eeuo pipefail
cd "$(dirname -- "$(readlink -f -- "$0")")"

build_dir="${BUILD_DIR:-build-linux}"

run_with_timeout() {
  if command -v timeout >/dev/null 2>&1; then
    timeout 10m "$@"
  else
    "$@"
  fi
}

echo "==> Configure ${build_dir}"
run_with_timeout cmake -S . -B "${build_dir}" -DCMAKE_BUILD_TYPE=Release -DFETCHCONTENT_QUIET=OFF

echo "==> Build"
cmake --build "${build_dir}" --config Release --parallel

echo "==> Strip"
if command -v sstrip >/dev/null 2>&1; then
  sstrip "${build_dir}/q2connect"
elif command -v strip >/dev/null 2>&1; then
  strip "${build_dir}/q2connect"
else
  echo "strip/sstrip not found; skipping"
fi

echo "==> Dist"
mkdir -p dist
cp -f "${build_dir}/q2connect" dist/q2connect
if [[ ! -f dist/q2connect.json ]]; then
  cp -f q2connect.json dist/q2connect.json
fi
cp -a assets dist/
cp -f q2connect.desktop.example dist/q2connect.desktop.example
echo "dist/q2connect ready"
