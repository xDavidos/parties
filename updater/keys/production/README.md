# Production update keys

Each `*.pub` file here is one Ed25519 public key that retail builds pin for verifying signed channel manifests. The file holds the key as 64 lowercase hex characters, and the key id is its first 16 characters. `parties_manifest_tool keygen` writes the file. At most two keys may be present, during a rotation.

Never put a private key in this directory. See `docs/releasing.md`.
