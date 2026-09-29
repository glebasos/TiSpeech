using System.Collections.Immutable;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace TiSpeech;

/// <summary>
/// P/Invoke surface for the from-scratch native reconstruction of the SoftVoice
/// engine — the C ABI declared in <c>TiSpeech/native/include/tispeech/capi.h</c>
/// and built by the <c>tispeech_shared</c> CMake target into
/// <c>libtispeech.dylib</c> / <c>libtispeech.so</c> / <c>tispeech.dll</c>.
///
/// HONESTY CONTRACT (mirrors capi.h): nothing here reports success it did not
/// earn. <see cref="Capabilities"/> is what callers gate on, not the host
/// operating system. English synthesis is available when both base and English
/// data were supplied at build time. Unsupported paths return an error; no
/// system voice or substitute audio is used.
///
/// ABSENCE IS NORMAL. The native library is built from an original TIENG32.DLL
/// that most contributors do not have, so "not loaded" is an expected steady
/// state, not a crash. Every member here is safe to touch with no library
/// present: <see cref="IsAvailable"/> goes false, <see cref="UnavailableReason"/>
/// explains what was looked for and where, and the calls return a status
/// instead of throwing.
/// </summary>
public static partial class TiSpeechNative
{
    /// <summary>
    /// Logical name used by the <c>LibraryImport</c> declarations below. The
    /// resolver registered in <see cref="RegisterResolver"/> turns it into a
    /// real file; it never reaches the OS loader unmodified unless every probe
    /// path missed.
    /// </summary>
    private const string LibraryName = "tispeech";

    /// <summary>Environment override: full path to the library file itself.</summary>
    public const string LibraryPathVariable = "TISPEECH_NATIVE_LIB";

    /// <summary>Environment override: directory containing the library file.</summary>
    public const string LibraryDirectoryVariable = "TISPEECH_NATIVE_DIR";

    // ── Native declarations ───────────────────────────────────────────────────
    // All parameters are blittable (raw pointers, marshalled by hand in the
    // wrappers below) so the generated stubs do no string/array marshalling of
    // their own. That keeps the encoding of `text` under our control: capi.h
    // documents it as UTF-8 restricted to Latin-1.

    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    [LibraryImport(LibraryName, EntryPoint ="tispeech_capabilities")]
    private static partial uint NativeCapabilities();

    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    [LibraryImport(LibraryName, EntryPoint ="tispeech_languages")]
    private static partial uint NativeLanguages();

    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    [LibraryImport(LibraryName, EntryPoint ="tispeech_synthesis_languages")]
    private static partial uint NativeSynthesisLanguages();

    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    [LibraryImport(LibraryName, EntryPoint ="tispeech_build_info")]
    private static partial IntPtr NativeBuildInfo();

    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    [LibraryImport(LibraryName, EntryPoint ="tispeech_text_to_phonemes")]
    private static unsafe partial int NativeTextToPhonemes(uint language, byte* text, byte* output, int outputSize);

    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    [LibraryImport(LibraryName, EntryPoint ="tispeech_synthesize")]
    private static unsafe partial int NativeSynthesize(uint language, byte* phonemes, byte** outSamples, int* outCount, int* outSampleRate);

    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    [LibraryImport(LibraryName, EntryPoint ="tispeech_free_samples")]
    private static unsafe partial void NativeFreeSamples(byte* samples);

    // ── Library resolution ────────────────────────────────────────────────────

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeVoiceOptions
    {
        public int Personality, Pitch, Rate, Voicing, F0Style;
        public int F0Range, F0Perturb, VowelFactor, GlottalSource;

        public NativeVoiceOptions(TiVoiceOptions o)
        {
            Personality = (int)o.Personality; Pitch = o.Pitch; Rate = o.Rate;
            Voicing = o.Voicing; F0Style = o.F0Style; F0Range = o.F0Range;
            F0Perturb = o.F0Perturb; VowelFactor = o.VowelFactor; GlottalSource = o.GlottalSource;
        }
    }

    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    [LibraryImport(LibraryName, EntryPoint = "tispeech_userdict_load")]
    internal static unsafe partial int NativeUserDictLoad(byte* bytes, int size, IntPtr* outDict);

    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    [LibraryImport(LibraryName, EntryPoint = "tispeech_userdict_free")]
    internal static partial void NativeUserDictFree(IntPtr dict);

    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    [LibraryImport(LibraryName, EntryPoint = "tispeech_text_to_phonemes_ex")]
    private static unsafe partial int NativeTextToPhonemesEx(uint language, byte* text, IntPtr dict,
        byte* output, int outputSize);

    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    [LibraryImport(LibraryName, EntryPoint = "tispeech_synthesize_ex")]
    private static unsafe partial int NativeSynthesizeEx(uint language, byte* phonemes,
        NativeVoiceOptions* options, byte** outSamples, int* outCount, int* outSampleRate);

