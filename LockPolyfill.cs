#if !NET9_0_OR_GREATER
#pragma warning disable CS9216 // Monitor on the stand-in itself is the point
namespace System.Threading;

// System.Threading.Lock arrived in .NET 9. The net8.0 build (OpenTalkItNet8,
// for macOS 10.15/11) gets this Monitor-backed stand-in; C# 13+ lowers
// `lock (Lock)` to EnterScope()/Dispose() whenever the type has that name.
internal sealed class Lock
{
    public Scope EnterScope()
    {
        Monitor.Enter(this);
        return new Scope(this);
    }

    public readonly ref struct Scope(Lock owner)
    {
        public void Dispose() => Monitor.Exit(owner);
    }
}
#endif
