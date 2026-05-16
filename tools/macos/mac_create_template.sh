set -o xtrace

lipo -create bin/godot.macos.template_release.x86_64 bin/godot.macos.template_release.arm64 -output bin/godot.macos.template_release.universal
lipo -create bin/godot.macos.template_debug.sandbox.x86_64 bin/godot.macos.template_debug.sandbox.arm64 -output bin/godot.macos.template_debug.sandbox.universal

cp bin/godot.macos.template_release.universal bin/macos_template.app/Contents/MacOS/godot_macos_release.universal
cp bin/godot.macos.template_debug.sandbox.universal bin/macos_template.app/Contents/Frameworks/Sandbox.universal

rm bin/macos.zip
(cd bin && zip -q -9 -r macos.zip macos_template.app)

set +o xtrace
