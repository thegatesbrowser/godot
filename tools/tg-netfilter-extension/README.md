# tg-netfilter-extension

macOS NEFilterDataProvider system extension for TheGates.

This directory holds the source for the system extension that ships embedded
in the launcher's `.app` bundle and blocks renderer outbound traffic to
private network destinations.

## Files

- `FilterDataProvider.mm` — principal class. NEFilterDataProvider subclass.
  Installs static drop rules for RFC 1918 / loopback / link-local during
  `startFilter`; the kernel evaluates them in-kernel before `handleNewFlow`
  is invoked, so the runtime overhead for the common case is zero.
- `Info.plist` — extension bundle metadata. `NSExtensionPointIdentifier`
  is `com.apple.networkextension.filter-data`.
- `Extension.entitlements` — entitlements for the system extension target.
  Uses `content-filter-provider-systemextension` (Developer ID variant).
- `Container.entitlements` — entitlements for the launcher app target.
  Needs `system-extension.install` plus the same NE entitlement.

## Xcode project setup

This source is not built by the Godot SCons system. Integrating with the
launcher's macOS distribution requires an Xcode project step:

1. Create a new target in the launcher's Xcode project:
   - Template: **System Extension** (under macOS).
   - Provider type: **Network Extension Provider**.
   - Subtype: **Filter Data**.
2. Replace the Xcode-generated principal class file with `FilterDataProvider.mm`.
3. Replace the generated Info.plist with `Info.plist`.
4. Set the extension target's entitlements file to `Extension.entitlements`.
5. Add the same files plus `Container.entitlements` to the launcher target's
   Code Signing entitlements.
6. In the launcher target's Build Phases, add a **Copy Files** phase:
   - Destination: **Wrapper**
   - Subpath: `Contents/Library/SystemExtensions`
   - Add the system extension product.

## Placeholders to fill before shipping

- **`YOUR_TEAM_ID`** in `Extension.entitlements`, `Container.entitlements`,
  and `FilterDataProvider.mm` — your Apple Developer Team ID (10-character
  identifier). Required for the application-group identifier and for the
  designated-requirement check on the renderer's audit token.
- **Renderer bundle identifier** in `FilterDataProvider.mm` —
  `flowIsFromRenderer:` currently checks `io.thegates.renderer`. Match
  whatever identifier your renderer binary is signed with.

## Signing notes

The `-systemextension` entitlement suffix has known issues with Xcode's
Direct Distribution workflow (Apple radar 108838909). For Developer ID
distribution:

1. Archive normally with development signing.
2. Re-sign from the inside out manually: extension first, then app.
3. Use a Developer ID provisioning profile for both targets.
4. Notarize with `notarytool`.

During development, use Apple Development signing with the non-suffixed
`content-filter-provider` entitlement value. Run
`systemextensionsctl developer on` on the dev machine to allow unsigned
system extensions to load.

## How the launcher activates the extension

In the launcher's existing Objective-C++ code (e.g., extending
`modules/the_gates/sandbox/macos/sandbox_macos.mm`), submit an activation
request at first launch:

```objc
OSSystemExtensionRequest *req = [OSSystemExtensionRequest
        activationRequestForExtensionWithIdentifier:@"io.thegates.launcher.netfilter"
                                              queue:dispatch_get_main_queue()];
req.delegate = self;
[[OSSystemExtensionManager sharedManager] submitRequest:req];
```

The user gets a system notification, opens System Settings, clicks Allow.
After that, `NEFilterManager.sharedManager` activates the filter via
`isEnabled = YES` + `saveToPreferencesWithCompletionHandler:`.

The launcher's `NetworkFilterMacOS::engage()` in
`modules/the_gates/network/macos/network_filter_macos.mm` then verifies
the extension is enabled before allowing renderer spawn.

## Audit token reliability

The audit token on `NEFilterFlow.sourceAppAuditToken` is set by the kernel
at process creation and is stable for the lifetime of the process. For
`posix_spawn`-spawned renderers, the token correctly reflects the child,
not the parent. The designated-requirement check via
`SecCodeCopyGuestWithAttributes` survives PID reuse and can't be spoofed
by a renamed binary.

Reference: [Apple Developer forum 743812](https://developer.apple.com/forums/thread/743812).
