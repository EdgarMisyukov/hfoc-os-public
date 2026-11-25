# Third-Party Dependency License Inventory

## mbedTLS mbedtls-3.6.7

- Upstream: Mbed TLS
- Release: `mbedtls-3.6.7`
- Commit: `068ff080b369adfac81509f9b57b2afabaf82dc5`
- URL: `https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2`
- Archive SHA-256: `a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6`
- License family: `Apache-2.0 OR GPL-2.0-or-later`
- License text: `third_party/mbedtls/LICENSE`
- Covered files: `file_count=272`

The exact vendored file list and per-file SHA-256 values are pinned in
`third_party/LOCKS.sha256`. No file under `third_party/mbedtls/` outside that
lock is permitted. All covered files are distributed under the mbedTLS
Apache-2.0 OR GPL-2.0-or-later license text in
`third_party/mbedtls/LICENSE`.

For the HFOC Appliance Platform the Apache-2.0 branch of that offered
disjunction is elected: the component ships as `Apache-2.0`
(`release/hfoc-license-inventory.txt`, `dependency_2_election=Apache-2.0`), the
election is stated in the shipped notice
`notices/mbedtls-Apache-2.0-election.txt`, and the release SBOM concludes
`Apache-2.0`. The upstream expression above is not rewritten: it describes what
upstream offers.

## lwIP STABLE-2_2_1_RELEASE

- Upstream: lwIP
- Release: `STABLE-2_2_1_RELEASE`
- Commit: `77dcd25a72509eb83f72b033d219b1d40cd8eb95`
- URL: `https://github.com/lwip-tcpip/lwip/archive/refs/tags/STABLE-2_2_1_RELEASE.tar.gz`
- Archive SHA-256: `ce0b7461c0ad9602c376f0bf07c5eb7253b48c7bf66f011c6bf3e2a96731c539`
- License family: `BSD-3-Clause`
- License text: `third_party/lwip/COPYING`
- Covered files: `file_count=107`

The exact vendored file list and per-file SHA-256 values are pinned in
`third_party/LOCKS.sha256`. No file under `third_party/lwip/` outside that
lock is permitted. All covered files are distributed under the lwIP
BSD-3-Clause license text in `third_party/lwip/COPYING`.

## Limine 12.3.2

- Upstream: Limine Bootloader
- Release: `12.3.2`
- URL: `https://github.com/Limine-Bootloader/Limine/releases/download/v12.3.2/limine-binary.tar.gz`
- Archive SHA-256: `433ac5933ef7918b6a6751db0a81e47fa122581dfb32430bc257be686f3906e5`
- Notice source commit: `8c8a688776735b2b2d12683a032e442583d361db`
- Notice source tree: `220df4be15f25bf0e241a9c7a0a51843d62e5cec`
- Root license family: `BSD-2-Clause`
- Root license text: `third_party/limine/COPYING`
- Bundled dependency inventory: `third_party/limine/3RDPARTY.md`
- Bundled license texts: `third_party/limine/LICENSES/`

The exact redistributed notice set is pinned in
`third_party/LICENSE-DOCUMENTS.sha256`.

Third-party components are excluded from the HFOC dual-license grant.
