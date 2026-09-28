using System.Text;

namespace TiSpeech;

/// <summary>RIFF/WAVE writer for the engine's unsigned 8-bit mono samples.</summary>
public static class PcmWaveFile
{
    public static void Write(Stream output, ReadOnlySpan<byte> samples, int sampleRate)
    {
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(sampleRate);
        using var writer = new BinaryWriter(output, Encoding.ASCII, leaveOpen: true);
        uint length = checked((uint)samples.Length);
        writer.Write("RIFF"u8);
        writer.Write(checked(36u + length + (length & 1)));
        writer.Write("WAVEfmt "u8);
        writer.Write(16u);
        writer.Write((ushort)1); // PCM
        writer.Write((ushort)1); // mono
        writer.Write(sampleRate);
        writer.Write(sampleRate); // one byte per sample
        writer.Write((ushort)1);
        writer.Write((ushort)8);
        writer.Write("data"u8);
        writer.Write(length);
        writer.Write(samples);
        if ((length & 1) != 0) writer.Write((byte)0);
    }
}
