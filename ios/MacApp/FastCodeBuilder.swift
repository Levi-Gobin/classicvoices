import CommonCrypto
import Foundation

/// Builds the fast engine code on this Mac, as the other platforms do
/// (nvda/.../fastcode.py): the recompilers (RecompARM.swift, RecompX86.swift,
/// byte for byte tools/recomp.py and recomp_x86.py) translate each imported
/// engine into C, and Zig's C compiler, downloaded from ziglang.org and checked
/// against the SHA-256 in its own index, makes the libraries the speech
/// extension loads from the App Group (VoiceCatalog.nativeDir). What it builds
/// derives from Apple's code and stays on this Mac.
enum FastCodeBuilder {
    typealias Progress = EngineImport.Progress
    struct BuildError: LocalizedError {
        let message: String
        init(_ m: String) { message = m }
        var errorDescription: String? { message }
    }

    /// Engines whose function boundaries come from their BL targets (recomp.py --scan-bl).
    static let stripped: Set<String> = ["ios3", "ios4", "ios5"]
    static let armBuilds = ["ios3", "ios4", "ios5", "ios6", "ios70", "ios7", "ios10"]
    static let macinTalkGens = ["mt104", "mt105", "mt106", "mt107"]

    static var sources: URL? { Bundle.main.resourceURL?.appendingPathComponent("FastCode") }
    static var cache: URL {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Classic Voices")
    }

    static func library(_ build: String, in dir: URL) -> URL {
        dir.appendingPathComponent(build.hasPrefix("mt") ? "cvmtnative_\(build).so" : "cvnative_\(build).so")
    }

    /// A built library is kept as long as the engine it came from and this
    /// app's version are the same (the semantics headers change with the app).
    static func stamp(_ engine: URL) -> String {
        let a = (try? FileManager.default.attributesOfItem(atPath: engine.path)) ?? [:]
        return "\(sourcesHash) \(flagsVersion) \((a[.size] as? Int) ?? 0) \(((a[.modificationDate] as? Date)?.timeIntervalSince1970) ?? 0)"
    }

    /// What the libraries are built from besides the engine: the instruction
    /// semantics, the glue and the profiles in the app (FastCode/). An update
    /// that leaves them alone keeps the fast code; one that changes them (the
    /// interpreter's semantics must match it) rebuilds it.
    static let sourcesHash: String = {
        guard let src = sources, let files = FileManager.default.enumerator(atPath: src.path) else { return "none" }
        var hash = SHA256Stream()
        for case let rel as String in files.allObjects.compactMap({ $0 as? String }).sorted() {
            guard let d = try? Data(contentsOf: src.appendingPathComponent(rel)) else { continue }
            hash.update(Data(rel.utf8))
            hash.update(d)
        }
        return String(hash.hex.prefix(16))
    }()

    /// Bumped when the compiler flags or the recompilers (RecompARM/RecompX86)
    /// change what's built (f2: no SLP vectorizer, NewGVN).
    static let flagsVersion = "f2"

    /// Compiles running now, stopped when the app quits (they would go on
    /// writing into a folder the next launch deletes).
    nonisolated(unsafe) private static var live: Set<Process> = []
    private static let liveLock = NSLock()

    static func stopAll() {
        liveLock.lock(); let all = live; liveLock.unlock()
        all.forEach { $0.terminate() }
    }

    static func isBuilt(_ build: String, engine: URL, in dir: URL) -> Bool {
        let lib = library(build, in: dir)
        return FileManager.default.fileExists(atPath: lib.path)
            && (try? String(contentsOf: lib.appendingPathExtension("stamp"), encoding: .utf8)) == stamp(engine)
    }

    /// The engine file a build's fast code comes from, when it's imported.
    static func engine(_ build: String, root: URL) -> URL? {
        let u = build.hasPrefix("mt") ? root.appendingPathComponent("\(build)/MacinTalk") : root.appendingPathComponent("\(build)/voiced")
        return FileManager.default.fileExists(atPath: u.path) ? u : nil
    }

    /// Builds that have an engine here and no current fast code.
    static func todo(root: URL, native: URL) -> [String] {
        (armBuilds + macinTalkGens).filter { b in engine(b, root: root).map { !isBuilt(b, engine: $0, in: native) } ?? false }
    }

    // MARK: - the compiler

