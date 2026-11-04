// SPDX-License-Identifier: GPL-2.0-or-later
// Re-embeds the player's own game icon (sce_sys/icon0.png) into installed executables.
// Published builds carry only the fallback icon; official artwork never ships.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;
public static class OwnerIcon
{
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr BeginUpdateResource(string file, bool deleteExisting);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool UpdateResource(IntPtr handle, IntPtr type, IntPtr name, ushort language,
                                      byte[] data, uint size);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool EndUpdateResource(IntPtr handle, bool discard);
    static readonly int[] Sizes = { 16, 32, 48, 64, 128, 256 };

    public static string Source(string root)
    {
        return Path.Combine(root, "game", "CUSA02046", "sce_sys", "icon0.png");
    }

    static List<byte[]> Frames(string source)
    {
        var frames = new List<byte[]>();
        using (var original = Image.FromFile(source)) foreach (int size in Sizes) using (
            var bitmap = new Bitmap(
                size, size, PixelFormat.Format32bppArgb)) using (var g = Graphics.FromImage(bitmap))
        {
            g.Clear(Color.Transparent);
            g.InterpolationMode = InterpolationMode.HighQualityBicubic;
            g.PixelOffsetMode = PixelOffsetMode.HighQuality;
            double factor = Math.Min((double)size / original.Width, (double)size / original.Height);
            int w = (int)Math.Round(original.Width * factor),
                h = (int)Math.Round(original.Height * factor);
            g.DrawImage(original, (size - w) / 2, (size - h) / 2, w, h);
            using (var stream = new MemoryStream())
            {
                bitmap.Save(stream, ImageFormat.Png);
                frames.Add(stream.ToArray());
            }
        }
        return frames;
    }

    public static void Embed(string executable, List<byte[]> frames)
    {
        var handle = BeginUpdateResource(executable, false);
        if (handle == IntPtr.Zero)
            throw new IOException("Cannot open executable resources.");
        bool done = false;
        try
        {
            var group = new MemoryStream();
            var writer = new BinaryWriter(group);
            writer.Write((ushort)0);
            writer.Write((ushort)1);
            writer.Write((ushort)frames.Count);
            for (int i = 0; i < frames.Count; ++i)
            {
                ushort id = (ushort)(101 + i);
                if (!UpdateResource(handle, (IntPtr)3, (IntPtr)id, 0, frames[i],
                                    (uint)frames[i].Length))
                    throw new IOException("Icon frame update failed.");
                writer.Write((byte)(Sizes[i] % 256));
                writer.Write((byte)(Sizes[i] % 256));
                writer.Write((byte)0);
                writer.Write((byte)0);
                writer.Write((ushort)1);
                writer.Write((ushort)32);
                writer.Write((uint)frames[i].Length);
                writer.Write(id);
            }
            writer.Flush();
            var bytes = group.ToArray();
            // 32512 is the compiler's fallback group; overwrite it so Explorer never picks the old icon.
            foreach (int groupId in new[] { 1, 32512 })
                if (!UpdateResource(handle, (IntPtr)14, (IntPtr)groupId, 0, bytes,
                                    (uint)bytes.Length))
                    throw new IOException("Icon group update failed.");
            if (!EndUpdateResource(handle, false))
                throw new IOException("Icon commit failed.");
            done = true;
        }
        finally
        {
            if (!done)
                EndUpdateResource(handle, true);
        }
    }

    // Best effort: a missing or unreadable game icon leaves the fallback icon in place.
    public static void Restore(string root)
    {
        try
        {
            string source = Source(root);
            if (!File.Exists(source))
                return;
            var frames = Frames(source);
            foreach (string name in new[] { "Godzilla.exe", "Godzilla.Update.exe", "gzport.exe" })
            {
                string path = Path.Combine(root, name);
                if (File.Exists(path))
                    try
                    {
                        Embed(path, frames);
                    }
                    catch
                    {
                    }
            }
        }
        catch
        {
        }
    }
}