    /// <summary>capi.h <c>tispeech_event</c>.</summary>
    [StructLayout(LayoutKind.Sequential)]
    private struct NativeEvent
    {
        public int Sample, TimeMs;
        public ushort Code, Value;
    }

    /// <summary>capi.h <c>TISPEECH_TEXT_WORD_MARKS</c>.</summary>
    private const uint TextWordMarks = 0x8;

    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    [LibraryImport(LibraryName, EntryPoint = "tispeech_text_to_phonemes_flags")]
    private static unsafe partial int NativeTextToPhonemesFlags(uint language, byte* text, IntPtr dict,
        uint flags, byte* output, int outputSize);

    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    [LibraryImport(LibraryName, EntryPoint = "tispeech_synthesize_events")]
    private static unsafe partial int NativeSynthesizeEvents(uint language, byte* phonemes,
        NativeVoiceOptions* options, uint events, byte** outSamples, int* outCount, int* outSampleRate,
        NativeEvent** outEvents, int* outEventCount);

    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    [LibraryImport(LibraryName, EntryPoint = "tispeech_free_events")]
    private static unsafe partial void NativeFreeEvents(NativeEvent* events);

    private static readonly Lock ProbeLock = new();
    private static ImmutableArray<string> _probedPaths = [];
    private static string? _resolvedPath;

    /// <summary>
    /// Registered as a module initializer so the resolver is in place before any
    /// declaration above can fire, regardless of which type a caller touches
    /// first. <c>SetDllImportResolver</c> throws if called twice for the same
    /// assembly, and a module initializer runs exactly once.
    /// </summary>
    // CA2255 warns that [ModuleInitializer] is meant for application code. This
    // is the documented exception: a DllImport resolver has to be installed
    // before the first P/Invoke in the assembly, and a library cannot rely on
    // its consumers to remember to call an Init() method first. The initializer
    // does one cheap registration and touches nothing else.
#pragma warning disable CA2255
    [ModuleInitializer]
#pragma warning restore CA2255
    internal static void RegisterResolver()
    {
        NativeLibrary.SetDllImportResolver(typeof(TiSpeechNative).Assembly, Resolve);
    }

    /// <summary>
    /// Candidate file names for this platform. The CMake target sets
    /// OUTPUT_NAME to <c>tispeech</c>, so the toolchain decorates it per OS.
    /// </summary>
    private static string[] FileNames()
    {
        // libtispeech.dll first: Windows file names are case-insensitive, so
        // "tispeech.dll" beside the app is the managed TiSpeech.dll itself.
        if (OperatingSystem.IsWindows())  return ["libtispeech.dll", "tispeech.dll"];
        if (OperatingSystem.IsMacOS())    return ["libtispeech.dylib", "tispeech.dylib"];
        return ["libtispeech.so", "tispeech.so"];
    }

