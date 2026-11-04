// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.IO;
using System.Security.Cryptography;
using System.Web.Script.Serialization;

public sealed class ArtworkWrite
{
    public int offset, repeat = 1;
    public string data;
}
public sealed class ArtworkPatch
{
    public int format, inputBytes, outputBytes;
    public string inputSha256, outputSha256;
    public int identityOffset;
    public string identitySha256;
    public ArtworkWrite[] writes;
    public ArtworkPatch[] alternatives;
}
public static class PromptArtworkUpdate
{
    const int Limit = 64 * 1024 * 1024;
    static string Hash(byte[] value)
    {
        using (var h = SHA256.Create()) return BitConverter.ToString(h.ComputeHash(value))
            .Replace("-", "")
            .ToLowerInvariant();
    }
    static void NoLinks(string path)
    {
        for (string at = Path.GetFullPath(path); at != null; at = Path.GetDirectoryName(at))
            if ((File.Exists(at) || Directory.Exists(at)) &&
                (File.GetAttributes(at) & FileAttributes.ReparsePoint) != 0)
                throw new InvalidDataException("Linked artwork path.");
    }
    static uint U(byte[] data, int at)
    {
        return BitConverter.ToUInt32(data, at);
    }
    static byte[] Identity(string game, string expected)
    {
        byte[] packed = null;
        using (var f = File.OpenRead(game)) using (var r = new BinaryReader(f))
        {
            byte[] head = r.ReadBytes(32);
            if (head.Length != 32 || U(head, 0) != 0x646461 || U(head, 4) != 4 ||
                U(head, 12) != 32 || U(head, 20) != f.Length || U(head, 16) > 100000)
                throw new InvalidDataException("Unsupported game archive.");
            uint table = U(head, 8), count = U(head, 16);
            if ((long)table + 32L * count > f.Length)
                throw new InvalidDataException("Archive table outside file.");
            for (uint i = 0; i < count; i++)
            {
                f.Position = table + 32L * i;
                byte[] entry = r.ReadBytes(32);
                uint offset = U(entry, 0), size = U(entry, 4), name = U(entry, 16);
                if ((long)offset + size > f.Length || name >= offset || size > 1024 * 1024)
                    continue;
                f.Position = name;
                var text = new System.Text.StringBuilder();
                for (int n = 0; n < 256 && f.Position < offset; n++)
                {
                    byte c = r.ReadByte();
                    if (c == 0)
                        break;
                    text.Append((char)c);
                }
                if (text.ToString() != "buttonicon_grow.dds")
                    continue;
                f.Position = offset;
                packed = r.ReadBytes((int)size);
                break;
            }
        }
        if (packed == null || packed.Length < 32 || U(packed, 0) != 0x535a4c || packed[6] != 8)
            throw new InvalidDataException("Missing native artwork identity.");
        int command = (int)U(packed, 8), literal = (int)U(packed, 12), sizeOut = (int)U(packed, 16),
            stored = (int)U(packed, 20), bits = packed[5], threshold = packed[25];
        if (bits < 1 || bits > 15 || threshold < 1 || command < 32 || literal < command ||
            stored != packed.Length || literal > stored || sizeOut < 256 * 256 * 4 ||
            sizeOut > 1024 * 1024)
            throw new InvalidDataException("Invalid compressed artwork.");
        byte[] plain = new byte[sizeOut];
        int end = literal, pos = 0, mask = (1 << bits) - 1;
        while (pos < plain.Length)
        {
            if (command + 2 > end)
                throw new InvalidDataException("Truncated artwork flags.");
            int flags = BitConverter.ToUInt16(packed, command);
            command += 2;
            for (int bit = 0; bit < 16 && pos < plain.Length; bit++)
            {
                if ((flags & (1 << bit)) != 0)
                {
                    if (literal >= stored)
                        throw new InvalidDataException("Truncated artwork literal.");
                    plain[pos++] = packed[literal++];
                }
                else
                {
                    if (command + 2 > end)
                        throw new InvalidDataException("Truncated artwork reference.");
                    int word = BitConverter.ToUInt16(packed, command);
                    command += 2;
                    int length = (word & mask) + threshold, distance = word >> bits;
                    if (distance < 1 || distance > pos || length > plain.Length - pos)
                        throw new InvalidDataException("Invalid artwork reference.");
                    for (int n = 0; n < length; n++)
                    {
                        plain[pos] = plain[pos - distance];
                        pos++;
                    }
                }
            }
        }
        if (command != end || literal != stored || Hash(plain) != expected)
            throw new InvalidDataException("Native artwork identity changed.");
        byte[] result = new byte[256 * 256 * 4];
        int start = plain.Length - result.Length;
        for (int y = 0; y < 256; y++)
            for (int x = 0; x < 256; x++)
            {
                int pixel = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) |
                            ((x & 4) << 2) | ((y & 4) << 3);
                int at = start + (((y / 8) * 32 + x / 8) * 64 + pixel) * 4;
                Buffer.BlockCopy(plain, at, result, (y * 256 + x) * 4, 4);
            }
        return result;
    }
    public static bool Apply(string root, string user, string game)
    {
        string spec = Path.Combine(root, "assets", "tutorial-symbols-patch.json");
        if (!File.Exists(spec))
            return false;
        NoLinks(spec);
        if (new FileInfo(spec).Length > 4 * 1024 * 1024)
            throw new InvalidDataException("Artwork patch too large.");
        var patch =
            new JavaScriptSerializer { MaxJsonLength = 4 * 1024 * 1024 }.Deserialize<ArtworkPatch>(
                File.ReadAllText(spec));
        if (patch == null)
            throw new InvalidDataException("Invalid artwork patch.");
        string target = Path.Combine(user, "patches", "pc-prompts.bin");
        NoLinks(target);
        if (new FileInfo(target).Length > Limit)
            throw new InvalidDataException("Artwork source too large.");
        byte[] before = File.ReadAllBytes(target);
        string current = Hash(before);
        if (patch.format == 2)
        {
            if (patch.alternatives == null || patch.alternatives.Length < 1 ||
                patch.alternatives.Length > 8)
                throw new InvalidDataException("Invalid artwork alternatives.");
            ArtworkPatch selected = null;
            foreach (var choice in patch.alternatives)
            {
                if (choice == null || choice.format != 1)
                    throw new InvalidDataException("Invalid artwork alternative.");
                if (current == choice.outputSha256)
                    return false;
                if (current == choice.inputSha256)
                    selected = choice;
            }
            if (selected == null)
                throw new InvalidDataException("Unsupported artwork source.");
            patch = selected;
        }
        if (patch.format != 1 || patch.inputBytes <= 0 || patch.inputBytes > Limit ||
            patch.outputBytes < patch.inputBytes || patch.outputBytes > Limit ||
            patch.writes == null || patch.writes.Length > 50000)
            throw new InvalidDataException("Invalid artwork patch.");
        if (current == patch.outputSha256)
            return false;
        if (before.Length != patch.inputBytes || current != patch.inputSha256)
            throw new InvalidDataException("Artwork source differs from the supported version.");
        byte[] after = new byte[patch.outputBytes];
        Buffer.BlockCopy(before, 0, after, 0, before.Length);
        long end = 0;
        foreach (var write in patch.writes)
        {
            if (write == null || write.offset < end || write.repeat < 1)
                throw new InvalidDataException("Invalid artwork write.");
            byte[] data = Convert.FromBase64String(write.data ?? "");
            long count = (long)data.Length * write.repeat;
            if (data.Length == 0 || count > Limit || (long)write.offset + count > after.Length)
                throw new InvalidDataException("Artwork write outside file.");
            for (int i = 0; i < write.repeat; i++)
                Buffer.BlockCopy(data, 0, after, write.offset + i * data.Length, data.Length);
            end = (long)write.offset + count;
        }
        if (patch.identityOffset != 0)
        {
            if (patch.identityOffset < end ||
                (long)patch.identityOffset + 256 * 256 * 4 > after.Length)
                throw new InvalidDataException("Invalid identity destination.");
            byte[] identity = Identity(game, patch.identitySha256);
            Buffer.BlockCopy(identity, 0, after, patch.identityOffset, identity.Length);
        }
        if (Hash(after) != patch.outputSha256)
            throw new InvalidDataException("Artwork result verification failed.");
        string temporary = target + ".update-" + Guid.NewGuid().ToString("N");
        string backup = target + ".before-" + patch.inputSha256;
        NoLinks(backup);
        try
        {
            using (var file = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write,
                                             FileShare.None))
            {
                file.Write(after, 0, after.Length);
                file.Flush(true);
            }
            // Open without sharing writes or deletion to verify the source again.
            using (var file =
                       new FileStream(target, FileMode.Open, FileAccess.Read, FileShare.Read))
            {
                if (file.Length != before.Length)
                    throw new IOException("Artwork changed during update.");
                byte[] now = new byte[before.Length];
                int at = 0, n;
                while (at < now.Length && (n = file.Read(now, at, now.Length - at)) > 0)
                    at += n;
                if (at != now.Length || Hash(now) != patch.inputSha256)
                    throw new IOException("Artwork changed during update.");
                if (!File.Exists(backup))
                    File.WriteAllBytes(backup, now);
                else if (Hash(File.ReadAllBytes(backup)) != patch.inputSha256)
                    throw new IOException("Artwork backup differs.");
            }
            File.Replace(temporary, target, null);
            return true;
        }
        finally
        {
            if (File.Exists(temporary))
                File.Delete(temporary);
        }
    }
}
