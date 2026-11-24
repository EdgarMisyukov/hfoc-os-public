# Building HFOC OS

[Home](../README.md) · English · [Русский](build.ru.md)

These commands apply to a complete source checkout. The public documentation branch is being prepared for the source snapshot.

On Debian, install the build, test and ISO tools:

```sh
sudo apt-get install build-essential binutils curl ca-certificates file xorriso \
  qemu-system-x86 python3 shellcheck ccache
```

From the source checkout, fetch the pinned boot dependency, run the checks and build the generic boot image:

```sh
make deps
make check
make iso
```

The image is `build/hfoc-os-1.0.0.iso`. Run `sha256sum -c build/hfoc-os-1.0.0.iso.sha256` from the checkout to verify the build output. The [installation overview](install.md) has a QEMU command for booting this image. Disk installation and updating use a separate installer image with a declared target and layout.

## Release provenance

The original 1.0.0 build used source commit `933f7cd739a9437e86638fbfe30a738e22f2e9aa`, build ID `933f7cd739a9` and `SOURCE_DATE_EPOCH=1795545829`. The published image SHA-256 is `c903a2abaefe0f35b7924d6d01a123a618142fd7bb3087ee5b449c196750ed56`.

The original release check built the image twice, compared the results and generated an SPDX inventory. The public snapshot's commit ID will reflect its own history. Verify any reproduction against the release checksum.