    /// <summary>
    /// Directories searched, in order. The app's own folder comes first because
    /// that is where the build integration drops the library; the environment
    /// overrides come before it so a developer can point at a scratch CMake
    /// build tree without reinstalling anything.
    /// </summary>
    private static IEnumerable<string> SearchDirectories()
    {
        // Deduplicated because AppContext.BaseDirectory and the assembly's own
        // folder are usually the same, and a diagnostic listing the same path
        // twice reads like a bug.
        var seen = new HashSet<string>(StringComparer.Ordinal);

        IEnumerable<string> Candidates()
        {
            var fromEnv = Environment.GetEnvironmentVariable(LibraryDirectoryVariable);
            if (!string.IsNullOrWhiteSpace(fromEnv))
                yield return fromEnv;

            var baseDir = AppContext.BaseDirectory;
            if (!string.IsNullOrEmpty(baseDir))
            {
                yield return baseDir;
                yield return Path.Combine(baseDir, "native");
                yield return Path.Combine(baseDir, "runtimes", RuntimeInformation.RuntimeIdentifier, "native");
            }

            // Under some hosts (shadow copy, test runners) the assembly does not
            // sit in AppContext.BaseDirectory, so try its own folder too.
            var assemblyDir = Path.GetDirectoryName(typeof(TiSpeechNative).Assembly.Location);
            if (!string.IsNullOrEmpty(assemblyDir))
            {
                yield return assemblyDir;
                yield return Path.Combine(assemblyDir, "native");
            }
        }

        foreach (var directory in Candidates())
        {
            var normalised = directory.TrimEnd(Path.DirectorySeparatorChar);
            if (seen.Add(normalised))
                yield return normalised;
        }
    }

    private static IntPtr Resolve(string libraryName, Assembly assembly, DllImportSearchPath? searchPath)
    {
        if (!string.Equals(libraryName, LibraryName, StringComparison.Ordinal))
            return IntPtr.Zero;

        var probed = ImmutableArray.CreateBuilder<string>();

        var explicitPath = Environment.GetEnvironmentVariable(LibraryPathVariable);
        if (!string.IsNullOrWhiteSpace(explicitPath))
        {
            probed.Add(explicitPath);
            if (NativeLibrary.TryLoad(explicitPath, out var handle))
                return Remember(explicitPath, probed, handle);
        }

        var names = FileNames();
        foreach (var directory in SearchDirectories())
        {
            foreach (var name in names)
            {
                string candidate;
                try { candidate = Path.Combine(directory, name); }
                catch (ArgumentException) { continue; } // malformed override path
                probed.Add(candidate);
                if (NativeLibrary.TryLoad(candidate, out var handle))
                    return Remember(candidate, probed, handle);
            }
        }

        // Last resort: let the OS loader try its own search path (LD_LIBRARY_PATH,
        // DYLD_LIBRARY_PATH, PATH, /usr/local/lib, ...). Returning IntPtr.Zero
        // instead would hand the runtime a name it has already failed to find.
        foreach (var name in names)
        {
            probed.Add($"{name} (OS library search path)");
            if (NativeLibrary.TryLoad(name, out var handle))
                return Remember(name, probed, handle);
        }

        lock (ProbeLock) { _probedPaths = probed.ToImmutable(); }
        return IntPtr.Zero;
    }

    private static IntPtr Remember(string path, ImmutableArray<string>.Builder probed, IntPtr handle)
    {
        lock (ProbeLock)
        {
            _probedPaths = probed.ToImmutable();
            _resolvedPath = path;
        }
        return handle;
    }

    // ── Cached capability probe ───────────────────────────────────────────────

    private sealed record ProbeResult(
        bool Available,
        string? UnavailableReason,
        string? UnavailableDetail,
        TiEngineCapabilities Capabilities,
        TiLanguageFlags Languages,
        TiLanguageFlags SynthesisLanguages,
        string? BuildInfo,
        string? LibraryPath);

    private static readonly Lazy<ProbeResult> LazyProbe =
        new(Probe, LazyThreadSafetyMode.ExecutionAndPublication);

    private static ProbeResult Probe()
    {
        try
        {
            // The first call is what actually triggers loading; everything after
            // it is a plain function call.
            var caps = (TiEngineCapabilities)NativeCapabilities();
            var langs = (TiLanguageFlags)NativeLanguages();
            var info = Marshal.PtrToStringUTF8(NativeBuildInfo());
            TiLanguageFlags synth;
            try { synth = (TiLanguageFlags)NativeSynthesisLanguages(); }
            catch (EntryPointNotFoundException)
            {
                // Libraries from before the export synthesized English only.
                synth = caps.HasFlag(TiEngineCapabilities.Synthesis) ? TiLanguageFlags.English : 0;
            }
            return new ProbeResult(true, null, null, caps, langs, synth, info, _resolvedPath);
        }
        catch (Exception ex) when (ex is DllNotFoundException
                                      or EntryPointNotFoundException
                                      or BadImageFormatException)
        {
            return new ProbeResult(false, DescribeLoadFailure(ex), DescribeProbedPaths(),
                                   TiEngineCapabilities.None, 0, 0, null, null);
        }
    }

