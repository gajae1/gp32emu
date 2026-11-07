#import "GP32JITService.h"
#import "GP32JIT.h"
#include <TargetConditionals.h>
#include <dlfcn.h>
#include <unistd.h>

/* NSExtension hosts our own embedded extension. Resolve at runtime because
 * these hosting selectors are not part of the public iOS SDK. */
@interface NSObject (GP32ExtensionHosting)
+ (id)extensionWithIdentifier:(NSString *)identifier error:(NSError **)error;
- (void)beginExtensionRequestWithInputItems:(NSArray *)items completion:(void (^)(NSUUID *))completion;
- (void)setRequestCompletionBlock:(void (^)(NSUUID *, NSArray *))block;
- (void)setRequestCancellationBlock:(void (^)(NSUUID *, NSError *))block;
- (void)setRequestInterruptionBlock:(void (^)(NSUUID *))block;
- (void)cancelExtensionRequestWithIdentifier:(NSUUID *)identifier;
@end

static BOOL GP32JITPairingDataValid(NSData *data) {
    if (!data.length || data.length > 1024 * 1024) return NO;
    id record = [NSPropertyListSerialization propertyListWithData:data options:NSPropertyListImmutable
        format:NULL error:NULL];
    if (![record isKindOfClass:NSDictionary.class]) return NO;
    return [record[@"public_key"] isKindOfClass:NSData.class] && [record[@"public_key"] length] == 32 &&
           [record[@"private_key"] isKindOfClass:NSData.class] && [record[@"private_key"] length] == 32 &&
           [record[@"identifier"] isKindOfClass:NSString.class] && [record[@"identifier"] length] > 0;
}

@implementation GP32JITService {
    id _extension;
    NSUUID *_request;
    void (^_completion)(BOOL, NSString *);
    BOOL _cancelled;
    NSUInteger _generation;
}

+ (NSURL *)pairingURL {
    NSURL *documents = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory
                                                          inDomains:NSUserDomainMask].firstObject;
    return [documents URLByAppendingPathComponent:@"StikJIT/pairingFile.plist"];
}

+ (BOOL)hasPairingFile {
    return GP32JITPairingDataValid([NSData dataWithContentsOfURL:self.pairingURL]);
}

+ (NSBundle *)helperBundle {
    NSURL *url = [NSBundle.mainBundle.builtInPlugInsURL URLByAppendingPathComponent:@"GP32JITHelper.appex"];
    return url ? [NSBundle bundleWithURL:url] : nil;
}

+ (NSString *)availabilityError {
#if TARGET_OS_SIMULATOR || !GP32EMU_IOS_BUILTIN_JIT
    return @"Built-in JIT is not included in this build.";
#else
    if (@available(iOS 17.4, *)) {} else return @"Built-in JIT requires iOS 17.4 or later.";
    if (getenv("LC_HOME_PATH")) return @"Built-in JIT is unavailable inside LiveContainer. Use StikDebug instead.";
    NSString *permission = GP32JITExternalError();
    if (permission) return permission;
    if (![self helperBundle].bundleIdentifier.length)
        return @"The JIT helper is missing. Reinstall the app with its app extensions included.";
    return nil;
#endif
}

+ (BOOL)importPairingURL:(NSURL *)source error:(NSError **)error {
    NSNumber *size = nil;
    if (![source getResourceValue:&size forKey:NSURLFileSizeKey error:error]) return NO;
    if (!size.unsignedLongLongValue || size.unsignedLongLongValue > 1024 * 1024) {
        if (error) *error = [NSError errorWithDomain:NSCocoaErrorDomain code:NSFileReadCorruptFileError
            userInfo:@{NSLocalizedDescriptionKey:@"Choose a valid pairing file smaller than 1 MiB."}];
        return NO;
    }
    NSData *data = [NSData dataWithContentsOfURL:source options:0 error:error];
    if (!GP32JITPairingDataValid(data)) {
        if (error && !*error) *error = [NSError errorWithDomain:NSCocoaErrorDomain code:NSFileReadCorruptFileError
            userInfo:@{NSLocalizedDescriptionKey:@"Choose a remote pairing file from a current iloader. Older pairing formats cannot be used."}];
        return NO;
    }
    NSURL *destination = self.pairingURL;
    if (![NSFileManager.defaultManager createDirectoryAtURL:destination.URLByDeletingLastPathComponent
        withIntermediateDirectories:YES attributes:nil error:error]) return NO;
    if (![data writeToURL:destination options:(NSDataWritingAtomic | NSDataWritingFileProtectionCompleteUntilFirstUserAuthentication)
        error:error]) return NO;
    [destination setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:NULL];
    return YES;
}

