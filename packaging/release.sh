#!/usr/bin/env bash
# Tag the version in PKGBUILD, build and sign the package, attach it to the
# versioned GitHub release and publish it to the pacman repository that lives
# on the GitHub release tagged "repo" (so `pacman -Syu` picks up new versions).
# Usage: packaging/release.sh   (bump pkgver in PKGBUILD and commit first)
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"
REPO=Fanikalin/Files4Hyprland
GNUPGHOME_RELEASE="$HOME/.local/share/files4hyprland-release/gnupg"
KEY=DABFB081F06B48B502AB5BAE004D0E8617C2774B
ver=$(sed -n 's/^pkgver=//p' PKGBUILD)

git -C .. tag -f "v$ver" >/dev/null
git -C .. push -q -f origin "v$ver"

out=$(mktemp -d)
cp PKGBUILD files4hyprland.install "$out/"
( cd "$out" \
  && curl -fsSL -o "files4hyprland-$ver.tar.gz" "https://github.com/$REPO/archive/refs/tags/v$ver.tar.gz" \
  && sed -i "s/^sha256sums=.*/sha256sums=('$(sha256sum "files4hyprland-$ver.tar.gz" | cut -d' ' -f1)')/" PKGBUILD \
  && makepkg -f --noconfirm --nodeps )
pkg=$(ls "$out"/files4hyprland-[0-9]*.pkg.tar.zst)
name=$(basename "$pkg")

# detached binary signature, the format pacman verifies
gpg --homedir "$GNUPGHOME_RELEASE" --batch --yes --local-user "$KEY" --detach-sign -o "$pkg.sig" "$pkg"

# versioned release
if gh release view "v$ver" --repo "$REPO" >/dev/null 2>&1; then
  gh release upload "v$ver" --repo "$REPO" --clobber "$pkg" "$pkg.sig"
else
  gh release create "v$ver" --repo "$REPO" --title "Files4Hyprland $ver" --generate-notes "$pkg" "$pkg.sig"
fi

# pacman repository: database with the latest package only
( cd "$out" && repo-add -q files4hyprland.db.tar.gz "$name" \
  && cp --remove-destination "$(readlink -f files4hyprland.db)" files4hyprland.db \
  && cp --remove-destination "$(readlink -f files4hyprland.files)" files4hyprland.files )
gh release view repo --repo "$REPO" >/dev/null 2>&1 ||
  gh release create repo --repo "$REPO" --latest=false --title "pacman repository" \
    --notes "Pacman repository for files4hyprland. Don't download from here by hand; see the README."
gh release upload repo --repo "$REPO" --clobber "$pkg" "$pkg.sig" "$out/files4hyprland.db" "$out/files4hyprland.files"
gh release view repo --repo "$REPO" --json assets -q '.assets[].name' |
  grep -E '^files4hyprland-[0-9].*\.pkg\.tar\.zst(\.sig)?$' | grep -vxF -e "$name" -e "$name.sig" |
  while read -r old; do gh release delete-asset repo "$old" --repo "$REPO" -y; done || true

# keep the AUR-style PKGBUILD in the repo in sync with the verified checksum
cp "$out/PKGBUILD" PKGBUILD
echo "Published $ver: https://github.com/$REPO/releases/download/v$ver/$name (and the pacman repo)"
