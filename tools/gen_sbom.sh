#!/bin/sh
set -eu

# SPDX 2.3 JSON SBOM for a release tarball, using only POSIX shell and binutils.
# Usage: gen_sbom.sh TARBALL BINARY VERSION WITH_TLS OUTPUT
# The timestamp comes from SOURCE_DATE_EPOCH, so the same inputs give the same bytes.

[ $# -eq 5 ] || { echo "usage: $0 TARBALL BINARY VERSION WITH_TLS OUTPUT" >&2; exit 2; }
tarball=$1
binary=$2
version=$3
with_tls=$4
out=$5
: "${SOURCE_DATE_EPOCH:?SOURCE_DATE_EPOCH must be set}"

root=$(cd "$(dirname "$0")/.." && pwd)
repo=https://github.com/BolvicBolvicovic/mogbus
tar_name=$(basename "$tarball")
created=$(date -u -d "@$SOURCE_DATE_EPOCH" +%Y-%m-%dT%H:%M:%SZ)
tar_sha256=$(sha256sum "$tarball" | cut -d' ' -f1)
bin_sha256=$(sha256sum "$binary" | cut -d' ' -f1)
bin_sha1=$(sha1sum "$binary" | cut -d' ' -f1)
# The vlib submodule commit: VLIB_REVISION, else the checkout's gitlink, else unknown.
vlib_rev=${VLIB_REVISION:-$(git -C "$root" ls-tree HEAD vlib 2>/dev/null | awk '{print $3}')}
vlib_version=NOASSERTION
[ -z "$vlib_rev" ] || vlib_version=$vlib_rev
s7_version=$(sed -n 's/^#define S7_VERSION "\(.*\)"/\1/p' "$root/third_party/s7/s7.h")
[ -n "$s7_version" ] || { echo "gen_sbom.sh: S7_VERSION not found" >&2; exit 1; }
# NOTICE.md and s7's LICENSE both say 0-clause BSD (SPDX: 0BSD); refuse to guess if that changes.
head -1 "$root/third_party/s7/LICENSE" | grep -q '^0-clause BSD' ||
	{ echo "gen_sbom.sh: third_party/s7/LICENSE is no longer 0-clause BSD" >&2; exit 1; }

copyright=$(sed -n '1,/^Copyright/s/^\(Copyright .*\)/\1/p' "$root/LICENSE")
holder=$(printf '%s' "$copyright" | sed 's/^Copyright ([cC]) [0-9, -]* //')
[ -n "$holder" ] || { echo "gen_sbom.sh: no Copyright line in LICENSE" >&2; exit 1; }

glibc=$(getconf GNU_LIBC_VERSION 2>/dev/null | sed 's/^glibc //' || true)
ssl=
if [ "$with_tls" = 1 ]; then
	ssl=$(pkg-config --modversion openssl 2>/dev/null || true)
fi

{
cat <<JSON
{
  "spdxVersion": "SPDX-2.3",
  "dataLicense": "CC0-1.0",
  "SPDXID": "SPDXRef-DOCUMENT",
  "name": "mogbus-$version",
  "documentNamespace": "$repo/spdx/$tar_name-$tar_sha256",
  "creationInfo": {
    "created": "$created",
    "creators": ["Tool: mogbus-gen_sbom.sh"]
  },
  "documentDescribes": ["SPDXRef-Package-mogbus"],
  "packages": [
    {
      "SPDXID": "SPDXRef-Package-mogbus",
      "name": "mogbus",
      "versionInfo": "$version",
      "packageFileName": "$tar_name",
      "downloadLocation": "$repo/releases/download/v$version/$tar_name",
      "homepage": "$repo",
      "filesAnalyzed": false,
      "checksums": [{"algorithm": "SHA256", "checksumValue": "$tar_sha256"}],
      "licenseConcluded": "MIT",
      "licenseDeclared": "MIT",
      "copyrightText": "$copyright",
      "supplier": "Person: $holder"
    },
    {
      "SPDXID": "SPDXRef-Package-s7",
      "name": "s7",
      "versionInfo": "$s7_version",
      "downloadLocation": "https://cm-gitlab.stanford.edu/bil/s7",
      "filesAnalyzed": false,
      "licenseConcluded": "0BSD",
      "licenseDeclared": "0BSD",
      "copyrightText": "NOASSERTION",
      "comment": "Vendored in third_party/s7 and statically linked."
    },
    {
      "SPDXID": "SPDXRef-Package-vlib",
      "name": "vlib",
      "versionInfo": "$vlib_version",
      "downloadLocation": "https://github.com/BolvicBolvicovic/vlib",
      "filesAnalyzed": false,
      "licenseConcluded": "NOASSERTION",
      "licenseDeclared": "NOASSERTION",
      "copyrightText": "NOASSERTION",
      "comment": "Git submodule vlib/ (arena allocator and headers), unmodified, pinned to the commit in versionInfo and statically linked; it carries no license file."
    }
JSON
needed=$(readelf -d "$binary" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | LC_ALL=C sort)
for lib in $needed; do
	id=$(printf '%s' "$lib" | sed 's/[^A-Za-z0-9.-]/-/g')
	ver=
	case $lib in
	libssl.so*|libcrypto.so*) ver=$ssl; name=openssl ;;
	libc.so*|libm.so*|ld-linux*) ver=$glibc; name=glibc ;;
	*) name=$lib ;;
	esac
	printf '    ,{\n      "SPDXID": "SPDXRef-Dynamic-%s",\n      "name": "%s",\n' "$id" "$name"
	[ -z "$ver" ] || printf '      "versionInfo": "%s",\n' "$ver"
	printf '      "downloadLocation": "NOASSERTION",\n      "filesAnalyzed": false,\n'
	printf '      "licenseConcluded": "NOASSERTION",\n      "licenseDeclared": "NOASSERTION",\n'
	printf '      "copyrightText": "NOASSERTION",\n      "comment": "Dynamic runtime dependency (DT_NEEDED %s), not distributed in the tarball."\n    }\n' "$lib"
done
cat <<JSON
  ],
  "files": [
    {
      "SPDXID": "SPDXRef-File-mogbus-binary",
      "fileName": "bin/mogbus",
      "fileTypes": ["BINARY"],
      "checksums": [
        {"algorithm": "SHA1", "checksumValue": "$bin_sha1"},
        {"algorithm": "SHA256", "checksumValue": "$bin_sha256"}
      ],
      "licenseConcluded": "MIT",
      "licenseInfoInFiles": ["MIT"],
      "copyrightText": "NOASSERTION"
    }
  ],
  "relationships": [
    {"spdxElementId": "SPDXRef-DOCUMENT", "relationshipType": "DESCRIBES", "relatedSpdxElement": "SPDXRef-Package-mogbus"},
    {"spdxElementId": "SPDXRef-Package-mogbus", "relationshipType": "CONTAINS", "relatedSpdxElement": "SPDXRef-File-mogbus-binary"},
    {"spdxElementId": "SPDXRef-Package-mogbus", "relationshipType": "CONTAINS", "relatedSpdxElement": "SPDXRef-Package-s7"},
    {"spdxElementId": "SPDXRef-Package-mogbus", "relationshipType": "CONTAINS", "relatedSpdxElement": "SPDXRef-Package-vlib"},
    {"spdxElementId": "SPDXRef-File-mogbus-binary", "relationshipType": "STATIC_LINK", "relatedSpdxElement": "SPDXRef-Package-s7"},
    {"spdxElementId": "SPDXRef-File-mogbus-binary", "relationshipType": "STATIC_LINK", "relatedSpdxElement": "SPDXRef-Package-vlib"}
JSON
for lib in $needed; do
	id=$(printf '%s' "$lib" | sed 's/[^A-Za-z0-9.-]/-/g')
	printf '    ,{"spdxElementId": "SPDXRef-File-mogbus-binary", "relationshipType": "DYNAMIC_LINK", "relatedSpdxElement": "SPDXRef-Dynamic-%s"}\n' "$id"
done
printf '  ]\n}\n'
} > "$out"
