// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.IO;
using System.Diagnostics;
using System.Threading;
using System.Windows;
public static class GodzillaUpdater
{
    [STAThread]
    public static int Main(string[] args)
    {
        if (args.Length != 2 || args[0] != "--apply")
            return 2;
        string root = null;
        try
        {
            var job = ReleaseUpdater.Read<UpdateJob>(args[1]);
            root = Path.GetFullPath(job.root);
            try
            {
                using (var parent = Process.GetProcessById(job.parent))
                {
                    if (parent.StartTime.ToUniversalTime().Ticks != job.started ||
                        !String.Equals(parent.MainModule.FileName,
                                       Path.Combine(root, "Godzilla.exe"),
                                       StringComparison.OrdinalIgnoreCase))
                        throw new IOException("Launcher identity changed.");
                    if (!parent.WaitForExit(30000))
                        throw new IOException("Launcher is still open.");
                }
            }
            catch (ArgumentException)
            {
            }
            foreach (string name in new[] { "gzport", "Godzilla" })
                foreach (var process in Process.GetProcessesByName(name))
                    using (process) if (
                        String.Equals(
                            process.MainModule.FileName, Path.Combine(root, name + ".exe"),
                            StringComparison
                                .OrdinalIgnoreCase)) throw new IOException("Application is still " +
                                                                           "running.");
            ReleaseUpdater.Apply(root, job.stage);
            OwnerIcon.Restore(root);
            Process.Start(new ProcessStartInfo(Path.Combine(
                root, "Godzilla.exe")) { WorkingDirectory = root, UseShellExecute = true });
            return 0;
        }
        catch
        {
            MessageBox.Show(
                "Não foi possível concluir a atualização. Seus saves não foram alterados.\n\nThe " +
                    "update could not be completed. Your saves were not changed.",
                "Godzilla", MessageBoxButton.OK, MessageBoxImage.Information);
            return 1;
        }
    }
}
