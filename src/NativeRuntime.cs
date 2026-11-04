// App-local Microsoft runtime. Payload travels as explicit JSON so Preview 1's
// updater can carry it; only these four pinned redistributable DLLs are accepted.
// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.IO;
using System.Collections.Generic;
using System.Security.Cryptography;
using System.Web.Script.Serialization;

public sealed class RuntimeLibrary
{
    public string name, sha256, data;
}
public sealed class RuntimePayload
{
    public int format;
    public RuntimeLibrary[] files;
}
public static class NativeRuntime
{
    static readonly Dictionary<string, string> Expected = new Dictionary<string, string>(
        StringComparer.Ordinal) {
        { "msvcp140.dll", "0f885b509a685d2bbfa652fed26b5fb31d88fbdab0a978c641d1c7b8aa460aa9" },
        { "msvcp140_atomic_wait.dll",
          "640b2aefced484d0368eea5bdd06addd0658a3a70a49256e560d6923b404a479" },
        { "vcruntime140.dll", "d5e4d9a3e835fa679450145d6a7d94e36573a509317111904d9b3712c30d9066" },
        { "vcruntime140_1.dll",
          "1f2d41c4aa5db0bc33ebf7b66d72943a817d7ce6cbe880502a9403823633093f" }
    };
    static string Hash(byte[] data)
    {
        using (var h = SHA256.Create()) return BitConverter.ToString(h.ComputeHash(data))
            .Replace("-", "")
            .ToLowerInvariant();
    }
    static void NoLinks(string path)
    {
        for (string at = Path.GetFullPath(path); at != null; at = Path.GetDirectoryName(at))
            if ((File.Exists(at) || Directory.Exists(at)) &&
                (File.GetAttributes(at) & FileAttributes.ReparsePoint) != 0)
                throw new InvalidDataException("Linked runtime path.");
    }
    public static bool Ensure(string root)
    {
        string path = Path.Combine(root, "assets", "visual-cpp-runtime.json");
        if (!File.Exists(path))
            return false; // Older portable folders keep working.
        NoLinks(path);
        if (new FileInfo(path).Length > 4 * 1024 * 1024)
            throw new InvalidDataException("Runtime payload too large.");
        var payload = new JavaScriptSerializer { MaxJsonLength = 4 * 1024 * 1024 }
                          .Deserialize<RuntimePayload>(File.ReadAllText(path));
        return Install(root, payload, Expected);
    }
    internal static bool Install(string root, RuntimePayload payload,
                                 Dictionary<string, string> expected)
    {
        NoLinks(root);
        if (payload == null || payload.format != 1 || payload.files == null ||
            payload.files.Length != expected.Count)
            throw new InvalidDataException("Invalid runtime payload.");
        var decoded = new Dictionary<string, byte[]>(StringComparer.Ordinal);
        foreach (var file in payload.files)
        {
            string hash;
            if (file == null || file.name == null || !expected.TryGetValue(file.name, out hash) ||
                file.sha256 != hash || file.data == null || file.data.Length > 2 * 1024 * 1024 ||
                decoded.ContainsKey(file.name))
                throw new InvalidDataException("Unsupported runtime library.");
            byte[] data = Convert.FromBase64String(file.data);
            if (data.Length == 0 || Hash(data) != hash)
                throw new InvalidDataException("Runtime library differs.");
            string target = Path.Combine(root, file.name);
            NoLinks(target);
            if (Directory.Exists(target) ||
                File.Exists(target) && Hash(File.ReadAllBytes(target)) != hash)
                throw new InvalidDataException(
                    "Existing runtime library differs; previous files preserved.");
            decoded.Add(file.name, data);
        }
        var stages = new Dictionary<string, string>();
        var installed = new List<string>();
        try
        {
            foreach (var file in decoded)
            {
                string target = Path.Combine(root, file.Key);
                if (File.Exists(target))
                    continue;
                string temporary = target + ".new-" + Guid.NewGuid().ToString("N");
                stages.Add(target, temporary);
                using (var output = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write,
                                                   FileShare.None))
                    output.Write(file.Value, 0, file.Value.Length);
            }
            foreach (var stage in stages)
            {
                File.Move(stage.Value, stage.Key);
                installed.Add(stage.Key);
            }
            return installed.Count != 0;
        }
        catch
        {
            foreach (string path in installed)
                if (File.Exists(path) &&
                    Hash(File.ReadAllBytes(path)) == expected[Path.GetFileName(path)])
                    File.Delete(path);
            throw;
        }
        finally
        {
            foreach (string path in stages.Values)
                if (File.Exists(path))
                    File.Delete(path);
        }
    }
}
