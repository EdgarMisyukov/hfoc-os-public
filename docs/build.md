# Building HFOC OS

[Home](../README.md) · English · [Русский](build.ru.md)

Run these commands from a Git checkout of the public `v1.0.0` tag.

Install the build, test and ISO tools on Debian.

```sh
sudo apt-get install build-essential binutils curl ca-certificates file xorriso \
  qemu-system-x86 python3 shellcheck ccache
```

Fetch the pinned boot dependency, run the public host checks and build the generic boot image.

```sh
make deps
make test-console-host test-production-install-media-host \
  test-appliance-production-static-boot-host
make iso
make test-console-history
```

The image is `build/hfoc-os-1.0.0.iso`. Run `sha256sum -c build/hfoc-os-1.0.0.iso.sha256` from the checkout to verify the build output. The [installation overview](install.md) has a QEMU command for booting this image. Disk installation and updating use a separate installer image with a declared target and layout.

## Release provenance

The original 1.0.0 build used source commit `933f7cd739a9437e86638fbfe30a738e22f2e9aa`, build ID `933f7cd739a9` and `SOURCE_DATE_EPOCH=1795545829`. The published image SHA-256 is `c903a2abaefe0f35b7924d6d01a123a618142fd7bb3087ee5b449c196750ed56`.

The original release check built the image twice, compared the results and generated an SPDX inventory. The public snapshot has its own commit ID and uses fixture device identities in examples and tests. A local rebuild can produce a different checksum. Verify the downloaded release against `SHA256SUMS.txt`.

The maintainers ran the full `make check` suite with the original Git history. The licensing check verifies the ancestry of the legacy MIT boundary and requires the original commit objects. The public checkout independently supports the commands above. [Source provenance](../release/source-provenance-v1.0.0.txt).