    /// <summary>
    /// The one-paragraph "what happened and what to do" message. Deliberately
    /// short enough to show in the UI; the path list lives in
    /// <see cref="UnavailableDetail"/> instead of being appended here.
    /// </summary>
    private static string DescribeLoadFailure(Exception ex)
    {
        if (ex is EntryPointNotFoundException)
            return $"A TiSpeech native library was loaded from {_resolvedPath ?? "an unknown path"}, but it does " +
                   "not export the entry points capi.h declares. It is probably an older or unrelated build — " +
                   "rebuild the tispeech_shared CMake target.";

        if (ex is BadImageFormatException)
            return $"The TiSpeech native library at {_resolvedPath ?? "the resolved path"} is built for a " +
                   $"different architecture than this process ({RuntimeInformation.ProcessArchitecture}). " +
                   "Rebuild the tispeech_shared CMake target for this architecture.";

        return $"The TiSpeech native library ({FileNames()[0]}) was not found, so the reconstructed engine's " +
               "features are unavailable. This is expected if you have not built it: it comes from the " +
               "tispeech_shared CMake target in TiSpeech/native, and its language data is extracted from an " +
               $"original TIENG32.DLL you supply. Set {LibraryPathVariable} (the library file) or " +
               $"{LibraryDirectoryVariable} (its folder) to point at an existing build.";
    }

    private static string? DescribeProbedPaths()
    {
        ImmutableArray<string> probed;
        lock (ProbeLock) { probed = _probedPaths; }
        return probed.IsDefaultOrEmpty ? null : "Searched: " + string.Join("; ", probed) + ".";
    }

    // ── Public surface ────────────────────────────────────────────────────────

    /// <summary>
    /// True when the native library loaded and exports the capi.h entry points.
    /// False is a normal state — see <see cref="UnavailableReason"/>. This says
    /// nothing about what the library can <em>do</em>; ask
    /// <see cref="Capabilities"/> for that.
    /// </summary>
    public static bool IsAvailable => LazyProbe.Value.Available;

    /// <summary>
    /// Why the library could not be used, or null when it loaded. Written for a
    /// contributor who simply has not built the native side yet, not as an
    /// error, and kept short enough to show in the UI. The full probe trail is
    /// in <see cref="UnavailableDetail"/>.
    /// </summary>
    public static string? UnavailableReason => LazyProbe.Value.UnavailableReason;

    /// <summary>
    /// Every path that was tried while looking for the library, for a tooltip
    /// or a log. Null when the library loaded. Separate from
    /// <see cref="UnavailableReason"/> because it can run to a dozen paths,
    /// which belongs in a diagnostic rather than on screen.
    /// </summary>
    public static string? UnavailableDetail => LazyProbe.Value.UnavailableDetail;

    /// <summary>
    /// What this build of the native library can really do
    /// (<c>tispeech_capabilities()</c>). <see cref="TiEngineCapabilities.None"/>
    /// when the library is missing.
    /// </summary>
    public static TiEngineCapabilities Capabilities => LazyProbe.Value.Capabilities;

    /// <summary>
    /// Languages whose letter-to-sound rule data was compiled into this build
    /// (<c>tispeech_languages()</c>). Zero when the library was built without an
    /// original language DLL, which is a supported configuration: the library
    /// still loads, it just cannot convert text.
    /// </summary>
    public static TiLanguageFlags Languages => LazyProbe.Value.Languages;

    /// <summary>
    /// Languages <see cref="Synthesize"/> accepts (<c>tispeech_synthesis_languages()</c>):
    /// English with TIBASE32 and TIENG32, Spanish with TISPAN32 as well.
    /// </summary>
    public static TiLanguageFlags SynthesisLanguages => LazyProbe.Value.SynthesisLanguages;