    static func zig(progress: @escaping Progress) throws -> URL {
        let fm = FileManager.default
        let root = cache.appendingPathComponent("zig")
        if let found = try? fm.contentsOfDirectory(atPath: root.path).sorted().reversed()
            .map({ root.appendingPathComponent($0).appendingPathComponent("zig") })
            .first(where: { fm.isExecutableFile(atPath: $0.path) }) {
            return found
        }
        progress("Finding the Zig compiler", nil)
        let (data, _) = try HTTP.get(URL(string: "https://ziglang.org/download/index.json")!)
        guard let index = try JSONSerialization.jsonObject(with: data) as? [String: Any],
              let version = index.keys.filter({ $0 != "master" }).max(by: { UpdateCheck.isNewer($1, than: $0) }),
              let entry = (index[version] as? [String: Any])?[arch + "-macos"] as? [String: Any],
              let tarball = (entry["tarball"] as? String).flatMap(URL.init(string:)), let sha = entry["shasum"] as? String
        else { throw BuildError("ziglang.org didn't list a compiler for this Mac.") }
        try fm.createDirectory(at: root, withIntermediateDirectories: true)
        let archive = root.appendingPathComponent(tarball.lastPathComponent)
        fm.createFile(atPath: archive.path, contents: nil)
        let out = try FileHandle(forWritingTo: archive)
        var hash = SHA256Stream()
        var got = 0
        let total = (entry["size"] as? String).flatMap(Int.init) ?? (entry["size"] as? Int) ?? 0
        progress("Downloading the Zig compiler (\(total >> 20) MB)", 0)
        do {
            try HTTP.stream(tarball, from: 0, to: UInt64(max(total, 1)) - 1) { chunk in
                try out.write(contentsOf: chunk)
                hash.update(chunk)
                got += chunk.count
                if total > 0 { progress(nil, Double(got) / Double(total)) }
            }
            try out.close()
        } catch {
            try? out.close(); try? fm.removeItem(at: archive); throw error
        }
        guard hash.hex == sha.lowercased() else {
            try? fm.removeItem(at: archive)
            throw BuildError("The Zig download doesn't match its checksum.")
        }
        progress("Unpacking the Zig compiler", nil)
        let tmp = root.appendingPathComponent(".unpack")
        try? fm.removeItem(at: tmp)
        try fm.createDirectory(at: tmp, withIntermediateDirectories: true)
        try run(URL(fileURLWithPath: "/usr/bin/tar"), ["-xf", archive.path, "-C", tmp.path])
        try? fm.removeItem(at: archive)
        guard let inner = try fm.contentsOfDirectory(atPath: tmp.path).first else { throw BuildError("The Zig download was empty.") }
        let dest = root.appendingPathComponent(version)
        try? fm.removeItem(at: dest)
        try fm.moveItem(at: tmp.appendingPathComponent(inner), to: dest)
        try? fm.removeItem(at: tmp)
        return dest.appendingPathComponent("zig")
    }

    static var arch: String {
        #if arch(arm64)
        "aarch64"
        #else
        "x86_64"
        #endif
    }

    // MARK: - building

    /// Translates and compiles one engine's fast code into `native`.
    static func build(_ build: String, root: URL, native: URL, zig: URL, progress: Progress) throws {
        guard let src = sources, let engine = engine(build, root: root) else { throw BuildError("Nothing to build \(build) from.") }
        let fm = FileManager.default
        try fm.createDirectory(at: native, withIntermediateDirectories: true)
        let work = cache.appendingPathComponent("fast-\(build)")
        try? fm.removeItem(at: work)
        defer { try? fm.removeItem(at: work) }
        let gen = work.appendingPathComponent("gen")
        let name = VoiceCatalog.builds.first { $0.0 == build }?.1 ?? VoiceCatalog.macinTalkEras.first { $0.0 == build }.map { "Mac OS X \($0.1) MacinTalk" } ?? build
        progress("Translating the \(name) engine", nil)
        let glue: URL
        if build.hasPrefix("mt") {
            let dump = work.appendingPathComponent("dump")
            try fm.createDirectory(at: work, withIntermediateDirectories: true)
            try dumpMacinTalk(root.appendingPathComponent(build), to: dump)
            try X86Recompiler.run(dump: dump, profile: src.appendingPathComponent("profiles/\(build).txt"), outdir: gen, prefix: build)
            glue = src.appendingPathComponent("mt_native_glue.c")
        } else {
            try ArmRecompiler.run(voiced: engine, profile: src.appendingPathComponent("profiles/\(build).txt"), outdir: gen,
                                  prefix: build, scanBL: stripped.contains(build))
            glue = src.appendingPathComponent("native_glue.c")
        }
        // biggest first, so no long file is left running alone at the end
        let size = { (u: URL) in (try? fm.attributesOfItem(atPath: u.path)[.size] as? Int) ?? 0 }
        let files = (try fm.contentsOfDirectory(atPath: gen.path).filter { $0.hasSuffix(".c") }.sorted()
            .map { gen.appendingPathComponent($0) } + [glue]).sorted { size($0) > size($1) }
        let objects = work.appendingPathComponent("obj")
        try fm.createDirectory(at: objects, withIntermediateDirectories: true)
        // As ios/project.yml compiles it (fastflags): no SLP vectorizer, NewGVN. -g0:
        // Zig adds debug info otherwise.
        let flags = ["cc", "-target", "\(arch)-macos", "-O2", "-g0", "-fno-slp-vectorize", "-mllvm", "-enable-newgvn",
                     "-std=gnu11", "-ffp-contract=off", "-w", "-fPIC",
                     "-I", src.appendingPathComponent("core").path, "-DCV_BUILD=\(build)"]
        progress("Compiling the \(name) engine", 0)
        // Every core, but at utility priority (still the performance cores):
        // the compiler gives way only when something else needs the CPU, so
        // voices still on the interpreter don't stutter while it builds.
        let jobs = max(1, ProcessInfo.processInfo.activeProcessorCount)
        var running: [(Process, Pipe)] = []
        var done = 0
        var objs: [String] = []
        // the first of the running compiles to finish, whichever it is
        func reapAny() throws {
            while true {
                if let i = running.firstIndex(where: { !$0.0.isRunning }) { return try reap(running.remove(at: i)) }
                Thread.sleep(forTimeInterval: 0.05)
            }
        }
        func reap(_ p: (Process, Pipe)) throws {
            p.0.waitUntilExit()
            if p.0.terminationStatus != 0 {
                let err = String(decoding: p.1.fileHandleForReading.readDataToEndOfFile().suffix(2000), as: UTF8.self)
                running.forEach { $0.0.terminate() }
                throw BuildError("Compiling the \(name) engine failed: \(err)")
            }
            done += 1
            progress(nil, Double(done) / Double(files.count))
        }
        for f in files {
            if running.count >= jobs { try reapAny() }
            let o = objects.appendingPathComponent(f.lastPathComponent + ".o").path
            objs.append(o)
            running.append(try start(zig, flags + ["-c", f.path, "-o", o]))
        }
        while !running.isEmpty { try reapAny() }
        progress("Linking the \(name) engine", nil)
        let lib = library(build, in: native)
        let tmp = lib.appendingPathExtension("new")
        try run(zig, ["cc", "-target", "\(arch)-macos", "-shared", "-o", tmp.path] + objs)
        try? fm.removeItem(at: lib)  // a process that has it loaded keeps its copy
        try fm.moveItem(at: tmp, to: lib)
        try stamp(engine).write(to: lib.appendingPathExtension("stamp"), atomically: true, encoding: .utf8)
    }

