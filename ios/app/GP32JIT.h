#import <Foundation/Foundation.h>
#include "gp32_ios.h"

/* Current attachment, not the persistent CS_DEBUGGED flag. */
BOOL GP32JITDebuggerAttached(void);
/* nil means the current installation supports the external script workflow. */
NSString * _Nullable GP32JITExternalError(void);
NSURL * _Nullable GP32JITExternalURL(void);

/* Owner queue only. Call ONLY after explicitly requesting the matching
 * universal.js debugger script. A normal debugger cannot handle these traps.
 * No core may run or change content until this function returns. */
BOOL GP32JITEnableWithUniversalScript(gp32_ios_emulator_t * _Nullable emulator);
