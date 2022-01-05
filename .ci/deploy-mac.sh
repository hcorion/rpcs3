#!/bin/sh -ex

cd build || exit 1

# Gather explicit version number and number of commits
COMM_TAG=$(awk '/version{.*}/ { printf("%d.%d.%d", $5, $6, $7) }' ../rpcs3/rpcs3_version.cpp)
COMM_COUNT=$(git rev-list --count HEAD)
COMM_HASH=$(git rev-parse --short=8 HEAD)

AVVER="${COMM_TAG}-${COMM_COUNT}"

# AVVER is used for GitHub releases, it is the version number.
echo "AVVER=$AVVER" >> ../.ci/ci-vars.env

cd bin
mkdir "rpcs3.app/Contents/lib/"
cp "/usr/local/Cellar/llvm/13.0.0_2/lib/libc++abi.1.0.dylib" "rpcs3.app/Contents/lib/libc++abi.1.dylib"
rm -rf "rpcs3.app/Contents/Frameworks/QtPdf.framework" \
"rpcs3.app/Contents/Frameworks/QtQml.framework" \
"rpcs3.app/Contents/Frameworks/QtQmlModels.framework" \
"rpcs3.app/Contents/Frameworks/QtQuick.framework" \
"rpcs3.app/Contents/Frameworks/QtVirtualKeyboard.framework" \
"rpcs3.app/Contents/Plugins/platforminputcontexts" \
"rpcs3.app/Contents/Plugins/virtualkeyboard"

# Need to do this rename hack due to case insensitive filesystem
mv rpcs3.app RPCS3_.app
mv RPCS3_.app RPCS3.app

echo "[InternetShortcut]" > Quickstart.url
echo "URL=https://rpcs3.net/quickstart" >> Quickstart.url
echo "IconIndex=0" >> Quickstart.url

create-dmg --volname RPCS3 \
--window-size 800 400 \
--icon-size 100 \
--icon rpcs3.app 200 190 \
--add-file Quickstart.url Quickstart.url 400 20 \
--hide-extension rpcs3.app \
--hide-extension Quickstart.url \
--app-drop-link 600 185 \
--skip-jenkins \
"$BUILD_ARTIFACTSTAGINGDIRECTORY/rpcs3-v${COMM_TAG}-${COMM_COUNT}-${COMM_HASH}_macos.dmg" \
RPCS3.app

7z a -mx9 rpcs3-v"${COMM_TAG}"-"${COMM_COUNT}"-"${COMM_HASH}"_macos.7z RPCS3.app
mv rpcs3-v"${COMM_TAG}"-"${COMM_COUNT}"-"${COMM_HASH}"_macos.7z "$ARTDIR"