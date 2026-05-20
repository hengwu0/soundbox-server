#!/usr/bin/env bash
set -euo pipefail

version="1.3"
archive_url="https://launchpad.net/ubuntu/+archive/primary/+sourcefiles/webrtc-audio-processing/1.3-3build2/webrtc-audio-processing_1.3.orig.tar.gz"
archive_sha256="95552fc17faa0202133707bbb3727e8c2cf64d4266fe31bfdb2298d769c1db75"

work_dir="${WORK_DIR:-/tmp/webrtc-apm-${version}-work}"
prefix="${PREFIX:-/tmp/webrtc-apm-${version}-install}"
default_library="${DEFAULT_LIBRARY:-static}"
archive="${work_dir}/webrtc-audio-processing_${version}.orig.tar.gz"
source_dir="${work_dir}/src"
build_dir="${work_dir}/build-${default_library}"
meson_dir="${work_dir}/meson-python"

mkdir -p "${work_dir}"

if [[ ! -f "${archive}" ]]; then
  if command -v curl >/dev/null 2>&1; then
    curl -L "${archive_url}" -o "${archive}"
  elif command -v wget >/dev/null 2>&1; then
    wget -O "${archive}" "${archive_url}"
  else
    echo "curl or wget is required to download ${archive_url}" >&2
    exit 1
  fi
fi

printf '%s  %s\n' "${archive_sha256}" "${archive}" | sha256sum -c -

if [[ ! -d "${source_dir}" ]]; then
  mkdir -p "${source_dir}"
  tar -xf "${archive}" -C "${source_dir}" --strip-components=1
fi

if ! python3 -c 'import mesonbuild' >/dev/null 2>&1; then
  python3 -m pip install --target "${meson_dir}" meson
  export PYTHONPATH="${meson_dir}${PYTHONPATH:+:${PYTHONPATH}}"
fi

if [[ ! -f "${build_dir}/build.ninja" ]]; then
  python3 -m mesonbuild.mesonmain setup "${build_dir}" "${source_dir}" \
    --prefix="${prefix}" \
    --libdir=lib \
    -Ddefault_library="${default_library}"
fi

python3 -m mesonbuild.mesonmain compile -C "${build_dir}"
python3 -m mesonbuild.mesonmain install -C "${build_dir}"

cat <<EOF
WebRTC APM ${version} installed at: ${prefix}

Use it with:
  export PKG_CONFIG_PATH="${prefix}/lib/pkgconfig:\${PKG_CONFIG_PATH:-}"
EOF
