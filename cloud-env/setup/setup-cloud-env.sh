#!/bin/bash
# Recreates the RadioS cloud build/test environment (Ubuntu 24.04 container):
# MSYS2 UCRT64 toolchain + libraries (exact versions, SHA-256 checked) run under Wine.
# Usage: bash setup-cloud-env.sh   (expects this directory's files in /opt/radios-port)
set -euo pipefail
HERE=/opt/radios-port
apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
  gcc-mingw-w64-x86-64-posix binutils-mingw-w64-x86-64 mingw-w64-tools wine64 wine xvfb xauth zstd \
  ninja-build fonts-wine fonts-liberation2 locales nasm make xz-utils
sed -i 's/^# *tr_TR.UTF-8/tr_TR.UTF-8/' /etc/locale.gen; grep -q '^tr_TR.UTF-8' /etc/locale.gen || echo 'tr_TR.UTF-8 UTF-8' >> /etc/locale.gen; locale-gen
python3 -m pip install --break-system-packages -q "cmake==4.4.4"
if ! command -v pwsh >/dev/null; then
  mkdir -p /opt/pwsh && curl -sSL -o /tmp/pwsh.tgz https://github.com/PowerShell/PowerShell/releases/download/v7.5.4/powershell-7.5.4-linux-x64.tar.gz
  tar xzf /tmp/pwsh.tgz -C /opt/pwsh && chmod +x /opt/pwsh/pwsh && ln -sf /opt/pwsh/pwsh /usr/local/bin/pwsh
fi
# MSYS2 UCRT64 packages (pkglist.txt: filename sha256 version)
mkdir -p /opt/msys2-pkgs /opt/msys2
while read -r f sha ver; do
  [ -f "/opt/msys2-pkgs/$f" ] || curl -sS --retry 3 -o "/opt/msys2-pkgs/$f" "https://repo.msys2.org/mingw/ucrt64/$f"
  echo "$sha  /opt/msys2-pkgs/$f" | sha256sum -c --quiet
  tar --zstd -xf "/opt/msys2-pkgs/$f" -C /opt/msys2 --exclude=.PKGINFO --exclude=.BUILDINFO --exclude=.MTREE --exclude=.INSTALL
done < "$HERE/setup/pkglist.txt"
mkdir -p /opt/msys2/ucrt64/etc/gtk-3.0; printf '[Settings]\ngtk-font-name=Tahoma 9\n' > /opt/msys2/ucrt64/etc/gtk-3.0/settings.ini
# Wine prefix + test-environment support DLLs/registry
export WINEDEBUG=-all WINEPREFIX=/opt/wineprefix WINEARCH=win64
wineboot -i
export PATH=$HERE/bin:$PATH
for l in ar ranlib objdump nm strip dlltool; do ln -sf /usr/bin/x86_64-w64-mingw32-$l $HERE/bin/x86_64-w64-mingw32-$l; done
cd $HERE/wine-support
x86_64-w64-mingw32-gcc -shared -O2 dwmapi_stub.c -o dwmapi.dll
x86_64-w64-mingw32-gcc -shared -O2 -Wno-missing-braces kbdus_min.c -o kbdus.dll
x86_64-w64-mingw32-gcc -O2 -mwindows desktop_keeper.c -o desktop_keeper.exe
cp dwmapi.dll kbdus.dll $WINEPREFIX/drive_c/windows/system32/
wine regedit /S 'Z:\opt\radios-port\wine-support\test-env.reg'
wineserver -w
# Windows CPython runtime binaries (git-ignored) from the verified embeddable zip
cd /home/user/RadioSs
Z=packaging/windows/cache/python-3.14.8-embed-amd64.zip
echo "a93abe456ab01bd96d7a085b3cdb6566b3063f4241360d114142fbdb07f0a310  $Z" | sha256sum -c
python3 -I -c "import zipfile,os,sys; z=zipfile.ZipFile(sys.argv[1]); [open(os.path.join('runtime/python',i.filename),'wb').write(z.read(i)) for i in z.infolist() if not i.filename.endswith('._pth') and not os.path.exists(os.path.join('runtime/python',i.filename))]" "$Z"
echo "cloud environment ready"
# Test-only icu.dll proxy (Wine has no system ICU)
ICU=mingw-w64-ucrt-x86_64-icu-78.3-4-any.pkg.tar.zst
curl -sS -o /opt/msys2-pkgs/$ICU https://repo.msys2.org/mingw/ucrt64/$ICU
echo "db9282bbd0a2e74c31d66af6bd89eb68d647a09f721eef4c661768588dca4324  /opt/msys2-pkgs/$ICU" | sha256sum -c
mkdir -p $HERE/wine-support/icu-root && tar --zstd -xf /opt/msys2-pkgs/$ICU -C $HERE/wine-support/icu-root --exclude=.PKGINFO --exclude=.BUILDINFO --exclude=.MTREE
cd $HERE/wine-support; I=$HERE/wine-support/icu-root/ucrt64
x86_64-w64-mingw32-gcc -shared -O2 -I$I/include icu_proxy.c icu_proxy.def -o icu.dll $I/lib/libicuin.a $I/lib/libicuuc.a $I/lib/libicudt.a -static-libgcc -Wl,-Bstatic -lstdc++ -lwinpthread -Wl,-Bdynamic -ladvapi32
cp icu.dll $WINEPREFIX/drive_c/windows/system32/icu.dll