    /// <summary>
    /// The library's own one-line description of itself
    /// (<c>tispeech_build_info()</c>), or null when it is not loaded.
    /// </summary>
    public static string? BuildInfo => LazyProbe.Value.BuildInfo;

    /// <summary>Full path the library was actually loaded from, or null.</summary>
    public static string? LibraryPath => LazyProbe.Value.LibraryPath;

    /// <summary>True when <paramref name="language"/>'s rule data is in this build.</summary>
    public static bool SupportsLanguage(TiLanguage language) =>
        language is TiLanguage.English or TiLanguage.Spanish or TiLanguage.German
        && (Languages & (TiLanguageFlags)(uint)language) != 0;

    /// <summary>
    /// Text-to-phoneme conversion on macOS, Linux and Windows. English and
    /// Spanish use the reconstructed normaliser, exception dictionary and
    /// stress rules (Spanish needs TIENG32 as well as TISPAN32 at build time,
    /// otherwise letter-to-sound rules only). A <paramref name="dictionary"/> is consulted first, as
    /// SVTextToPhon does. At most 514 Latin-1 characters per call.
    /// </summary>
    public static TiPhonemeResult TextToPhonemes(TiLanguage language, string text,
        TiUserDictionary? dictionary = null) => TextToPhonemes(language, text, dictionary, wordMarks: false);

    /// <summary>
    /// As <see cref="TextToPhonemes(TiLanguage, string, TiUserDictionary?)"/>; with
    /// <paramref name="wordMarks"/> each word is preceded by <c>@w</c> and its index in
    /// <paramref name="text"/>, which synthesis reports as <see cref="TiSpeechEventKind.Word"/>.
    /// </summary>
    public static TiPhonemeResult TextToPhonemes(TiLanguage language, string text,
        TiUserDictionary? dictionary, bool wordMarks)
    {
        ArgumentNullException.ThrowIfNull(text);

        if (!IsAvailable)
            return TiPhonemeResult.Failure(TiStatus.LibraryUnavailable, UnavailableReason);

        if (!Capabilities.HasFlag(TiEngineCapabilities.TextToPhonemes) || !SupportsLanguage(language))
            return TiPhonemeResult.Failure(TiStatus.NoLanguage,
                DescribeConversionFailure(TiStatus.NoLanguage, language));

        if (text.Contains('\0'))
            return TiPhonemeResult.Failure(TiStatus.BadParam,
                "Text cannot contain embedded NUL characters; the native ABI uses NUL-terminated strings.");

        // capi.h: "NUL-terminated UTF-8 restricted to Latin-1 (the rule tables
        // are 8-bit)". Anything above U+00FF is outside that contract, and
        // passing it would yield confident-looking but wrong phonemes, so refuse
        // instead.
        foreach (var c in text)
        {
            if (c > 'ÿ')
                return TiPhonemeResult.Failure(TiStatus.UnsupportedCharacters,
                    $"The letter-to-sound tables are 8-bit and cannot represent '{c}'. " +
                    "Only Latin-1 text can be converted.");
        }

        if (language is TiLanguage.English or TiLanguage.Spanish && text.Length > 514)
            return TiPhonemeResult.Failure(TiStatus.BadParam,
                $"{language} conversion currently supports at most 514 characters per call. " +
                "Split the text into shorter passages.");

        if (string.IsNullOrWhiteSpace(text))
            return TiPhonemeResult.Success(string.Empty);

        var utf8 = Encoding.UTF8.GetBytes(text);
        var input = new byte[utf8.Length + 1];
        utf8.CopyTo(input, 0);
        input[^1] = 0;

        // The native side emits roughly one to a few phoneme characters per
        // input character. Start generously, then grow on TISPEECH_E_BUFFERFULL;
        // give up after a few rounds because capi.c also returns BUFFERFULL for
        // a single word longer than its internal 4 KiB scratch buffer, which no
        // amount of growth on our side can fix.
        var size = Math.Max(512, (utf8.Length * 8) + 64);
        for (var attempt = 0; attempt < 5; attempt++)
        {
            var output = new byte[size];
            int rc;
            if (wordMarks)
            {
                try
                {
                    rc = dictionary is null ? Convert(IntPtr.Zero) : dictionary.Use(Convert);
                }
                catch (EntryPointNotFoundException)
                {
                    return TiPhonemeResult.Failure(TiStatus.NotImplemented,
                        "Rebuild the native library to enable speech events.");
                }

                int Convert(IntPtr dict)
                {
                    unsafe
                    {
                        fixed (byte* pIn = input)
                        fixed (byte* pOut = output)
                            return NativeTextToPhonemesFlags((uint)language, pIn, dict, TextWordMarks,
                                pOut, output.Length);
                    }
                }
            }
            else if (dictionary is null)
            {
                unsafe
                {
                    fixed (byte* pIn = input)
                    fixed (byte* pOut = output)
                        rc = NativeTextToPhonemes((uint)language, pIn, pOut, output.Length);
                }
            }
            else
            {
                try
                {
                    rc = dictionary.Use(dict =>
                    {
                        unsafe
                        {
                            fixed (byte* pIn = input)
                            fixed (byte* pOut = output)
                                return NativeTextToPhonemesEx((uint)language, pIn, dict, pOut, output.Length);
                        }
                    });
                }
                catch (EntryPointNotFoundException)
                {
                    return TiPhonemeResult.Failure(TiStatus.NotImplemented,
                        "Rebuild the native library to enable user dictionaries.");
                }
            }

            var status = (TiStatus)rc;
            if (status == TiStatus.Ok)
            {
                var terminator = Array.IndexOf(output, (byte)0);
                if (terminator < 0) terminator = output.Length;
                return TiPhonemeResult.Success(Encoding.UTF8.GetString(output, 0, terminator));
            }

            if (status != TiStatus.BufferFull)
                return TiPhonemeResult.Failure(status, DescribeConversionFailure(status, language));

            size *= 4;
        }

        return TiPhonemeResult.Failure(TiStatus.BufferFull,
            "The native converter reported TISPEECH_E_BUFFERFULL even after growing the output buffer; " +
            "a single word is probably longer than the engine's internal limit.");
    }

