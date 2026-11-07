import Foundation
import StikJIT

@objc(GP32JITHelper)
final class GP32JITHelper: NSObject, NSExtensionRequestHandling {
    private static let queue = DispatchQueue(label: "org.gp32emu.jit-helper")

    func beginRequest(with context: NSExtensionContext) {
        let request = (context.inputItems.first as? NSExtensionItem)?.userInfo?["GP32JITRequest"] as? [String: Any]
        Self.queue.async {
            let result: [String: Any]
            do {
                guard let request, let operation = request["operation"] as? String else {
                    throw Self.error("The JIT helper received no request.")
                }
                result = try Self.perform(operation, request: request)
            } catch {
                result = ["success": false, "message": error.localizedDescription]
            }
            let item = NSExtensionItem()
            item.userInfo = ["GP32JITResponse": result]
            context.completeRequest(returningItems: [item], completionHandler: nil)
        }
    }

    private static func error(_ message: String) -> NSError {
        NSError(domain: "GP32JITHelper", code: 1, userInfo: [NSLocalizedDescriptionKey: message])
    }

    private static func perform(_ operation: String, request: [String: Any]) throws -> [String: Any] {
        let files = FileManager.default
        let library = files.urls(for: .libraryDirectory, in: .userDomainMask)[0]
        let cache = library.appendingPathComponent("StikJIT", isDirectory: true)
        try files.createDirectory(at: cache, withIntermediateDirectories: true)
        let paths = DDIPaths.default(in: cache)
        if operation == "reset" {
            try StikJIT.resetCachedDDI(at: paths)
            return ["success": true, "message": "JIT support files reset. They will be downloaded again when needed."]
        }
        guard let pairing = request["pairing"] as? Data, !pairing.isEmpty, pairing.count <= 1024 * 1024 else {
            throw error("The pairing file is missing or invalid.")
        }
        let temporary = cache.appendingPathComponent("pairing-\(UUID().uuidString).plist")
        try pairing.write(to: temporary, options: [.atomic, .completeFileProtectionUntilFirstUserAuthentication])
        defer { try? files.removeItem(at: temporary) }
        if operation == "prepare" {
            switch StikJIT.prepareDevice(pairingFile: temporary, paths: paths) {
            case .ready:
                return ["success": true, "message": "JIT setup is ready. Open a game to start with JIT."]
            case .unreachable(let reason), .preparationFailed(let reason):
                throw error(reason)
            }
        }
        guard operation == "enable", let pid = request["pid"] as? NSNumber, pid.int32Value > 0 else {
            throw error("The JIT helper received an invalid operation or process.")
        }
        // This script is fixed to the host's BRK protocol, never supplied by a game.
        try StikJIT.enableJIT(targetPID: pid.int32Value, pairingFile: temporary,
                              ddiPaths: paths, script: .universal)
        return ["success": true, "message": "JIT helper finished."]
    }
}
