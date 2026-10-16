#import "GP32JIT.h"
#include <TargetConditionals.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <dlfcn.h>
#include <mach/mach.h>

#if GP32EMU_IOS_DEVELOPER_JIT && !TARGET_OS_SIMULATOR
extern CFTypeRef SecTaskCreateFromSelf(CFAllocatorRef allocator);
extern CFTypeRef SecTaskCopyValueForEntitlement(CFTypeRef task, CFStringRef entitlement,
                                               CFErrorRef *error);
#endif

NSString *GP32JITExternalError(void) {
#if GP32EMU_IOS_DEVELOPER_JIT && defined(__aarch64__) && !TARGET_OS_SIMULATOR
    CFTypeRef task = SecTaskCreateFromSelf(kCFAllocatorDefault);
    CFTypeRef allowed = task ? SecTaskCopyValueForEntitlement(task, CFSTR("get-task-allow"), NULL) : NULL;
    BOOL debuggable = allowed && CFEqual(allowed, kCFBooleanTrue);
    if (allowed) CFRelease(allowed);
    if (task) CFRelease(task);
    if (!debuggable) return @"This installation cannot be debugged. Re-sign with get-task-allow enabled to use JIT.";

    /* StikDebug runs universal.js only on TXM devices. Never assume an unknown
     * hardware result is absent and execute a trap after an attach-only flow. */
    void *io = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_LAZY | RTLD_LOCAL);
    if (!io) return @"Could not determine JIT compatibility. The interpreter remains available.";
    mach_port_t (*entryFromPath)(mach_port_t, const char *) = dlsym(io, "IORegistryEntryFromPath");
    CFTypeRef (*copyProperty)(mach_port_t, CFStringRef, CFAllocatorRef, uint32_t) =
        dlsym(io, "IORegistryEntryCreateCFProperty");
    kern_return_t (*releaseObject)(mach_port_t) = dlsym(io, "IOObjectRelease");
    mach_port_t entry = entryFromPath && copyProperty && releaseObject
        ? entryFromPath(MACH_PORT_NULL, "IODeviceTree:/chosen/memory-map") : MACH_PORT_NULL;
    CFTypeRef keys = entry ? copyProperty(entry, CFSTR("IORegistryEntryPropertyKeys"), kCFAllocatorDefault, 0) : NULL;
    BOOL known = keys && CFGetTypeID(keys) == CFArrayGetTypeID();
    BOOL txm = known && CFArrayContainsValue((CFArrayRef)keys,
        CFRangeMake(0, CFArrayGetCount((CFArrayRef)keys)), CFSTR("TXM"));
    if (keys) CFRelease(keys);
    if (entry) releaseObject(entry);
    dlclose(io);
    if (!known) return @"Could not determine JIT compatibility. The interpreter remains available.";
    if (!txm) return @"Automatic JIT setup is not supported on this device yet. The interpreter remains available.";
    return nil;
#else
    return @"This build does not include device JIT support.";
#endif
}

NSURL *GP32JITExternalURL(void) {
    NSString *bundle = NSBundle.mainBundle.bundleIdentifier;
    if (!bundle.length) return nil;
    NSURLComponents *url = [[NSURLComponents alloc] init];
    url.scheme = @"stikdebug";
    url.host = @"enable-jit";
    url.queryItems = @[[NSURLQueryItem queryItemWithName:@"bundle-id" value:bundle],
        [NSURLQueryItem queryItemWithName:@"pid" value:[NSString stringWithFormat:@"%d", getpid()]],
        [NSURLQueryItem queryItemWithName:@"script-name" value:@"universal.js"]];
    return url.URL;
}

BOOL GP32JITDebuggerAttached(void) {
    struct kinfo_proc info = {0};
    size_t size = sizeof(info);
    int query[] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    return sysctl(query, 4, &info, &size, NULL, 0) == 0 &&
           size == sizeof(info) && (info.kp_proc.p_flag & P_TRACED) != 0;
}

#if GP32EMU_IOS_DEVELOPER_JIT && defined(__aarch64__) && !TARGET_OS_SIMULATOR
/* StikDebug universal.js ABI: x16 selects the operation, x0/x1 carry the
 * region and size, and the debugger advances past BRK before returning.
 * Protocol: https://github.com/StikDebug/StikJIT/blob/main/INTEGRATION.md */
__attribute__((naked, noinline))
static void *GP32JITUniversalPrepare(void *address __attribute__((unused)),
                                     size_t length __attribute__((unused))) {
    __asm__ volatile("mov x16, #1\n\tbrk #0xf00d\n\tret");
}

__attribute__((naked, noinline))
static void GP32JITUniversalDetach(void) {
    __asm__ volatile("mov x16, #0\n\tbrk #0xf00d\n\tret");
}
#endif

BOOL GP32JITEnableWithUniversalScript(gp32_ios_emulator_t *emulator) {
#if GP32EMU_IOS_DEVELOPER_JIT && defined(__aarch64__) && !TARGET_OS_SIMULATOR
    if (gp32_ios_emulator_jit_enabled(emulator)) return YES;
    if (!GP32JITDebuggerAttached()) return NO;
    gp32_ios_jit_region_t region;
    BOOL prepared = gp32_ios_emulator_prepare_jit(emulator, &region) != 0;
    if (prepared) {
        /* The script uses a fixed 16 KiB step. Reject other host page sizes
         * rather than accepting partially initialized executable memory. */
        prepared = region.page_size == 16384 &&
                   GP32JITUniversalPrepare(region.execute, region.size) == region.execute;
    }
    /* Even failed allocation must release the waiting script's attachment.
     * No guest code executes until detach and the per-page proof both pass. */
    GP32JITUniversalDetach();
    return prepared && gp32_ios_emulator_activate_jit_universal(emulator);
#else
    (void)emulator;
    return NO;
#endif
}