    private static string DescribeConversionFailure(TiStatus status, TiLanguage language) => status switch
    {
        TiStatus.NoLanguage =>
            $"No {language} letter-to-sound data in this build of the TiSpeech native library " +
            $"(it has: {DescribeLanguages(Languages)}).",
        _ => status.Describe(),
    };

    /// <summary>Comma-separated list of the languages in this build, or "none".</summary>
    public static string DescribeLanguages(TiLanguageFlags languages)
    {
        if (languages == 0) return "none";
        var names = new List<string>();
        if (languages.HasFlag(TiLanguageFlags.English)) names.Add("English");
        if (languages.HasFlag(TiLanguageFlags.Spanish)) names.Add("Spanish");
        if (languages.HasFlag(TiLanguageFlags.German))  names.Add("German");
        return names.Count == 0 ? $"0x{(uint)languages:X}" : string.Join(", ", names);
    }

    /// <summary>
    /// Phoneme-to-PCM synthesis (<c>tispeech_synthesize</c>).
    ///
    /// Uses the original default voice for any language in
    /// <see cref="SynthesisLanguages"/>. Unsupported commands and builds without
    /// synthesis data return <see cref="TiStatus.NotImplemented"/>.
    /// </summary>
    public static TiSynthesisResult Synthesize(TiLanguage language, string phonemes, TiVoiceOptions? options = null)
        => Synthesize(language, phonemes, options, TiSpeechEventMask.None);

