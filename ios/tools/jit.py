"""LLDB commands for GP32emu's opt-in iOS developer JIT build.

Import with `command script import /absolute/path/ios/tools/jit.py` after
attaching to your own GP32emu app. See ../README.md for the two-step workflow.
No pairing records, signing identities or process addresses are saved here.
"""
import lldb


def command(debugger, text):
    result = lldb.SBCommandReturnObject()
    debugger.GetCommandInterpreter().HandleCommand(text, result)
    if not result.Succeeded():
        raise RuntimeError(result.GetError())
    return result.GetOutput()


def stopped(debugger):
    process = debugger.GetSelectedTarget().GetProcess()
    if not process.IsValid() or process.GetState() != lldb.eStateStopped:
        raise RuntimeError("Attach to GP32emu and interrupt it first.")
    return process


def value(debugger, expression):
    options = lldb.SBExpressionOptions()
    options.SetLanguage(lldb.eLanguageTypeObjC_plus_plus)
    v = debugger.GetSelectedTarget().EvaluateExpression(expression, options)
    if v.GetError().Fail():
        raise RuntimeError(str(v.GetError()))
    return v.GetValueAsUnsigned()


def resume(debugger, process):
    previous = debugger.GetAsync()
    debugger.SetAsync(True)
    try:
        error = process.Continue()
        if error.Fail():
            raise RuntimeError(str(error))
    finally:
        debugger.SetAsync(previous)


def bind_object(debugger, name, expression):
    # LLDB rejects redeclaring a persistent variable in the same session.
    # An existing pointer is only tested as an integer, never messaged.
    try:
        value(debugger, f"(uintptr_t){name}")
        statement = f"(void)({name} = {expression})"
    except RuntimeError:
        statement = f"id {name} = {expression}"
    command(debugger, "expr -l objc++ -- " + statement)


def find_owner(debugger):
    # Reacquire the app object on every command: a previous process or scene's
    # LLDB object variable is not a lifetime guarantee.
    command(debugger, "expr -l objc++ -- @import UIKit")
    bind_object(debugger, "$gp32_jit_owner", '[(NSObject *)'
                '((UIWindowScene *)[[UIApplication sharedApplication].connectedScenes '
                'anyObject]).windows.firstObject.rootViewController valueForKey:@"emulator"]')
    if not value(debugger, '(BOOL)[(NSObject *)$gp32_jit_owner respondsToSelector:NSSelectorFromString(@"prepareJIT")]'):
        raise RuntimeError("This app does not include the developer JIT option.")


def is_active(debugger):
    return value(debugger, '(BOOL)[(NSNumber *)[(NSObject *)$gp32_jit_owner valueForKey:@"jitActive"] boolValue]') != 0


def prepare(debugger, arguments, result, internal_dict):
    try:
        process = stopped(debugger)
        find_owner(debugger)
        if is_active(debugger):
            result.AppendMessage("JIT is already enabled. No memory was changed; continue or detach to resume.")
            return
        command(debugger, 'expr -l objc++ -- (void)[(NSObject *)$gp32_jit_owner '
                'performSelectorOnMainThread:NSSelectorFromString(@"prepareJIT") '
                'withObject:nil waitUntilDone:NO]')
        resume(debugger, process)
        result.AppendMessage("Preparation queued. Wait for the app status, interrupt, then run gp32-jit-enable.")
    except Exception as error:
        result.SetError(str(error))


def enable(debugger, arguments, result, internal_dict):
    try:
        process = stopped(debugger)
        find_owner(debugger)
        if is_active(debugger):
            result.AppendMessage("JIT is already enabled. No memory was changed; continue or detach to resume.")
            return
        bind_object(debugger, "$gp32_jit_region",
                '[(NSObject *)$gp32_jit_owner valueForKey:@"jitPreparation"]')
        if not value(debugger, '(uintptr_t)$gp32_jit_region'):
            raise RuntimeError("No prepared region. Load a game and run gp32-jit-prepare first.")
        fields = {key: value(debugger, '(unsigned long long)[(NSNumber *)'
                  '[(NSDictionary *)$gp32_jit_region objectForKey:@"' + key + '"] unsignedLongLongValue]')
                  for key in ("address", "size", "pageSize", "marker")}
        address, size, page, marker = (fields[k] for k in ("address", "size", "pageSize", "marker"))
        if (not address or not page or size <= 0 or size > 128*1024*1024 or
                address % page or size % page or marker != 0xd4200000):
            raise RuntimeError("Invalid prepared region; nothing was written.")
        if not value(debugger, '(BOOL)([(NSObject *)$gp32_jit_owner valueForKey:@"jitPreparation"] == $gp32_jit_region)'):
            raise RuntimeError("Content changed during setup; prepare again.")
        # Initialize every byte, not merely one witness word per host page.
        chunk = marker.to_bytes(4, "little") * (1024*1024//4)
        error = lldb.SBError()
        for offset in range(0, size, len(chunk)):
            data = chunk[:min(len(chunk), size-offset)]
            written = process.WriteMemory(address+offset, data, error)
            if error.Fail() or written != len(data):
                raise RuntimeError(f"Initialization stopped at {offset}: {error}")
        # Activation releases this dictionary. Do not leave a dangling LLDB
        # object variable that a later diagnostic might accidentally message.
        command(debugger, 'expr -l objc++ -- (void)($gp32_jit_region = nil)')
        command(debugger, 'expr -l objc++ -- (void)[(NSObject *)$gp32_jit_owner '
                'performSelectorOnMainThread:NSSelectorFromString(@"activateJIT") '
                'withObject:nil waitUntilDone:NO]')
        resume(debugger, process)
        result.AppendMessage(f"Initialized {size} bytes; activation queued. Interrupt and use gp32-jit-status to confirm.")
    except Exception as error:
        result.SetError(str(error))
    finally:
        if debugger.GetSelectedTarget().GetProcess().GetState() == lldb.eStateStopped:
            try:
                command(debugger, 'expr -l objc++ -- (void)($gp32_jit_region = nil)')
            except RuntimeError:
                pass  # Failure before the variable was created.


def status(debugger, arguments, result, internal_dict):
    try:
        stopped(debugger)
        find_owner(debugger)
        if is_active(debugger):
            result.AppendMessage("JIT enabled.")
        elif value(debugger, '(uintptr_t)[(NSObject *)$gp32_jit_owner valueForKey:@"jitPreparation"]'):
            result.AppendMessage("Interpreter; JIT memory prepared but not activated.")
        else:
            result.AppendMessage("Interpreter; no JIT preparation pending.")
    except Exception as error:
        result.SetError(str(error))


def __lldb_init_module(debugger, internal_dict):
    debugger.HandleCommand(f"command script add --overwrite -f {__name__}.prepare gp32-jit-prepare")
    debugger.HandleCommand(f"command script add --overwrite -f {__name__}.enable gp32-jit-enable")
    debugger.HandleCommand(f"command script add --overwrite -f {__name__}.status gp32-jit-status")
