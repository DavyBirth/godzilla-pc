// SPDX-License-Identifier: GPL-2.0-or-later
// Release transport and transactional installation. Player data is never managed here.
using System;
using System.IO;
using System.IO.Compression;
using System.Net;
using System.Linq;
using System.Collections.Generic;
using System.Security.Cryptography;
using System.Text.RegularExpressions;
using System.Web.Script.Serialization;

public sealed class UpdateFeed
{
    public string repository = "", channel = "preview";
}
public sealed class ReleaseVersion
{
    public string version = "0.1.0.1", display = "0.1.0 Preview 1";
}
public sealed class ReleaseAsset
{
    public string name, browser_download_url, digest;
    public long size;
}
public sealed class RemoteRelease
{
    public string tag_name;
    public bool draft, prerelease;
    public ReleaseAsset[] assets;
}
public sealed class UpdateOffer
{
    public string version, url, sha256;
    public long bytes;
}
public sealed class UpdateFile
{
    public string path, sha256;
    public long bytes;
}
public sealed class UpdateManifest
{
    public string product, version;
    public UpdateFile[] files;
}
public sealed class UpdateJob
{
    public string root, stage;
    public int parent;
    public long started;
}

public static class ReleaseUpdater
{
    public const string Product = "GodzillaWindows",
                        LegacyAssetName = "Godzilla-Windows-update.zip";
    public const long MaxArchive = 512L * 1024 * 1024, MaxExpanded = 1024L * 1024 * 1024;
    static JavaScriptSerializer Json()
    {
        return new JavaScriptSerializer { MaxJsonLength = 4 * 1024 * 1024 };
    }
    public static T Read<T>(string path)
    {
        return Json().Deserialize<T>(File.ReadAllText(path));
    }
    public static void Write(string path, object value)
    {
        File.WriteAllText(path, Json().Serialize(value), new System.Text.UTF8Encoding(false));
    }
    public static ReleaseVersion Current(string root)
    {
        try
        {
            var value = Read<ReleaseVersion>(Path.Combine(root, "version.json"));
            Version.Parse(value.version);
            return value;
        }
        catch
        {
            return new ReleaseVersion();
        }
    }
    public static UpdateFeed Feed(string root)
    {
        var feed = Read<UpdateFeed>(Path.Combine(root, "update-feed.json"));
        if (!Regex.IsMatch(feed.repository ?? "", @"\A[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+\z") ||
            (feed.channel != "preview" && feed.channel != "stable"))
            throw new InvalidDataException("Update channel is not configured.");
        return feed;
    }
    sealed class Client : WebClient
    {
        protected override WebRequest GetWebRequest(Uri address)
        {
            if (address.Scheme != "https")
                throw new InvalidDataException("HTTPS required.");
            var request = base.GetWebRequest(address);
            request.Timeout = 15000;
            var http = request as HttpWebRequest;
            if (http != null)
                http.ReadWriteTimeout = 15000;
            return request;
        }
    }
    static Client Network()
    {
        ServicePointManager.SecurityProtocol |= SecurityProtocolType.Tls12;
        var client = new Client();
        client.Headers["User-Agent"] = "GodzillaWindows/0.1";
        client.Headers["Accept"] = "application/vnd.github+json";
        return client;
    }
    public static UpdateOffer Check(string root)
    {
        var feed = Feed(root);
        string data;
        using (var client = Network()) data = client.DownloadString(
            "https://api.github.com/repos/" + feed.repository + "/releases?per_page=20");
        var releases = Json().Deserialize<RemoteRelease[]>(data);
        var current = Version.Parse(Current(root).version);
        UpdateOffer best = null;
        foreach (var release in releases ?? new RemoteRelease[0])
        {
            Version version;
            if (release.draft || (release.prerelease && feed.channel == "stable") ||
                !Version.TryParse((release.tag_name ?? "").TrimStart('v'), out version) ||
                version <= current)
                continue;
            var asset =
                (release.assets ?? new ReleaseAsset[0])
                    .FirstOrDefault(x => Regex.IsMatch(x.name ?? "",
                                                       @"\AGodzilla-Windows(-[0-9.]+)?\.zip\z")) ??
                (release.assets ?? new ReleaseAsset[0])
                    .FirstOrDefault(
                        x =>
                            x.name ==
                            LegacyAssetName); // e.g. Godzilla-Windows-0.1.0.2.zip; Preview 1 knows only the legacy name.
            if (asset == null || asset.size <= 0 || asset.size > MaxArchive ||
                !Regex.IsMatch(asset.digest ?? "", @"\Asha256:[0-9a-fA-F]{64}\z"))
                continue;
            var uri = new Uri(asset.browser_download_url);
            if (uri.Scheme != "https" || uri.Host != "github.com" ||
                !uri.AbsolutePath.StartsWith("/" + feed.repository + "/releases/download/",
                                             StringComparison.Ordinal))
                continue;
            if (best == null || version > Version.Parse(best.version))
                best = new UpdateOffer { version = version.ToString(), url = uri.AbsoluteUri,
                                         sha256 = asset.digest.Substring(7).ToLowerInvariant(),
                                         bytes = asset.size };
        }
        return best;
    }
    public static string Hash(string path)
    {
        using (var hash = SHA256.Create()) using (
            var file = File.OpenRead(path)) return BitConverter.ToString(hash.ComputeHash(file))
            .Replace("-", "")
            .ToLowerInvariant();
    }
    public static bool Managed(string relative)
    {
        string path = relative.Replace('\\', '/');
        if (path.StartsWith("/", StringComparison.Ordinal) || path.Length > 220)
            return false;
        foreach (string part in path.Split('/'))
            if (part.Length == 0 || part == "." || part == ".." || part.EndsWith(".") ||
                part.EndsWith(" ") || part.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0 ||
                Regex.IsMatch(part, @"\A(CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9])(?:\.|$)",
                              RegexOptions.IgnoreCase))
                return false;
        string[] fixedFiles = { "Godzilla.exe", "gzport.exe",  "Godzilla.Update.exe",
                                "version.json", "SOURCE.zip",  "CODIGO-FONTE.zip",
                                "README.txt",   "LEIA-ME.txt", "THIRD-PARTY-NOTICES.txt" };
        if (fixedFiles.Contains(path, StringComparer.OrdinalIgnoreCase))
            return true;
        string[] roots = { "assets/", "fonts/", "licenses/" };
        string[] extensions = { ".png", ".jpg", ".jpeg", ".ico", ".svg",  ".ttf",
                                ".ttc", ".otf", ".txt",  ".md",  ".json", ".xaml" };
        return roots.Any(x => path.StartsWith(x, StringComparison.OrdinalIgnoreCase)) &&
               extensions.Contains(Path.GetExtension(path), StringComparer.OrdinalIgnoreCase);
    }
    static void NoLinks(string full)
    {
        for (string at = full; at != null; at = Path.GetDirectoryName(at))
            if ((File.Exists(at) || Directory.Exists(at)) &&
                (File.GetAttributes(at) & FileAttributes.ReparsePoint) != 0)
                throw new InvalidDataException("Links are not update targets.");
    }
    static string Within(string root, string relative)
    {
        if (!Managed(relative))
            throw new InvalidDataException("Unmanaged update path.");
        string full = Path.GetFullPath(Path.Combine(root, relative));
        if (!full.StartsWith(Path.GetFullPath(root).TrimEnd('\\') + "\\",
                             StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("Path escapes installation.");
        NoLinks(full);
        return full;
    }
    public static UpdateManifest Validate(string stage)
    {
        var manifest = Read<UpdateManifest>(Path.Combine(stage, "update-manifest.json"));
        Version version;
        if (manifest.product != Product || !Version.TryParse(manifest.version, out version) ||
            manifest.files == null || manifest.files.Length == 0 || manifest.files.Length > 2048)
            throw new InvalidDataException("Invalid update manifest.");
        var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        long expanded = 0;
        foreach (var entry in manifest.files)
        {
            if (entry == null || !Managed(entry.path) ||
                !names.Add(entry.path.Replace('\\', '/')) || entry.bytes < 0 ||
                entry.bytes > MaxArchive ||
                !Regex.IsMatch(entry.sha256 ?? "", @"\A[0-9a-fA-F]{64}\z"))
                throw new InvalidDataException("Invalid update file.");
            string path = Within(stage, entry.path);
            expanded = checked(expanded + entry.bytes);
            if (expanded > MaxExpanded || new FileInfo(path).Length != entry.bytes ||
                !Hash(path).Equals(entry.sha256, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("Update file verification failed.");
        }
        if (!names.Contains("Godzilla.exe") || !names.Contains("Godzilla.Update.exe") ||
            !names.Contains("gzport.exe") || !names.Contains("version.json"))
            throw new InvalidDataException("Incomplete application update.");
        if (Read<ReleaseVersion>(Path.Combine(stage, "version.json")).version != manifest.version)
            throw new InvalidDataException("Version mismatch.");
        return manifest;
    }
    public static void Extract(string archive, string stage)
    {
        if (Directory.Exists(stage))
            throw new IOException("Staging folder already exists.");
        Directory.CreateDirectory(stage);
        long total = 0;
        var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        using (var zip = ZipFile.OpenRead(archive))
        {
            if (zip.Entries.Count > 2049)
                throw new InvalidDataException("Too many update entries.");
            foreach (var entry in zip.Entries)
            {
                string name = entry.FullName.Replace('\\', '/');
                if (name.EndsWith("/"))
                    continue;
                if (!names.Add(name) || ((entry.ExternalAttributes >> 16) & 0xf000) == 0xa000 ||
                    entry.Length > MaxArchive ||
                    (name == "update-manifest.json" && entry.Length > 4 * 1024 * 1024) ||
                    (name != "update-manifest.json" && !Managed(name)))
                    throw new InvalidDataException("Invalid archive entry.");
                total = checked(total + entry.Length);
                if (total > MaxExpanded)
                    throw new InvalidDataException("Update too large.");
                string path = name == "update-manifest.json" ? Path.Combine(stage, name)
                                                             : Within(stage, name);
                Directory.CreateDirectory(Path.GetDirectoryName(path));
                entry.ExtractToFile(path, false);
            }
        }
        Validate(stage);
    }
    public static string Download(UpdateOffer offer)
    {
        string folder =
            Path.Combine(Path.GetTempPath(), "GodzillaUpdate-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(folder);
        string zip = Path.Combine(folder, "update.zip");
        using (var client = Network()) using (var input = client.OpenRead(offer.url)) using (
            var output = File.Create(zip))
        {
            var buffer = new byte[65536];
            long total = 0;
            int count;
            while ((count = input.Read(buffer, 0, buffer.Length)) > 0)
            {
                total = checked(total + count);
                if (total > offer.bytes || total > MaxArchive)
                    throw new InvalidDataException("Download exceeds release size.");
                output.Write(buffer, 0, count);
            }
        }
        if (new FileInfo(zip).Length != offer.bytes ||
            !Hash(zip).Equals(offer.sha256, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("Download verification failed.");
        string stage = Path.Combine(folder, "stage");
        Extract(zip, stage);
        if (Validate(stage).version != offer.version)
            throw new InvalidDataException("Release version mismatch.");
        return stage;
    }
    public static string Apply(string root, string stage)
    {
        root = Path.GetFullPath(root);
        stage = Path.GetFullPath(stage);
        var manifest = Validate(stage);
        if (Version.Parse(manifest.version) <= Version.Parse(Current(root).version))
            throw new InvalidDataException("Update is not newer.");
        string backup = Path.Combine(root, ".updates", "backup-" + Guid.NewGuid().ToString("N"));
        NoLinks(backup);
        var existed = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var written = new List<UpdateFile>();
        foreach (var entry in manifest.files)
        {
            string target =
                Within(root, entry.path); // Preflight every destination before changing any.
            if (File.Exists(target))
            {
                string saved = Path.Combine(backup, entry.path);
                Directory.CreateDirectory(Path.GetDirectoryName(saved));
                File.Copy(target, saved, false);
                if (Hash(saved) != Hash(target))
                    throw new IOException("Backup verification failed.");
                existed.Add(entry.path);
            }
        }
        try
        {
            foreach (var entry in manifest.files)
            {
                string target = Within(root, entry.path),
                       temp = target + ".update-" + Guid.NewGuid().ToString("N");
                Directory.CreateDirectory(Path.GetDirectoryName(target));
                try
                {
                    File.Copy(Within(stage, entry.path), temp, false);
                    if (!Hash(temp).Equals(entry.sha256, StringComparison.OrdinalIgnoreCase))
                        throw new IOException("Staged file changed.");
                    if (File.Exists(target))
                        File.Replace(temp, target, null);
                    else
                        File.Move(temp, target);
                    written.Add(entry);
                    if (!Hash(target).Equals(entry.sha256, StringComparison.OrdinalIgnoreCase))
                        throw new IOException("Installed file differs.");
                }
                finally
                {
                    if (File.Exists(temp))
                        File.Delete(temp);
                }
            }
        }
        catch
        {
            foreach (var entry in written.AsEnumerable().Reverse())
            {
                string target = Within(root, entry.path);
                if (existed.Contains(entry.path))
                    File.Copy(Path.Combine(backup, entry.path), target, true);
                else
                    File.Delete(target);
            }
            throw;
        }
        return backup;
    }
}
