#!/bin/sh
# Have Xcode create a development provisioning profile for a bundle identifier, so that
# `philae pack --target device --bundle-id <id>` can sign for a device.
#
#   scripts/make-dev-profile.sh <bundle-id> <team-id> [--check]
#
# A converted app needs a profile for its own bundle identifier, and profiles only come
# from Apple's developer portal. Xcode obtains one as a side effect of building a project
# with automatic signing, so this builds a throwaway empty app with that identifier.
#
# This registers the identifier (an "App ID") in the team's developer account and needs
# the Apple ID signed in to Xcode. The device must already be registered with the team
# (it is, if Xcode has ever run an app on it). A free account allows 10 new identifiers
# per week. With --check, only the throwaway project is built, unsigned: nothing is
# registered.
set -eu
[ $# -ge 2 ] || { sed -n '2,16p' "$0"; exit 1; }
bundle_id=$1; team=$2; check=${3:-}
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
mkdir -p "$dir/Stub.xcodeproj"
cat > "$dir/main.m" <<'SRC'
#import <UIKit/UIKit.h>
int main(int argc, char *argv[]) { @autoreleasepool { return UIApplicationMain(argc, argv, nil, nil); } }
SRC
cat > "$dir/Stub.xcodeproj/project.pbxproj" <<'PBX'
// !$*UTF8*$!
{
	archiveVersion = 1;
	classes = {};
	objectVersion = 56;
	objects = {
		A0000000000000000000001 /* main.m */ = {isa = PBXFileReference; lastKnownFileType = sourcecode.c.objc; path = main.m; sourceTree = "<group>"; };
		A0000000000000000000002 /* Stub.app */ = {isa = PBXFileReference; explicitFileType = wrapper.application; includeInIndex = 0; path = Stub.app; sourceTree = BUILT_PRODUCTS_DIR; };
		A0000000000000000000003 /* main.m in Sources */ = {isa = PBXBuildFile; fileRef = A0000000000000000000001; };
		A0000000000000000000004 = {isa = PBXGroup; children = (A0000000000000000000001, A0000000000000000000002); sourceTree = "<group>"; };
		A0000000000000000000005 /* Sources */ = {isa = PBXSourcesBuildPhase; buildActionMask = 2147483647; files = (A0000000000000000000003); runOnlyForDeploymentPostprocessing = 0; };
		A0000000000000000000006 /* Stub */ = {isa = PBXNativeTarget; buildConfigurationList = A0000000000000000000009; buildPhases = (A0000000000000000000005); buildRules = (); dependencies = (); name = Stub; productName = Stub; productReference = A0000000000000000000002; productType = "com.apple.product-type.application"; };
		A0000000000000000000007 /* Project */ = {isa = PBXProject; buildConfigurationList = A000000000000000000000A; compatibilityVersion = "Xcode 14.0"; mainGroup = A0000000000000000000004; productRefGroup = A0000000000000000000004; projectDirPath = ""; projectRoot = ""; targets = (A0000000000000000000006); };
		A0000000000000000000008 /* Debug */ = {isa = XCBuildConfiguration; buildSettings = {CODE_SIGN_STYLE = Automatic; GENERATE_INFOPLIST_FILE = YES; OTHER_LDFLAGS = ("-framework", UIKit, "-framework", Foundation); INFOPLIST_KEY_UILaunchScreen_Generation = YES; IPHONEOS_DEPLOYMENT_TARGET = 17.4; PRODUCT_NAME = Stub; SDKROOT = iphoneos; TARGETED_DEVICE_FAMILY = "1,2"; }; name = Debug; };
		A0000000000000000000009 = {isa = XCConfigurationList; buildConfigurations = (A0000000000000000000008); defaultConfigurationName = Debug; };
		A000000000000000000000A = {isa = XCConfigurationList; buildConfigurations = (A000000000000000000000B); defaultConfigurationName = Debug; };
		A000000000000000000000B /* Debug */ = {isa = XCBuildConfiguration; buildSettings = {}; name = Debug; };
	};
	rootObject = A0000000000000000000007;
}
PBX
if [ "$check" = "--check" ]; then
    signing="CODE_SIGNING_ALLOWED=NO"
    provisioning=""
else
    signing="CODE_SIGN_IDENTITY=Apple Development"
    provisioning="-allowProvisioningUpdates"
fi
xcodebuild -project "$dir/Stub.xcodeproj" -target Stub -configuration Debug -sdk iphoneos \
    $provisioning SYMROOT="$dir/build" OBJROOT="$dir/obj" \
    PRODUCT_BUNDLE_IDENTIFIER="$bundle_id" DEVELOPMENT_TEAM="$team" "$signing" build \
    | grep -E "error|warning: .*[Pp]rovision|BUILD (SUCCEEDED|FAILED)" || true
if [ "$check" != "--check" ]; then
    echo "profiles now installed for:"
    for f in "$HOME/Library/Developer/Xcode/UserData/Provisioning Profiles"/*; do
        security cms -D -i "$f" 2>/dev/null | plutil -extract Entitlements.application-identifier raw - 2>/dev/null
    done
fi