- (BOOL)busy { return _extension != nil; }

- (void)finish:(BOOL)success message:(NSString *)message {
    NSAssert(NSThread.isMainThread, @"JIT service belongs to the main thread");
    void (^completion)(BOOL, NSString *) = _completion;
    _completion = nil;
    _request = nil;
    _extension = nil;
    if (completion) completion(success, message);
}

- (void)performOperation:(NSString *)operation completion:(void (^)(BOOL, NSString *))completion {
    NSAssert(NSThread.isMainThread, @"JIT service belongs to the main thread");
    NSString *error = self.class.availabilityError;
    if (self.busy) error = @"The previous JIT request is still finishing. Try again shortly.";
    NSData *pairing = nil;
    if (!error && ![operation isEqualToString:@"reset"]) {
        pairing = [NSData dataWithContentsOfURL:self.class.pairingURL options:0 error:NULL];
        if (!GP32JITPairingDataValid(pairing))
            error = @"Import this iPhone's pairing file in JIT setup first.";
    }
    if (error) { completion(NO, error); return; }
    if (!NSClassFromString(@"NSExtension"))
        dlopen("/System/Library/Frameworks/ExtensionFoundation.framework/ExtensionFoundation", RTLD_NOW | RTLD_LOCAL);
    Class extensionClass = NSClassFromString(@"NSExtension");
    NSError *launchError = nil;
    id helper = [extensionClass extensionWithIdentifier:self.class.helperBundle.bundleIdentifier error:&launchError];
    if (!helper) { completion(NO, launchError.localizedDescription ?: @"iOS could not start the JIT helper."); return; }
    _extension = helper;
    _completion = [completion copy];
    _cancelled = NO;
    NSUInteger generation = ++_generation;
    __weak typeof(self) weakSelf = self;
    [helper setRequestCompletionBlock:^(NSUUID *identifier, NSArray *items) {
        (void)identifier;
        id payload = [items.firstObject isKindOfClass:NSExtensionItem.class]
            ? ((NSExtensionItem *)items.firstObject).userInfo[@"GP32JITResponse"] : nil;
        NSDictionary *response = [payload isKindOfClass:NSDictionary.class] ? payload : nil;
        dispatch_async(dispatch_get_main_queue(), ^{
            typeof(self) self = weakSelf;
            if (!self || generation != self->_generation || !self->_extension) return;
            BOOL success = [response[@"success"] boolValue] && !self->_cancelled;
            NSString *message = [response[@"message"] isKindOfClass:NSString.class] ? response[@"message"] : @"The JIT helper returned no result.";
            [self finish:success message:message];
        });
    }];
    [helper setRequestCancellationBlock:^(NSUUID *identifier, NSError *reason) {
        (void)identifier;
        dispatch_async(dispatch_get_main_queue(), ^{
            typeof(self) self = weakSelf;
            if (self && generation == self->_generation && self->_extension)
                [self finish:NO message:reason.localizedDescription ?: @"JIT setup was cancelled."];
        });
    }];
    [helper setRequestInterruptionBlock:^(NSUUID *identifier) {
        (void)identifier;
        dispatch_async(dispatch_get_main_queue(), ^{
            typeof(self) self = weakSelf;
            if (self && generation == self->_generation && self->_extension)
                [self finish:NO message:@"The JIT helper stopped. Retry setup."];
        });
    }];
    NSExtensionItem *item = [[NSExtensionItem alloc] init];
    item.userInfo = @{@"GP32JITRequest":@{@"operation":operation, @"pid":@(getpid()), @"pairing":pairing ?: NSData.data}};
    [helper beginExtensionRequestWithInputItems:@[item] completion:^(NSUUID *identifier) {
        dispatch_async(dispatch_get_main_queue(), ^{
            typeof(self) self = weakSelf;
            if (!self || generation != self->_generation || self->_extension != helper) return;
            if (!identifier) { [self finish:NO message:@"The JIT helper did not start. Reinstall with app extensions included."]; return; }
            self->_request = identifier;
            if (self->_cancelled) [self cancel];
        });
    }];
}

- (void)cancel {
    _cancelled = YES;
    if (_request && [_extension respondsToSelector:@selector(cancelExtensionRequestWithIdentifier:)])
        [_extension cancelExtensionRequestWithIdentifier:_request];
    /* Keep ownership until iOS reports completion/cancellation. A stale helper
     * must not attach while a second request is already in flight. */
}
@end
