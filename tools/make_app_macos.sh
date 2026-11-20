#!/usr/bin/env bash
# Package build/macos/red_two into a self-contained macOS app bundle (build/macos/Red Two.app).
#   tools/make_app_macos.sh [--build-dir <dir>] [--disc <path.rvz|.iso>] [--textures <path-to-GSWE64-or-parent>]
# Bundles all non-system dylibs into Contents/Frameworks/ and rewrites their install names so the
# app runs on Apple Silicon Macs without Homebrew installed.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
build="$root/build/macos"
disc=""
textures=""

while [ $# -gt 0 ]; do
    case "$1" in
    --build-dir) build="$2"; shift 2 ;;
    --disc) disc="$2"; shift 2 ;;
    --textures) textures="$2"; shift 2 ;;
    *) build="$1"; shift ;;
    esac
done

bin="$build/red_two"
app="$build/Red Two.app"

if [ ! -x "$bin" ]; then
    echo "missing $bin (run tools/build_macos.sh first)" >&2
    exit 1
fi

rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Frameworks" "$app/Contents/Resources"
cp "$bin" "$app/Contents/MacOS/red_two"
chmod +w "$app/Contents/MacOS/red_two"

# Bundle all non-system dylibs recursively and rewrite load paths to @executable_path/../Frameworks/
python3 - "$app/Contents/MacOS/red_two" "$app/Contents/Frameworks" << 'PYEOF'
import os, shutil, subprocess, sys

main_bin, fw_dir = sys.argv[1], sys.argv[2]

def deps_of(path):
    out = subprocess.check_output(["otool", "-L", path], text=True)
    res = []
    for line in out.splitlines()[1:]:
        dep = line.strip().split(" (compatibility")[0]
        if not dep or dep.startswith("/System/") or dep.startswith("/usr/lib/"):
            continue
        if dep.startswith("@executable_path/") or dep.startswith("@loader_path/"):
            continue
        res.append(dep)
    return res

def resolve_dep(dep, parent_real):
    if dep.startswith("@rpath/"):
        name = dep[len("@rpath/"):]
        for cand_dir in (os.path.dirname(parent_real), "/opt/homebrew/lib", "/usr/local/lib"):
            cand = os.path.join(cand_dir, name)
            if os.path.exists(cand):
                return os.path.realpath(cand)
        # Also search /opt/homebrew/opt/*/lib
        for base in ("/opt/homebrew/opt", "/usr/local/opt"):
            if os.path.isdir(base):
                for entry in os.listdir(base):
                    cand = os.path.join(base, entry, "lib", name)
                    if os.path.exists(cand):
                        return os.path.realpath(cand)
        raise RuntimeError(f"cannot resolve {dep} referenced by {parent_real}")
    return os.path.realpath(dep)

copied = {}  # basename -> real src path
queue = [(main_bin, os.path.realpath(main_bin))]

while queue:
    target, real_target = queue.pop(0)
    for dep in deps_of(target):
        real_dep = resolve_dep(dep, real_target)
        base = os.path.basename(dep) if not dep.startswith("@rpath/") else dep[len("@rpath/"):]
        dst = os.path.join(fw_dir, base)
        if base not in copied:
            shutil.copy2(real_dep, dst)
            os.chmod(dst, 0o755)
            subprocess.run(["install_name_tool", "-id", f"@executable_path/../Frameworks/{base}", dst], check=True)
            copied[base] = real_dep
            queue.append((dst, real_dep))
        subprocess.run(["install_name_tool", "-change", dep, f"@executable_path/../Frameworks/{base}", target], check=True)

print(f"Bundled {len(copied)} dynamic libraries into Contents/Frameworks/")
PYEOF

# Convert res/red_two.png into an .icns bundle icon
if [ -f "$root/res/red_two.png" ] && command -v sips >/dev/null && command -v iconutil >/dev/null; then
    iconset="/tmp/red_two_$$.iconset"
    icns="/tmp/red_two_$$.icns"
    rm -rf "$iconset" "$icns"
    mkdir -p "$iconset"
    for sz in 16 32 128 256 512; do
        sips -s format png -z $sz $sz "$root/res/red_two.png" --out "$iconset/icon_${sz}x${sz}.png" >/dev/null
        sz2=$((sz * 2))
        sips -s format png -z $sz2 $sz2 "$root/res/red_two.png" --out "$iconset/icon_${sz}x${sz}@2x.png" >/dev/null
    done
    iconutil -c icns "$iconset" -o "$icns"
    cp "$icns" "$app/Contents/Resources/red_two.icns"
    rm -rf "$iconset" "$icns"
fi

# Optionally embed disc image inside Contents/Resources/
if [ -n "$disc" ]; then
    ext="${disc##*.}"
    echo "Copying disc image into bundle..."
    cp "$disc" "$app/Contents/Resources/GSWE64.$ext"
fi

# Optionally embed texture pack inside Contents/Resources/textures/GSWE64/
if [ -n "$textures" ]; then
    src_tex="$textures"
    if [ -d "$textures/GSWE64" ]; then
        src_tex="$textures/GSWE64"
    elif [ -d "$textures/GSW" ]; then
        src_tex="$textures/GSW"
    fi
    echo "Copying texture pack into bundle..."
    mkdir -p "$app/Contents/Resources/textures"
    cp -R "$src_tex" "$app/Contents/Resources/textures/GSWE64"
fi

cat > "$app/Contents/Info.plist" << 'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key>
    <string>Red Two</string>
    <key>CFBundleDisplayName</key>
    <string>Red Two</string>
    <key>CFBundleIdentifier</key>
    <string>io.github.gminustwo.red-two</string>
    <key>CFBundleVersion</key>
    <string>1.0</string>
    <key>CFBundleShortVersionString</key>
    <string>1.0</string>
    <key>CFBundleExecutable</key>
    <string>red_two</string>
    <key>CFBundleIconFile</key>
    <string>red_two</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>NSHighResolutionCapable</key>
    <true/>
    <key>LSMinimumSystemVersion</key>
    <string>13.0</string>
</dict>
</plist>
EOF

# Ad-hoc sign the bundle so modified Mach-O binaries & dylibs run cleanly on Apple Silicon
codesign --force --deep --sign - "$app"

echo "Created standalone app: $app"