    /// <summary>
    /// As <see cref="Synthesize(TiLanguage, string, TiVoiceOptions?)"/>, also collecting the
    /// engine's events into <see cref="TiSynthesisResult.Events"/> when <paramref name="events"/>
    /// is not <see cref="TiSpeechEventMask.None"/>. The audio is the same either way.
    /// </summary>
    public static TiSynthesisResult Synthesize(TiLanguage language, string phonemes, TiVoiceOptions? options,
        TiSpeechEventMask events)
    {
        ArgumentNullException.ThrowIfNull(phonemes);

        if (!IsAvailable)
            return TiSynthesisResult.Failure(TiStatus.LibraryUnavailable);

        if (phonemes.Contains('\0'))
            return TiSynthesisResult.Failure(TiStatus.BadParam, "Phonemes cannot contain embedded NUL characters.");

        var utf8 = Encoding.UTF8.GetBytes(phonemes);
        var input = new byte[utf8.Length + 1];
        utf8.CopyTo(input, 0);
        input[^1] = 0;

        unsafe
        {
            byte* samples = null;
            int count = 0;
            int sampleRate = 0;
            NativeEvent* nativeEvents = null;
            int eventCount = 0;
            int rc;
            fixed (byte* pIn = input)
            {
                try
                {
                    if (events != TiSpeechEventMask.None)
                    {
                        var nativeOptions = options is null ? default : new NativeVoiceOptions(options);
                        rc = NativeSynthesizeEvents((uint)language, pIn, options is null ? null : &nativeOptions,
                            (uint)events & 0xF, &samples, &count, &sampleRate, &nativeEvents, &eventCount);
                    }
                    else if (options is null)
                        rc = NativeSynthesize((uint)language, pIn, &samples, &count, &sampleRate);
                    else
                    {
                        var nativeOptions = new NativeVoiceOptions(options);
                        rc = NativeSynthesizeEx((uint)language, pIn, &nativeOptions, &samples, &count, &sampleRate);
                    }
                }
                catch (EntryPointNotFoundException)
                {
                    return TiSynthesisResult.Failure(TiStatus.NotImplemented,
                        "Rebuild the native library to enable voice controls.");
                }
            }

            var status = (TiStatus)rc;
            var managedEvents = new TiSpeechEvent[eventCount];
            for (var k = 0; k < eventCount; k++)
                managedEvents[k] = new((TiSpeechEventKind)nativeEvents[k].Code, nativeEvents[k].Value,
                    nativeEvents[k].Sample, nativeEvents[k].TimeMs);
            if (nativeEvents is not null) NativeFreeEvents(nativeEvents);
            if (status != TiStatus.Ok || samples is null || count <= 0)
            {
                if (samples is not null) NativeFreeSamples(samples);
                // Deliberately no buffer on failure: a caller that ignores the
                // status must not find something playable here.
                return TiSynthesisResult.Failure(status == TiStatus.Ok ? TiStatus.NotImplemented : status);
            }

            try
            {
                var managed = new byte[count];
                new ReadOnlySpan<byte>(samples, count).CopyTo(managed);
                return new TiSynthesisResult(TiStatus.Ok, managed, sampleRate) { Events = managedEvents };
            }
            finally { NativeFreeSamples(samples); }
        }
    }
    /// <summary>Convert ordinary text and synthesize the resulting phonemes.</summary>
    public static TiSynthesisResult SynthesizeText(TiLanguage language, string text, TiVoiceOptions? options = null,
        TiUserDictionary? dictionary = null) => SynthesizeText(language, text, options, dictionary, TiSpeechEventMask.None);

    /// <summary>
    /// Convert and synthesize, collecting <paramref name="events"/>. With
    /// <see cref="TiSpeechEventMask.Words"/> each word event's value is the word's index in
    /// <paramref name="text"/>.
    /// </summary>
    public static TiSynthesisResult SynthesizeText(TiLanguage language, string text, TiVoiceOptions? options,
        TiUserDictionary? dictionary, TiSpeechEventMask events)
    {
        if (!IsAvailable)
            return TiSynthesisResult.Failure(TiStatus.LibraryUnavailable, UnavailableReason);
        if ((SynthesisLanguages & (TiLanguageFlags)(uint)language) == 0)
            return TiSynthesisResult.Failure(TiStatus.NoLanguage,
                $"Native speech is built for {DescribeLanguages(SynthesisLanguages)} only; " +
                $"{language} phoneme previews may still be available.");
        var phonemes = TextToPhonemes(language, text, dictionary, events.HasFlag(TiSpeechEventMask.Words));
        if (!phonemes.IsSuccess)
            return TiSynthesisResult.Failure(phonemes.Status, phonemes.Message);
        if (string.IsNullOrWhiteSpace(phonemes.Phonemes))
            return TiSynthesisResult.Failure(TiStatus.BadParam, "Enter some text to speak.");
        return Synthesize(language, phonemes.Phonemes, options, events);
    }

}
