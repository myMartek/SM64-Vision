import Foundation

/// Just enough of the ZIP format to read a stored archive.
///
/// Not a general reader, and says so: it handles entries written without
/// compression, which is what the texture pack uses because its contents are
/// PNGs and compressing those again buys two and a half per cent. Anything
/// deflated is reported rather than silently skipped - a file quietly missing
/// from a texture pack would show up as one wrong-looking wall somewhere,
/// which is a miserable thing to track down.
///
/// The central directory at the end of the file is what is read, not the local
/// headers: it is the authoritative list, and it is the only part that can be
/// found without walking the whole archive.
enum ZipReader {

    struct Entry {
        let name: String
        let size: UInt64
        /// Where the bytes themselves begin, past the local header.
        let dataOffset: UInt64
        var isDirectory: Bool { name.hasSuffix("/") }
    }

    struct Failure: LocalizedError {
        let text: String
        init(_ text: String) { self.text = text }
        var errorDescription: String? { text }
    }

    static func entries(in url: URL) throws -> [Entry] {
        let handle = try FileHandle(forReadingFrom: url)
        defer { try? handle.close() }

        let length = try handle.seekToEnd()
        guard length > 22 else { throw Failure("The archive is too short to be one.") }

        // The end record is last, but a comment may follow it, so the last
        // 64 KiB are searched backwards for its signature.
        let tailSize = UInt64(min(Int(length), 65536 + 22))
        try handle.seek(toOffset: length - tailSize)
        let tail = try handle.read(upToCount: Int(tailSize)) ?? Data()

        guard let endAt = lastIndex(of: 0x06054b50, in: tail) else {
            throw Failure("No end-of-archive record - is this a ZIP file?")
        }

        let count = Int(read16(tail, endAt + 10))
        let directoryOffset = UInt64(read32(tail, endAt + 16))

        try handle.seek(toOffset: directoryOffset)
        let directory = try handle.read(upToCount: Int(length - directoryOffset)) ?? Data()

        var entries: [Entry] = []
        entries.reserveCapacity(count)

        var at = 0
        while at + 46 <= directory.count, read32(directory, at) == 0x02014b50 {
            let method = read16(directory, at + 10)
            let size = UInt64(read32(directory, at + 24))
            let nameLength = Int(read16(directory, at + 28))
            let extraLength = Int(read16(directory, at + 30))
            let commentLength = Int(read16(directory, at + 32))
            let localOffset = UInt64(read32(directory, at + 42))

            let nameData = directory.subdata(in: (at + 46) ..< (at + 46 + nameLength))
            let name = String(decoding: nameData, as: UTF8.self)

            if method != 0 && size > 0 {
                throw Failure("\(name) is compressed, and this reader only handles stored entries.")
            }

            // The local header repeats the name and extra field, and its extra
            // field is allowed to differ in length from the central one - so
            // where the bytes start can only be learned by reading it.
            try handle.seek(toOffset: localOffset)
            guard let local = try handle.read(upToCount: 30), local.count == 30,
                  read32(local, 0) == 0x04034b50 else {
                throw Failure("\(name) has no proper local header.")
            }
            let localName = Int(read16(local, 26))
            let localExtra = Int(read16(local, 28))

            entries.append(Entry(name: name,
                                 size: size,
                                 dataOffset: localOffset + 30 + UInt64(localName + localExtra)))

            at += 46 + nameLength + extraLength + commentLength
        }

        return entries
    }

    // MARK: - Little-endian reads

    private static func read16(_ d: Data, _ at: Int) -> UInt16 {
        guard at + 2 <= d.count else { return 0 }
        return UInt16(d[d.startIndex + at]) | UInt16(d[d.startIndex + at + 1]) << 8
    }

    private static func read32(_ d: Data, _ at: Int) -> UInt32 {
        guard at + 4 <= d.count else { return 0 }
        var value: UInt32 = 0
        for i in (0 ..< 4).reversed() {
            value = value << 8 | UInt32(d[d.startIndex + at + i])
        }
        return value
    }

    private static func lastIndex(of signature: UInt32, in d: Data) -> Int? {
        guard d.count >= 4 else { return nil }
        var at = d.count - 4
        while at >= 0 {
            if read32(d, at) == signature { return at }
            at -= 1
        }
        return nil
    }
}
