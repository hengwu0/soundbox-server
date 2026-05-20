#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
archives_dir="${script_dir}/archives"
downloads_file="${script_dir}/downloads"
source_root="${script_dir}/src"
build_root="${script_dir}/build"
install_root="${script_dir}/install"
tools_root="${script_dir}/tools"

version="1.3"
default_library="${DEFAULT_LIBRARY:-static}"

webrtc_section="webrtc-audio-processing"
abseil_section="abseil-cpp"
abseil_patch_section="abseil-cpp-patch"
meson_section="meson"

source_dir="${source_root}/webrtc-audio-processing-${version}"
meson_dir="${tools_root}/meson-1.11.1"
build_dir="${build_root}/webrtc-audio-processing-${version}-${default_library}"
prefix="${install_root}/webrtc-audio-processing-${version}"
processing_lib="${prefix}/lib/libwebrtc-audio-processing-1.a"
coding_lib="${prefix}/lib/libwebrtc-audio-coding-1.a"
processing_pc="${prefix}/lib/pkgconfig/webrtc-audio-processing-1.pc"
coding_pc="${prefix}/lib/pkgconfig/webrtc-audio-coding-1.pc"

lookup_download_value() {
  local section="$1"
  local key="$2"

  python3 - "${downloads_file}" "${section}" "${key}" <<'PY'
import configparser
import sys

downloads_file, section, key = sys.argv[1:]
parser = configparser.ConfigParser(interpolation=None)
read_files = parser.read(downloads_file, encoding="utf-8")
if not read_files:
    print(f"Missing third-party downloads file: {downloads_file}", file=sys.stderr)
    raise SystemExit(1)
if not parser.has_section(section):
    print(f"Missing [{section}] in {downloads_file}", file=sys.stderr)
    raise SystemExit(1)
if not parser.has_option(section, key):
    print(f"Missing {key}= in [{section}] of {downloads_file}", file=sys.stderr)
    raise SystemExit(1)
print(parser.get(section, key).strip())
PY
}

load_download_entry() {
  local section="$1"
  local archive_var="$2"
  local sha256_var="$3"
  local url_var="$4"
  local archive_name
  local archive_sha256
  local archive_url

  archive_name="$(lookup_download_value "${section}" archive)"
  archive_sha256="$(lookup_download_value "${section}" sha256)"
  archive_url="$(lookup_download_value "${section}" url)"

  printf -v "${archive_var}" '%s/%s' "${archives_dir}" "${archive_name}"
  printf -v "${sha256_var}" '%s' "${archive_sha256}"
  printf -v "${url_var}" '%s' "${archive_url}"
}

verify_archive() {
  local archive="$1"
  local expected_sha256="$2"
  local download_url="$3"

  if [[ ! -f "${archive}" ]]; then
    echo "Missing third-party archive: ${archive}" >&2
    echo "Download it from: ${download_url}" >&2
    exit 1
  fi

  printf '%s  %s\n' "${expected_sha256}" "${archive}" | sha256sum -c - >&2
}

prepare_webrtc_source() {
  local source_stamp="${source_dir}/.archive.sha256"

  if [[ ! -d "${source_dir}" ]] ||
     [[ ! -f "${source_stamp}" ]] ||
     [[ "$(cat "${source_stamp}")" != "${webrtc_sha256}" ]]; then
    rm -rf "${source_dir}"
    mkdir -p "${source_root}"
    tar --no-same-owner -xf "${webrtc_archive}" -C "${source_root}"
    printf '%s' "${webrtc_sha256}" > "${source_stamp}"
  fi

  mkdir -p "${source_dir}/subprojects/packagecache"
  cp "${abseil_archive}" "${source_dir}/subprojects/packagecache/"
  cp "${abseil_patch}" "${source_dir}/subprojects/packagecache/"
}

prepare_meson() {
  if [[ ! -d "${meson_dir}/mesonbuild" ]] ||
     [[ ! -f "${meson_dir}/.archive.sha256" ]] ||
     [[ "$(cat "${meson_dir}/.archive.sha256")" != "${meson_sha256}" ]]; then
    rm -rf "${meson_dir}"
    mkdir -p "${meson_dir}"
    python3 -m zipfile -e "${meson_wheel}" "${meson_dir}"
    printf '%s' "${meson_sha256}" > "${meson_dir}/.archive.sha256"
  fi
}

for command_name in python3 sha256sum tar ninja; do
  if ! command -v "${command_name}" >/dev/null 2>&1; then
    echo "${command_name} is required to build third-party sources." >&2
    exit 1
  fi
done

load_download_entry "${webrtc_section}" webrtc_archive webrtc_sha256 webrtc_url
load_download_entry "${abseil_section}" abseil_archive abseil_sha256 abseil_url
load_download_entry "${abseil_patch_section}" abseil_patch abseil_patch_sha256 abseil_patch_url
load_download_entry "${meson_section}" meson_wheel meson_sha256 meson_url

if [[ -f "${processing_lib}" && -f "${coding_lib}" &&
      -f "${processing_pc}" && -f "${coding_pc}" ]]; then
  echo "WebRTC APM ${version} already installed at: ${prefix}"
  exit 0
fi

verify_archive "${webrtc_archive}" "${webrtc_sha256}" "${webrtc_url}"
verify_archive "${abseil_archive}" "${abseil_sha256}" "${abseil_url}"
verify_archive "${abseil_patch}" "${abseil_patch_sha256}" "${abseil_patch_url}"
verify_archive "${meson_wheel}" "${meson_sha256}" "${meson_url}"

prepare_webrtc_source
prepare_meson

export PYTHONPATH="${meson_dir}${PYTHONPATH:+:${PYTHONPATH}}"

if [[ ! -f "${build_dir}/build.ninja" ]]; then
  python3 -m mesonbuild.mesonmain setup "${build_dir}" "${source_dir}" \
    --prefix="${prefix}" \
    --libdir=lib \
    -Ddefault_library="${default_library}" \
    --wrap-mode=nodownload \
    --force-fallback-for=absl_base,absl_flags,absl_strings,absl_synchronization,absl_bad_optional_access
fi

python3 -m mesonbuild.mesonmain compile -C "${build_dir}"
python3 -m mesonbuild.mesonmain install -C "${build_dir}"

cat <<EOF
WebRTC APM ${version} installed at: ${prefix}
EOF
