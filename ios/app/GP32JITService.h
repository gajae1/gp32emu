#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN
/* Main-thread coordinator for the separately hosted activation helper. */
@interface GP32JITService : NSObject
@property (nonatomic, readonly) BOOL busy;
+ (NSURL *)pairingURL;
+ (BOOL)hasPairingFile;
+ (nullable NSString *)availabilityError;
+ (BOOL)importPairingURL:(NSURL *)source error:(NSError **)error;
- (void)performOperation:(NSString *)operation
             completion:(void (^)(BOOL success, NSString *message))completion;
- (void)cancel;
@end
NS_ASSUME_NONNULL_END
