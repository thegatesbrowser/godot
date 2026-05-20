// FilterDataProvider.mm — TheGates macOS NEFilterDataProvider system extension.
//
// Principal class for the NetworkExtension content-filter system extension that
// drops outbound flows from TheGates renderer to private network destinations
// (RFC 1918 / loopback / link-local).
//
// Build/package: this file goes into a .systemextension target inside the
// launcher's Xcode project. See README.md for the project structure.
//
// Entitlements (Extension.entitlements):
//   com.apple.developer.networking.networkextension = [content-filter-provider-systemextension]
//   com.apple.security.app-sandbox = true
//
// Info.plist principal class: FilterDataProvider
// Info.plist NSExtensionPointIdentifier: com.apple.networkextension.filter-data

#import <Foundation/Foundation.h>
#import <NetworkExtension/NetworkExtension.h>
#import <Security/Security.h>
#include <bsm/libbsm.h>

@interface FilterDataProvider : NEFilterDataProvider
@end

@implementation FilterDataProvider

// Build the static drop list for RFC 1918 / loopback / link-local. Returns
// nil and a populated error on failure.
- (NSArray<NEFilterRule *> *)buildDenyRules {
    NSMutableArray<NEFilterRule *> *rules = [NSMutableArray array];

    NSArray<NSString *> *v4Blocks = @[ @"10.0.0.0", @"172.16.0.0", @"192.168.0.0", @"127.0.0.0", @"169.254.0.0" ];
    NSArray<NSNumber *> *v4Prefixes = @[ @8, @12, @16, @8, @16 ];

    for (NSUInteger i = 0; i < v4Blocks.count; i++) {
        NWHostEndpoint *ep = [NWHostEndpoint endpointWithHostname:v4Blocks[i] port:@"0"];
        NENetworkRule *netRule = [[NENetworkRule alloc]
                initWithRemoteNetwork:ep
                         remotePrefix:[v4Prefixes[i] unsignedIntegerValue]
                         localNetwork:nil
                          localPrefix:0
                             protocol:NENetworkRuleProtocolAny
                            direction:NETrafficDirectionOutbound];
        NEFilterRule *fr = [[NEFilterRule alloc] initWithNetworkRule:netRule action:NEFilterActionDrop];
        [rules addObject:fr];
    }

    NSArray<NSString *> *v6Blocks = @[ @"::1", @"fc00::", @"fe80::", @"::ffff:0:0" ];
    NSArray<NSNumber *> *v6Prefixes = @[ @128, @7, @10, @96 ];

    for (NSUInteger i = 0; i < v6Blocks.count; i++) {
        NWHostEndpoint *ep = [NWHostEndpoint endpointWithHostname:v6Blocks[i] port:@"0"];
        NENetworkRule *netRule = [[NENetworkRule alloc]
                initWithRemoteNetwork:ep
                         remotePrefix:[v6Prefixes[i] unsignedIntegerValue]
                         localNetwork:nil
                          localPrefix:0
                             protocol:NENetworkRuleProtocolAny
                            direction:NETrafficDirectionOutbound];
        NEFilterRule *fr = [[NEFilterRule alloc] initWithNetworkRule:netRule action:NEFilterActionDrop];
        [rules addObject:fr];
    }

    return rules;
}

- (void)startFilterWithCompletionHandler:(void (^)(NSError *_Nullable))completionHandler {
    NSArray<NEFilterRule *> *rules = [self buildDenyRules];
    NEFilterSettings *settings = [[NEFilterSettings alloc] initWithRules:rules defaultAction:NEFilterActionAllow];
    [self applySettings:settings
     completionHandler:^(NSError *_Nullable err) {
         NSLog(@"TheGatesFilter: startFilter applied %lu rules (err=%@)",
                 (unsigned long)rules.count, err ? err.localizedDescription : @"nil");
         completionHandler(err);
     }];
}

- (void)stopFilterWithReason:(NEProviderStopReason)reason
            completionHandler:(void (^)(void))completionHandler {
    NSLog(@"TheGatesFilter: stopFilter reason=%ld", (long)reason);
    completionHandler();
}

// Verifies the source flow originated from the TheGates renderer binary by
// checking its code-signing identity (Team ID + bundle identifier) against
// the designated requirement.
- (BOOL)flowIsFromRenderer:(NEFilterFlow *)flow {
    NSData *tokenData = flow.sourceAppAuditToken;
    if (!tokenData || tokenData.length != sizeof(audit_token_t)) {
        return NO;
    }

    SecCodeRef code = NULL;
    NSDictionary *attrs = @{ (__bridge id)kSecGuestAttributeAudit : tokenData };
    OSStatus s = SecCodeCopyGuestWithAttributes(NULL,
            (__bridge CFDictionaryRef)attrs,
            kSecCSDefaultFlags,
            &code);
    if (s != errSecSuccess || code == NULL) {
        return NO;
    }

    SecRequirementRef req = NULL;
    NSString *reqStr = @"identifier \"io.thegates.renderer\" and anchor apple generic "
                       @"and certificate leaf[subject.OU] = \"YOUR_TEAM_ID\"";
    s = SecRequirementCreateWithString((__bridge CFStringRef)reqStr,
            kSecCSDefaultFlags,
            &req);
    if (s != errSecSuccess || req == NULL) {
        CFRelease(code);
        return NO;
    }

    s = SecCodeCheckValidity(code, kSecCSDefaultFlags, req);
    BOOL match = (s == errSecSuccess);
    CFRelease(req);
    CFRelease(code);
    return match;
}

// Fast-path callback: rules above already decide private-IP destinations in
// kernel. This callback only fires for non-matching flows; allow them
// unconditionally for the renderer's process, allow others (this filter
// shouldn't impact other apps even with default-allow upstream).
- (NEFilterNewFlowVerdict *)handleNewFlow:(NEFilterFlow *)flow {
    return [NEFilterNewFlowVerdict allowVerdict];
}

@end
