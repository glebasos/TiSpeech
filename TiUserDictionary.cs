using System.Runtime.InteropServices;

namespace TiSpeech;

/// <summary>
/// A SoftVoice user dictionary (the "SVXF" files <c>SVLoadUserDictionary</c>
/// reads), loaded into the native reconstruction. Immutable once loaded and
/// safe to share between threads. The native library does no file I/O, so
/// this class reads the file and hands over its bytes.
/// </summary>
public sealed class TiUserDictionary : IDisposable
{
    private readonly Handle _handle;

    private TiUserDictionary(Handle handle, string? path)
    {
        _handle = handle;
        Path = path;
    }

    /// <summary>The file this dictionary was loaded from, if any.</summary>
    public string? Path { get; }

    /// <summary>Read and parse a dictionary file.</summary>
    /// <exception cref="InvalidDataException">The file is not a valid user dictionary.</exception>
    public static TiUserDictionary Load(string path) => Parse(File.ReadAllBytes(path), path);

    /// <summary>Parse a dictionary file's contents.</summary>
    /// <exception cref="InvalidDataException">The bytes are not a valid user dictionary.</exception>
    public static TiUserDictionary FromBytes(ReadOnlySpan<byte> bytes) => Parse(bytes, null);

    private static unsafe TiUserDictionary Parse(ReadOnlySpan<byte> bytes, string? path)
    {
        if (!TiSpeechNative.IsAvailable)
            throw new InvalidOperationException(TiSpeechNative.UnavailableReason);
        IntPtr dict = IntPtr.Zero;
        int rc;
        try
        {
            fixed (byte* p = bytes)
                rc = TiSpeechNative.NativeUserDictLoad(p, bytes.Length, &dict);
        }
        catch (EntryPointNotFoundException)
        {
            throw new NotSupportedException("Rebuild the native library to enable user dictionaries.");
        }
        var status = (TiStatus)rc;
        if (status == TiStatus.OutOfMemory)
            throw new OutOfMemoryException(status.Describe());
        if (status != TiStatus.Ok || dict == IntPtr.Zero)
            throw new InvalidDataException(path is null ? status.Describe() : $"{path}: {status.Describe()}");
        var handle = new Handle();
        Marshal.InitHandle(handle, dict);
        return new TiUserDictionary(handle, path);
    }

    /// <summary>Run <paramref name="call"/> with the native pointer kept alive for its duration.</summary>
    internal T Use<T>(Func<IntPtr, T> call)
    {
        var added = false;
        try
        {
            _handle.DangerousAddRef(ref added);
            return call(_handle.DangerousGetHandle());
        }
        finally
        {
            if (added) _handle.DangerousRelease();
        }
    }

    public void Dispose() => _handle.Dispose();

    private sealed class Handle() : SafeHandle(IntPtr.Zero, ownsHandle: true)
    {
        public override bool IsInvalid => handle == IntPtr.Zero;

        protected override bool ReleaseHandle()
        {
            TiSpeechNative.NativeUserDictFree(handle);
            return true;
        }
    }
}