    /// MacinTalk's images as its loader leaves them in guest memory, which
    /// recomp_x86.py reads: core/mt/mt_native.c writes them when MT_DUMP is set
    /// as an engine opens. Nothing else in this process opens MacinTalk.
    private static func dumpMacinTalk(_ engineRoot: URL, to dir: URL) throws {
        setenv("MT_DUMP", dir.path, 1)
        defer { unsetenv("MT_DUMP") }
        var err = [CChar](repeating: 0, count: 512)
        if let e = mte_open(engineRoot.path, &err, err.count) { mte_close(e) }
        guard FileManager.default.fileExists(atPath: dir.appendingPathComponent("images.txt").path) else {
            throw BuildError("MacinTalk couldn't be read for its fast code: \(String(cString: err))")
        }
    }

    private static func start(_ exe: URL, _ args: [String]) throws -> (Process, Pipe) {
        let p = Process()
        p.executableURL = exe
        p.arguments = args
        let pipe = Pipe()
        p.standardError = pipe
        p.standardOutput = FileHandle.nullDevice
        var env = ProcessInfo.processInfo.environment
        env["ZIG_GLOBAL_CACHE_DIR"] = cache.appendingPathComponent("zig-cache").path
        env["ZIG_LOCAL_CACHE_DIR"] = cache.appendingPathComponent("zig-cache").path
        p.environment = env
        p.qualityOfService = .utility
        p.terminationHandler = { done in liveLock.lock(); live.remove(done); liveLock.unlock() }
        liveLock.lock(); live.insert(p); liveLock.unlock()
        try p.run()
        return (p, pipe)
    }

    private static func run(_ exe: URL, _ args: [String]) throws {
        let (p, pipe) = try start(exe, args)
        let err = pipe.fileHandleForReading.readDataToEndOfFile()
        p.waitUntilExit()
        if p.terminationStatus != 0 {
            throw BuildError("\(exe.lastPathComponent) failed: \(String(decoding: err.suffix(2000), as: UTF8.self))")
        }
    }
}

/// SHA-256 over a download, a chunk at a time.
struct SHA256Stream {
    private var ctx = CC_SHA256_CTX()
    init() { CC_SHA256_Init(&ctx) }
    mutating func update(_ d: Data) { d.withUnsafeBytes { _ = CC_SHA256_Update(&ctx, $0.baseAddress, CC_LONG($0.count)) } }
    var hex: String {
        var c = ctx
        var out = [UInt8](repeating: 0, count: Int(CC_SHA256_DIGEST_LENGTH))
        CC_SHA256_Final(&out, &c)
        return out.map { String(format: "%02x", $0) }.joined()
    }
}
