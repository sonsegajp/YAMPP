// YAMPP: read the user's verified disc without requesting write/exclusive access.
// The running game keeps the same ISO open while a content mod is prepared.
using System.Buffers.Binary;
using System.Text;
namespace mexLib;
internal static class YamppDiscReader {
    static uint U32(byte[] data, int at) => BinaryPrimitives.ReadUInt32BigEndian(data.AsSpan(at, 4));
    public static void Extract(string path, string sys, string files) {
        using var input = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
        byte[] Read(long offset, int size) {
            if (offset < 0 || size < 0 || offset > input.Length - size)
                throw new InvalidDataException("Truncated GameCube disc");
            input.Position = offset; var bytes = new byte[size];
            int done = 0;
            while (done < size) { int n = input.Read(bytes, done, size - done); if (n == 0) throw new EndOfStreamException(); done += n; }
            return bytes;
        }
        void Copy(long offset, long size, string output) {
            if (offset < 0 || size < 0 || offset > input.Length - size)
                throw new InvalidDataException("Truncated GameCube file");
            Directory.CreateDirectory(Path.GetDirectoryName(output)!);
            using var destination = new FileStream(output, FileMode.CreateNew, FileAccess.Write);
            input.Position = offset; var buffer = new byte[128 * 1024];
            while (size > 0) {
                int read = input.Read(buffer, 0, (int)Math.Min(size, buffer.Length));
                if (read == 0) throw new EndOfStreamException();
                destination.Write(buffer, 0, read); size -= read;
            }
        }
        var boot = Read(0, 0x440);
        if (Encoding.ASCII.GetString(boot, 0, 6) != "GALE01" || boot[7] != 2 || U32(boot, 0x1c) != 0xc2339f3d)
            throw new InvalidDataException("Expected an original Melee 1.02 disc");
        Copy(0, 0x440, Path.Combine(sys, "boot.bin"));
        Copy(0x440, 0x2000, Path.Combine(sys, "bi2.bin"));
        var app = Read(0x2440, 0x20);
        Copy(0x2440, 0x20L + U32(app, 0x14) + U32(app, 0x18), Path.Combine(sys, "apploader.img"));
        uint dolOffset = U32(boot, 0x420); var dol = Read(dolOffset, 0x100);
        long dolSize = 0;
        for (int i = 0; i < 18; ++i) dolSize = Math.Max(dolSize, (long)U32(dol, i * 4) + U32(dol, 0x90 + i * 4));
        if (dolSize <= 0 || dolSize > 32 * 1024 * 1024) throw new InvalidDataException("Invalid DOL size");
        // The pinned m-ex patch hashes the disc DOL including 0x100 alignment.
        dolSize = (dolSize + 0xff) & ~0xffL;
        Copy(dolOffset, dolSize, Path.Combine(sys, "main.dol"));
        uint fstSize = U32(boot, 0x428);
        if (fstSize < 12 || fstSize > 16 * 1024 * 1024) throw new InvalidDataException("Invalid disc filesystem");
        var fst = Read(U32(boot, 0x424), (int)fstSize);
        uint count = U32(fst, 8);
        if (count < 1 || count > fst.Length / 12) throw new InvalidDataException("Invalid filesystem count");
        var stack = new Stack<(uint end, string path)>(); stack.Push((count, files));
        for (uint i = 1; i < count; ++i) {
            while (i >= stack.Peek().end) stack.Pop();
            int entry = (int)i * 12; uint flags = U32(fst, entry), offset = U32(fst, entry + 4), size = U32(fst, entry + 8);
            long nameStart = count * 12L + (flags & 0xffffff);
            if (nameStart >= fst.Length) throw new InvalidDataException("Invalid filename offset");
            int end = Array.IndexOf(fst, (byte)0, (int)nameStart);
            if (end < 0) throw new InvalidDataException("Missing filename terminator");
            string name = Encoding.ASCII.GetString(fst, (int)nameStart, end - (int)nameStart);
            if (name is "" or "." or ".." || name.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0 || name.Contains('/') || name.Contains('\\'))
                throw new InvalidDataException("Invalid disc filename");
            string target = Path.Combine(stack.Peek().path, name);
            if ((flags >> 24) != 0) {
                if (size <= i || size > stack.Peek().end) throw new InvalidDataException("Invalid directory range");
                Directory.CreateDirectory(target); stack.Push((size, target));
            } else Copy(offset, size, target);
        }
    }
}
