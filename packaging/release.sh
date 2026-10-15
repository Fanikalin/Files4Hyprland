#!/usr/bin/env bash
# Tag the version in PKGBUILD, build the package and attach it to the GitHub release.
# Usage: packaging/release.sh   (bump pkgver in PKGBUILD and commit first)
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"
REPO=Fanikalin/Files4Hyprland
ver=$(sed -n 's/^pkgver=//p' PKGBUILD)

git -C .. tag -f "v$ver" >/dev/null
git -C .. push -q -f origin "v$ver"

out=$(mktemp -d)
cp PKGBUILD files4hyprland.install "$out/"
( cd "$out" \
  && curl -fsSL -o "files4hyprland-$ver.tar.gz" "https://github.com/$REPO/archive/refs/tags/v$ver.tar.gz" \
  && sed -i "s/^sha256sums=.*/sha256sums=('$(sha256sum "files4hyprland-$ver.tar.gz" | cut -d' ' -f1)')/" PKGBUILD \
  && makepkg -f --noconfirm --nodeps )
pkg=$(ls "$out"/files4hyprland-*.pkg.tar.zst)

if gh release view "v$ver" --repo "$REPO" >/dev/null 2>&1; then
  gh release upload "v$ver" --repo "$REPO" --clobber "$pkg"
else
  gh release create "v$ver" --repo "$REPO" --title "Files4Hyprland $ver" --generate-notes "$pkg"
fi
# keep the AUR-style PKGBUILD in the repo in sync with the verified checksum
cp "$out/PKGBUILD" PKGBUILD
echo "Published: https://github.com/$REPO/releases/download/v$ver/$(basename "$pkg")"
