using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Security.Cryptography;
using System.Security.Principal;
using System.Text;
using System.Threading;
using System.Windows.Forms;

internal static class PortableLauncher
{
    // Compiled by the Windows packaging script from the verified ZIP snapshot.
    private const string PayloadHash = "@PAYLOAD_HASH@";
    private const string ArchiveRoot = "STPViewer-1.0.0-windows-x64/";

    private static string Hash(Stream stream)
    {
        using (var sha = SHA256.Create()) return BitConverter.ToString(sha.ComputeHash(stream)).Replace("-", "").ToLowerInvariant();
    }

    private static string SafePath(string root, string relative)
    {
        if (String.IsNullOrEmpty(relative) || relative.Contains("\\") || relative.Contains(":") || Path.IsPathRooted(relative))
            throw new InvalidDataException("Invalid embedded path.");
        foreach (var part in relative.Split('/'))
            if (part == ".." || part == "." || part.Length == 0) throw new InvalidDataException("Invalid embedded path component.");
        var result = Path.GetFullPath(Path.Combine(root, relative.Replace('/', Path.DirectorySeparatorChar)));
        if (!result.StartsWith(Path.GetFullPath(root) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("Embedded path escapes the cache.");
        return result;
    }

    private static Dictionary<string, string> Manifest()
    {
        var files = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        using (var reader = new StreamReader(Assembly.GetExecutingAssembly().GetManifestResourceStream("manifest"), Encoding.UTF8))
            while (!reader.EndOfStream) {
                var line = reader.ReadLine();
                if (line.Length < 66 || line[64] != ' ') throw new InvalidDataException("Invalid embedded manifest.");
                files.Add(line.Substring(65), line.Substring(0, 64));
            }
        return files;
    }

    private static void NoLinks(string root, string file)
    {
        for (var path = file; path != null && path.Length >= root.Length; path = Path.GetDirectoryName(path))
            if ((File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0) throw new IOException("Portable cache contains a link.");
    }

    private static bool Valid(string root, Dictionary<string, string> files)
    {
        if (!Directory.Exists(root)) return false;
        try {
            foreach (var entry in files) {
                var path = SafePath(root, entry.Key);
                if (!File.Exists(path)) return false;
                NoLinks(root, path);
                using (var input = File.OpenRead(path)) if (Hash(input) != entry.Value) return false;
            }
            // Check plugin folders too, and reject links before traversing a directory.
            var directories = new Stack<string>(); directories.Push(root);
            while (directories.Count != 0) {
                foreach (var path in Directory.GetFileSystemEntries(directories.Pop(), "*", SearchOption.TopDirectoryOnly)) {
                    NoLinks(root, path);
                    if ((File.GetAttributes(path) & FileAttributes.Directory) != 0) {
                        directories.Push(path); continue;
                    }
                    var extension = Path.GetExtension(path);
                    var relative = path.Substring(root.Length + 1).Replace(Path.DirectorySeparatorChar, '/');
                    if ((extension.Equals(".dll", StringComparison.OrdinalIgnoreCase) || extension.Equals(".exe", StringComparison.OrdinalIgnoreCase))
                        && !files.ContainsKey(relative)) return false;
                }
            }
            return true;
        } catch (IOException) { return false; }
        catch (UnauthorizedAccessException) { return false; }
    }

    private static void Extract(string target, Dictionary<string, string> files)
    {
        var staging = target + ".new-" + Guid.NewGuid().ToString("N");
        Directory.CreateDirectory(staging);
        try {
            using (var payload = Assembly.GetExecutingAssembly().GetManifestResourceStream("payload")) {
                if (Hash(payload) != PayloadHash) throw new InvalidDataException("Embedded package checksum mismatch.");
                payload.Position = 0;
                using (var archive = new ZipArchive(payload, ZipArchiveMode.Read)) {
                    var written = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                    foreach (var entry in archive.Entries) {
                        if (!entry.FullName.StartsWith(ArchiveRoot, StringComparison.Ordinal)) throw new InvalidDataException("Unexpected archive root.");
                        if (entry.FullName.EndsWith("/", StringComparison.Ordinal)) continue;
                        var relative = entry.FullName.Substring(ArchiveRoot.Length);
                        if (!files.ContainsKey(relative) || !written.Add(relative)) throw new InvalidDataException("Unexpected archive entry.");
                        var destination = SafePath(staging, relative);
                        Directory.CreateDirectory(Path.GetDirectoryName(destination));
                        using (var input = entry.Open()) using (var output = File.Create(destination)) input.CopyTo(output);
                    }
                    if (written.Count != files.Count) throw new InvalidDataException("Incomplete portable archive.");
                }
            }
            if (!Valid(staging, files)) throw new InvalidDataException("Extracted package checksum mismatch.");
            // Preserve an altered previous cache for recovery, rather than deleting user files.
            if (Directory.Exists(target)) Directory.Move(target, target + ".previous-" + Guid.NewGuid().ToString("N"));
            Directory.Move(staging, target);
        } finally { if (Directory.Exists(staging)) Directory.Delete(staging, true); }
    }

    private static string Quote(string value)
    {
        var result = new StringBuilder("\""); int slashes = 0;
        foreach (char character in value) {
            if (character == '\\') { ++slashes; continue; }
            result.Append('\\', character == '"' ? slashes * 2 + 1 : slashes);
            slashes = 0; result.Append(character);
        }
        result.Append('\\', slashes * 2); return result.Append('"').ToString();
    }

    [STAThread]
    private static int Main(string[] args)
    {
        bool commandLine = false;
        foreach (var argument in args)
            if (argument.StartsWith("--", StringComparison.Ordinal)) { commandLine = true; break; }
        try {
            var cacheParent = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "STPViewer", "Portable");
            Directory.CreateDirectory(cacheParent); NoLinks(cacheParent, cacheParent);
            var cache = Path.Combine(cacheParent, PayloadHash);
            var files = Manifest();
            var mutexName = "Local\\STPViewerPortable-" + WindowsIdentity.GetCurrent().User.Value + "-" + PayloadHash;
            using (var mutex = new Mutex(false, mutexName)) {
                bool locked = false;
                try {
                    try { locked = mutex.WaitOne(TimeSpan.FromMinutes(3)); } catch (AbandonedMutexException) { locked = true; }
                    if (!locked) throw new IOException("Another instance is preparing this portable version. Please retry.");
                    if (!Valid(cache, files)) Extract(cache, files);
                } finally { if (locked) mutex.ReleaseMutex(); }
            }
            if (args.Length == 2 && args[0] == "--portable-info") {
                File.WriteAllText(Path.GetFullPath(args[1]), cache, new UTF8Encoding(false)); return 0;
            }
            var start = new ProcessStartInfo(SafePath(cache, "STPViewer.exe"));
            start.UseShellExecute = false;
            start.RedirectStandardOutput = commandLine;
            start.RedirectStandardError = commandLine;
            start.CreateNoWindow = commandLine;
            // Preserve the caller's directory so relative STEP paths keep their meaning.
            start.WorkingDirectory = Environment.CurrentDirectory;
            var forwarded = new List<string>(); foreach (var argument in args) forwarded.Add(Quote(argument));
            start.Arguments = String.Join(" ", forwarded.ToArray());
            foreach (var key in new List<string>(start.EnvironmentVariables.Keys.CastStrings()))
                if (key.StartsWith("QT_", StringComparison.OrdinalIgnoreCase) || key.StartsWith("QML_", StringComparison.OrdinalIgnoreCase)
                    || key.StartsWith("CSF_", StringComparison.OrdinalIgnoreCase) || key.Equals("CASROOT", StringComparison.OrdinalIgnoreCase))
                    start.EnvironmentVariables.Remove(key);
            using (var child = Process.Start(start)) {
                if (commandLine) {
                    // A GUI wrapper does not reliably inherit its redirected handles
                    // into a second GUI process. Give the child explicit pipes and
                    // drain both concurrently so command-line modes remain non-modal.
                    using (var stdout = Console.OpenStandardOutput())
                    using (var stderr = Console.OpenStandardError()) {
                        var outputCopy = child.StandardOutput.BaseStream.CopyToAsync(stdout);
                        var errorCopy = child.StandardError.BaseStream.CopyToAsync(stderr);
                        child.WaitForExit();
                        outputCopy.Wait(); errorCopy.Wait();
                        stdout.Flush(); stderr.Flush();
                    }
                } else { child.WaitForExit(); }
                return child.ExitCode;
            }
        } catch (Exception error) {
            if (commandLine) {
                try {
                    using (var stderr = Console.OpenStandardError()) {
                        var message = Encoding.UTF8.GetBytes("Unable to start STP Viewer: " + error.Message + "\r\n");
                        stderr.Write(message, 0, message.Length); stderr.Flush();
                    }
                } catch { }
                return 1;
            }
            MessageBox.Show("Unable to start STP Viewer.\n\n" + error.Message, "STP Viewer", MessageBoxButtons.OK, MessageBoxIcon.Error); return 1;
        }
    }

    private static IEnumerable<string> CastStrings(this System.Collections.ICollection keys)
    {
        foreach (var key in keys) yield return (string)key;
    }
}
