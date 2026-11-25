# HFOC signing keys

## Development signing identity

`hfoc-development-signing-key.pem` is the declared **development** signing
identity. It is deliberately not a secret: it exists so that development and
QEMU media can carry a real, verifiable ECDSA P-256 boot-chain signature, and
so the production verification path is exercised on every build.

- Signer id: `1` (`IMAGE_SIGNATURE_DEVELOPMENT_SIGNER_ID`).
- Public half: compiled into `kernel/src/image_signature.c`.
- Trust: a development build trusts it; a release build (`HFOC_RELEASE_BUILD`)
  never compiles it in.

## Release signing identity

The release anchor (signer id `2`) is declared in the same file by its public
half only. The private half is **not** in this repository and is held by the
release ceremony owned by ITEM-380. Release media must be signed with it;
development media signed with the development key are refused by a release
kernel.

## Rules

- Never sign a release artifact with the development key.
- Never add the release private key to the repository.
- Rotating either identity is a reviewed change to `image_signature.c` and
  this directory in the same commit.
