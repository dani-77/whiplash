# Packaging

Copies of the package definitions kept elsewhere, here so a release tarball
carries them:

- `arch/PKGBUILD`: the AUR package (`whiplash`), from
  [dani-77/aur](https://github.com/dani-77/aur).
- `void/template`: the xbps-src template, from
  [d77void/srcpkgs-d77](https://github.com/d77void/srcpkgs-d77).

Both pin the same release tag and tarball checksum. They are updated right
after a release is tagged, so the copies inside a tag's own tarball still
name the previous release.
