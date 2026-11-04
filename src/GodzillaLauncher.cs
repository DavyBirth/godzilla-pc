// SPDX-License-Identifier: GPL-2.0-or-later
// Windows presentation layer. Reads the player's data and never changes the game.
using System;
using System.IO;
using System.Globalization;
using System.Text;
using System.Linq;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using System.Web.Script.Serialization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Markup;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Automation;

public class Preferences
{
    public string InterfaceLanguage = "auto";
    public int GameLanguage = -1;
    public int Width = 1920, Height = 1080, Mode = 1;
    public int Present = 0, Upscale = 0, Sharpen = 1, Sharpness = 250, Gamma = 100;
    public int MouseSensitivity = 180;
}
public class Installation
{
    public string Root, Work, User;
    public Installation(string root)
    {
        Root = Path.GetFullPath(root);
        string configured = Environment.GetEnvironmentVariable("GZ_WORK");
        string local = Path.Combine(Root, "game");
        Work = !String.IsNullOrWhiteSpace(configured) ? Path.GetFullPath(configured) : local;
        User = String.IsNullOrWhiteSpace(configured) ? Path.Combine(Root, "userdata")
                                                     : Path.Combine(Work, "user");
    }
    public string GameFile(string relative)
    {
        return Path.Combine(Work, "CUSA02046", relative);
    }
    public bool Ready
    {
        get {
            return File.Exists(Path.Combine(Root, "gzport.exe")) &&
                   File.Exists(Path.Combine(Work, "out", "boot-linked.bin"));
        }
    }
}
public class GodzillaLauncher : Window
{
    readonly string root, config;
    readonly Installation install;
    Preferences prefs;
    Grid body;
    Grid mainGrid, controlsPanel;
    FrameworkElement heroPanel;
    StackPanel content;
    TextBlock status;
    Button play;
    ComboBox interfaceLanguage;
    bool running, rendering, updating;
    UpdateOffer updateOffer;
    string updateMessage = "";
    int page;
    readonly List<Control> options = new List<Control>();
    string lastLog;
    static readonly int[] sharpValues = { 1000, 500, 250, 0 };
    static readonly int[] gammaValues = { 80, 90, 100, 110, 120 };
    static readonly int[] mouseValues = { 90, 135, 180, 270, 360 };
    static readonly int[] languageValues = { -1, 17, 1, 2, 5, 4, 3, 20, 0 };
    static Brush Brush(string hex)
    {
        return (Brush) new BrushConverter().ConvertFromString(hex);
    }
    string T(string pt, string en)
    {
        return LauncherLanguages.Text(
            LauncherLanguages.Resolve(prefs.InterfaceLanguage, CultureInfo.CurrentUICulture.Name),
            en, pt);
    }
    List<int> AvailableGameLanguages()
    {
        var result = new List<int> { -1 };
        for (int i = 0; i < LauncherLanguages.GameIds.Length; ++i)
        {
            string suffix = LauncherLanguages.Suffixes[i];
            if (LauncherLanguages.GameIds[i] == 1 ||
                File.Exists(
                    install.GameFile("mn_main" + (suffix.Length > 0 ? "_" + suffix : "") + ".pac")))
                result.Add(LauncherLanguages.GameIds[i]);
        }
        return result;
    }
    int EffectiveGameLanguage()
    {
        int selected = prefs.GameLanguage < 0
                           ? LauncherLanguages.SystemGameId(CultureInfo.CurrentUICulture.Name)
                           : prefs.GameLanguage;
        return AvailableGameLanguages().Contains(selected) ? selected : 1;
    }
    public GodzillaLauncher(string folder)
    {
        root = folder;
        config = Path.Combine(root, "launcher.json");
        install = new Installation(root);
        prefs = Load();
        Title = "Godzilla";
        var area = SystemParameters.WorkArea;
        Width = Math.Min(1120, Math.Max(1, area.Width - 48));
        Height = Math.Min(760, Math.Max(1, area.Height - 48));
        MinWidth = Math.Min(640, Width);
        MinHeight = Math.Min(480, Height);
        Left = area.Left + (area.Width - Width) / 2;
        Top = area.Top + (area.Height - Height) / 2;
        WindowStartupLocation = WindowStartupLocation.Manual;
        SizeChanged += delegate
        {
            UpdateResponsiveLayout();
        };
        WindowStyle = WindowStyle.None;
        ResizeMode = ResizeMode.CanResizeWithGrip;
        Background = Brush("#101115");
        Foreground = Brush("#F1F0ED");
        FontFamily = new FontFamily("Segoe UI");
        try
        {
            Icon = BitmapFrame.Create(new Uri(install.GameFile("sce_sys/icon0.png")));
        }
        catch
        {
        }
        Resources = (ResourceDictionary)XamlReader.Parse(Styles);
        Closing += delegate(object sender, System.ComponentModel.CancelEventArgs e)
        {
            if (running || updating)
            {
                e.Cancel = true;
                WindowState = WindowState.Minimized;
            }
            else
                Save();
        };
        Render();
    }
    void UpdateResponsiveLayout()
    {
        if (mainGrid == null || heroPanel == null || controlsPanel == null)
            return;
        bool compact = (ActualWidth > 0 ? ActualWidth : Width) < 1000;
        heroPanel.Visibility = compact ? Visibility.Collapsed : Visibility.Visible;
        mainGrid.ColumnDefinitions[0].Width =
            compact ? new GridLength(0) : new GridLength(1.08, GridUnitType.Star);
        controlsPanel.Margin =
            compact ? new Thickness(22, 12, 22, 0) : new Thickness(24, 12, 28, 0);
    }
    Preferences Load()
    {
        try
        {
            var loaded =
                new JavaScriptSerializer().Deserialize<Preferences>(File.ReadAllText(config));
            if (loaded != null && loaded.Width >= 640 && loaded.Width <= 8192 &&
                loaded.Height >= 360 && loaded.Height <= 4320 && loaded.Mode >= 0 &&
                loaded.Mode <= 2 && loaded.Present >= 0 && loaded.Present <= 2 &&
                loaded.Upscale >= 0 && loaded.Upscale <= 1 && loaded.Sharpen >= 0 &&
                loaded.Sharpen <= 1 && sharpValues.Contains(loaded.Sharpness) &&
                gammaValues.Contains(loaded.Gamma) && languageValues.Contains(loaded.GameLanguage))
            {
                if (!mouseValues.Contains(loaded.MouseSensitivity))
                    loaded.MouseSensitivity = 180;
                return loaded;
            }
        }
        catch
        {
        }
        string legacy = Path.Combine(root, "launcher.ini");
        if (File.Exists(legacy))
        {
            var migrated = new Preferences();
            int size = Math.Max(0, Math.Min(4, GetPrivateProfileInt("display", "size", 0, legacy)));
            migrated.Width = new[] { 1280, 1600, 1920, 2560, 3840 }[size];
            migrated.Height = new[] { 720, 900, 1080, 1440, 2160 }[size];
            migrated.Mode = GetPrivateProfileInt("display", "fullscreen", 0, legacy) == 1 ? 1 : 0;
            migrated.GameLanguage = new[] {
                17, 1, 2
            }[Math.Max(0, Math.Min(2, GetPrivateProfileInt("preferences", "language", 0, legacy)))];
            return migrated;
        }
        return new Preferences();
    }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    static extern int GetPrivateProfileInt(string section, string key, int fallback, string path);
    bool Save()
    {
        try
        {
            string temp = config + ".new";
            File.WriteAllText(temp, new JavaScriptSerializer().Serialize(prefs),
                              new UTF8Encoding(false));
            if (File.Exists(config))
                File.Replace(temp, config, null);
            else
                File.Move(temp, config);
            return true;
        }
        catch
        {
            return false;
        }
    }
    TextBlock Text(string value, int size, string color)
    {
        return new TextBlock { Text = value, FontSize = size, Foreground = Brush(color),
                               TextWrapping = TextWrapping.Wrap };
    }
    Button Button(string text, Action action, bool primary = false)
    {
        var button =
            new Button { Content = text, Height = 42, Padding = new Thickness(16, 0, 16, 0),
                         Margin = new Thickness(0, 0, 8, 0) };
        if (primary)
        {
            button.Background = Brush("#CC362D");
            button.FontWeight = FontWeights.SemiBold;
        }
        AutomationProperties.SetName(button, text);
        button.Click += delegate
        {
            action();
        };
        return button;
    }
    void Link(StackPanel panel, string label, Action action)
    {
        var button = Button(label, action);
        button.Background = Brushes.Transparent;
        button.Foreground = Brush("#ACAEB8");
        button.FontSize = 12;
        panel.Children.Add(button);
    }
    void Render()
    {
        rendering = true;
        options.Clear();
        body = new Grid();
        body.RowDefinitions.Add(new RowDefinition { Height = new GridLength(50) });
        body.RowDefinitions.Add(new RowDefinition());
        body.RowDefinitions.Add(new RowDefinition { Height = new GridLength(92) });
        Content = new Border { BorderBrush = Brush("#303139"), BorderThickness = new Thickness(1),
                               Child = body };
        var titleBar = new Grid { Background = Brush("#101115") };
        titleBar.MouseLeftButtonDown += delegate(object s, MouseButtonEventArgs e)
        {
            if (e.ClickCount == 2)
                WindowState = WindowState == WindowState.Maximized ? WindowState.Normal
                                                                   : WindowState.Maximized;
            else
                DragMove();
        };
        var name = Text("Godzilla", 14, "#D9D8D5");
        name.Margin = new Thickness(24, 13, 0, 0);
        titleBar.Children.Add(name);
        var actions = new StackPanel { Orientation = Orientation.Horizontal,
                                       HorizontalAlignment = HorizontalAlignment.Right,
                                       Margin = new Thickness(0, 4, 8, 4) };
        var uiLang = new ComboBox { Width = 150, Height = 32, Margin = new Thickness(0, 0, 18, 0) };
        interfaceLanguage = uiLang;
        for (int i = 0; i < LauncherLanguages.Names.Length; ++i)
            uiLang.Items.Add(i == 0 ? T("Automático (Windows)", "Automatic (Windows)")
                                    : LauncherLanguages.Names[i]);
        uiLang.SelectedIndex =
            Math.Max(0, Array.IndexOf(LauncherLanguages.Codes, prefs.InterfaceLanguage));
        AutomationProperties.SetName(uiLang, T("Idioma da interface", "Interface language"));
        uiLang.SelectionChanged += delegate
        {
            if (!rendering)
            {
                prefs.InterfaceLanguage = LauncherLanguages.Codes[uiLang.SelectedIndex];
                Save();
                Render();
            }
        };
        actions.Children.Add(uiLang);
        actions.Children.Add(Button("−", delegate { WindowState = WindowState.Minimized; }));
        actions.Children.Add(Button("×", delegate { Close(); }));
        titleBar.Children.Add(actions);
        body.Children.Add(titleBar);
        var main = new Grid();
        main.ColumnDefinitions.Add(
            new ColumnDefinition { Width = new GridLength(1.08, GridUnitType.Star) });
        main.ColumnDefinitions.Add(
            new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        Grid.SetRow(main, 1);
        body.Children.Add(main);
        var hero = new Grid { Margin = new Thickness(22, 10, 12, 0), Background = Brush("#17191E"),
                              ClipToBounds = true };
        string image = Path.Combine(root, "assets", "hero.jpg");
        if (!File.Exists(image))
            image = Path.Combine(root, "launcher-assets", "hero.jpg");
        if (File.Exists(image))
        {
            var photo =
                new ImageBrush(new BitmapImage(new Uri(image))) { Stretch = Stretch.UniformToFill,
                                                                  AlignmentX = AlignmentX.Right };
            RenderOptions.SetBitmapScalingMode(hero, BitmapScalingMode.HighQuality);
            hero.Background = photo;
        }
        hero.Children.Add(new Border {
            Background = new LinearGradientBrush(
                new GradientStopCollection { new GradientStop(Color.FromArgb(0, 9, 10, 13), 0),
                                             new GradientStop(Color.FromArgb(10, 9, 10, 13), .45),
                                             new GradientStop(Color.FromArgb(245, 9, 10, 13), 1) },
                new Point(0, 0), new Point(0, 1))
        });
        var brand = new StackPanel { VerticalAlignment = VerticalAlignment.Bottom,
                                     Margin = new Thickness(30, 0, 25, 32) };
        var logo = Text("GODZILLA", 54, "#FFFFFF");
        logo.FontFamily = new FontFamily("Bahnschrift");
        logo.FontWeight = FontWeights.Bold;
        brand.Children.Add(logo);
        var credit =
            Text("©2015 BANDAI NAMCO Entertainment Inc.\nTM & © TOHO CO., LTD.", 10, "#A6A7AC");
        credit.Margin = new Thickness(2, 12, 0, 0);
        brand.Children.Add(credit);
        hero.Children.Add(brand);
        main.Children.Add(hero);
        var right = new Grid { Margin = new Thickness(24, 12, 28, 0) };
        right.RowDefinitions.Add(new RowDefinition { Height = new GridLength(50) });
        right.RowDefinitions.Add(new RowDefinition());
        Grid.SetColumn(right, 1);
        main.Children.Add(right);
        mainGrid = main;
        heroPanel = hero;
        controlsPanel = right;
        UpdateResponsiveLayout();
        var tabs = new StackPanel { Orientation = Orientation.Horizontal };
        string[] tabNames = { T("Jogar", "Play"), T("Gráficos", "Graphics"),
                              T("Controles", "Controls"), T("Atualizações", "Updates") };
        if (updateOffer != null)
            tabNames[3] += " •";
        for (int i = 0; i < tabNames.Length; ++i)
        {
            int target = i;
            var tab = Button(tabNames[i], delegate {
                page = target;
                Render();
            });
            tab.Background = Brush(i == page ? "#2F3038" : "#17181D");
            tabs.Children.Add(tab);
        }
        right.Children.Add(tabs);
        var scroll =
            new ScrollViewer { VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
                               HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled };
        Grid.SetRow(scroll, 1);
        right.Children.Add(scroll);
        content = new StackPanel { Margin = new Thickness(0, 18, 8, 10) };
        scroll.Content = content;
        if (page == 0)
            Home();
        else if (page == 1)
            Graphics();
        else if (page == 2)
            Controls();
        else
            Updates();
        var footer = new Grid { Margin = new Thickness(22, 12, 28, 14) };
        Grid.SetRow(footer, 2);
        body.Children.Add(footer);
        footer.ColumnDefinitions.Add(new ColumnDefinition());
        footer.ColumnDefinitions.Add(new ColumnDefinition());
        var links = new StackPanel { Orientation = Orientation.Horizontal,
                                     VerticalAlignment = VerticalAlignment.Center };
        Link(links, T("Salvamentos", "Save files"),
             delegate { Open(Path.Combine(install.User, "savedata")); });
        Link(links, T("Registros", "Logs"), delegate { Open(Path.Combine(install.User, "logs")); });
        Link(links, T("Sobre", "About"), delegate {
            MessageBox.Show(
                this,
                "Godzilla · 2015\nNatsume Atari / Bandai Namco\n\n" +
                    T("Código baseado em bbport e shadPS4.", "Based on bbport and shadPS4.") +
                    "\n" + T("Licença GPL-2.0-or-later.", "License GPL-2.0-or-later."),
                "Godzilla");
        });
        footer.Children.Add(links);
        var launch =
            new StackPanel { HorizontalAlignment = HorizontalAlignment.Stretch, MaxWidth = 300 };
        Grid.SetColumn(launch, 1);
        status = Text("", 12, "#DAB5AF");
        status.TextWrapping = TextWrapping.NoWrap;
        status.TextTrimming = TextTrimming.CharacterEllipsis;
        status.Margin = new Thickness(0, 0, 0, 7);
        launch.Children.Add(status);
        if (updateOffer != null && !running)
            status.Text = T("Nova versão disponível: ", "New version available: ") +
                          updateOffer.version + T(" — veja Atualizações.", " — see Updates.");
        play = Button(running ? T("Em jogo", "Playing") : T("Jogar", "Play"),
                      delegate { Launch(); }, true);
        play.Height = 46;
        play.Margin = new Thickness(0);
        play.IsEnabled = !running && !updating;
        launch.Children.Add(play);
        footer.Children.Add(launch);
        if (running)
            foreach (var option in options)
                option.IsEnabled = false;
        rendering = false;
    }
    void Heading(string text)
    {
        var title = Text(text, 24, "#F4F1EC");
        title.FontWeight = FontWeights.SemiBold;
        title.Margin = new Thickness(0, 0, 0, 16);
        content.Children.Add(title);
    }
    ComboBox Choice(string label, string[] labels, int selected, Action<int> changed,
                    string hint = null)
    {
        var group = new StackPanel { Margin = new Thickness(0, 0, 0, 15) };
        var caption = Text(label, 13, "#BABBC3");
        caption.Margin = new Thickness(0, 0, 0, 6);
        group.Children.Add(caption);
        var box = new ComboBox { Height = 38 };
        foreach (var item in labels)
            box.Items.Add(item);
        box.SelectedIndex = Math.Max(0, Math.Min(labels.Length - 1, selected));
        AutomationProperties.SetName(box, label);
        box.SelectionChanged += delegate
        {
            if (!rendering)
            {
                changed(box.SelectedIndex);
                if (!Save())
                    status.Text = T("Não foi possível salvar as preferências.",
                                    "Could not save preferences.");
            }
        };
        options.Add(box);
        group.Children.Add(box);
        if (hint != null)
        {
            var note = Text(hint, 11, "#81848F");
            note.Margin = new Thickness(0, 6, 0, 0);
            group.Children.Add(note);
        }
        content.Children.Add(group);
        return box;
    }
    void Updates()
    {
        Heading(T("Atualizações", "Updates"));
        content.Children.Add(
            Text("Godzilla for Windows · " + ReleaseUpdater.Current(root).display, 16, "#F4F1EC"));
        var note = Text(T("Atualize o jogo e o launcher sem alterar seus saves.",
                          "Update the game and launcher while keeping your saves."),
                        13, "#ACAEB8");
        note.Margin = new Thickness(0, 12, 0, 18);
        content.Children.Add(note);
        var check =
            Button(T("Buscar atualização", "Check for updates"), delegate { CheckUpdates(); });
        check.IsEnabled = !running && !updating;
        content.Children.Add(check);
        if (updateOffer != null)
        {
            var version =
                Text(T("Nova versão:", "New version:") + " " + updateOffer.version, 15, "#F4F1EC");
            version.Margin = new Thickness(0, 16, 0, 12);
            content.Children.Add(version);
            var installUpdate = Button(T("Atualizar e reiniciar", "Update and restart"),
                                       delegate { InstallUpdate(); }, true);
            installUpdate.IsEnabled = !running && !updating;
            content.Children.Add(installUpdate);
        }
        var message = Text(updateMessage, 13, "#DAB5AF");
        message.Margin = new Thickness(0, 14, 0, 0);
        content.Children.Add(message);
    }
    // On open: announce a newer release without blocking; offline or errors stay silent.
    async void CheckUpdatesQuietly()
    {
        if (running || updating)
            return;
        try
        {
            var offer = await Task.Run(delegate { return ReleaseUpdater.Check(root); });
            if (offer == null || running || updating)
                return;
            updateOffer = offer;
            updateMessage = T("Atualização disponível.", "Update available.");
            Render();
        }
        catch
        {
        }
    }
    async void CheckUpdates()
    {
        if (running || updating)
            return;
        updating = true;
        updateOffer = null;
        updateMessage = T("Buscando atualizações…", "Checking for updates…");
        Render();
        try
        {
            updateOffer = await Task.Run(delegate { return ReleaseUpdater.Check(root); });
            updateMessage = updateOffer == null ? T("Você está na versão mais recente.",
                                                    "You have the latest version.")
                                                : T("Atualização disponível.", "Update available.");
        }
        catch
        {
            updateMessage = T("Não foi possível buscar atualizações. Tente novamente mais tarde.",
                              "Could not check for updates. Try again later.");
        }
        updating = false;
        Render();
    }
    static string QuoteArgument(string value)
    {
        var text = new StringBuilder("\"");
        int slashes = 0;
        foreach (char c in value)
        {
            if (c == '\\')
            {
                ++slashes;
                continue;
            }
            text.Append('\\', c == '"' ? slashes * 2 + 1 : slashes);
            text.Append(c);
            slashes = 0;
        }
        return text.Append('\\', slashes * 2).Append('"').ToString();
    }
    async void InstallUpdate()
    {
        if (running || updating || updateOffer == null)
            return;
        updating = true;
        updateMessage =
            T("Baixando e verificando a atualização…", "Downloading and verifying the update…");
        Render();
        try
        {
            var offer = updateOffer;
            string stage = await Task.Run(delegate { return ReleaseUpdater.Download(offer); });
            string folder = Path.GetDirectoryName(stage),
                   helper = Path.Combine(folder, "Godzilla.Update.exe"),
                   jobPath = Path.Combine(folder, "job.json");
            File.Copy(Path.Combine(root, "Godzilla.Update.exe"), helper, false);
            using (var parent = Process.GetCurrentProcess()) ReleaseUpdater.Write(
                jobPath, new UpdateJob { root = root, stage = stage, parent = parent.Id,
                                         started = parent.StartTime.ToUniversalTime().Ticks });
            Process.Start(new ProcessStartInfo(helper, "--apply " + QuoteArgument(jobPath)) {
                UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = folder
            });
            updating = false;
            Application.Current.Shutdown();
        }
        catch
        {
            updating = false;
            updateMessage = T("Não foi possível atualizar. Seus saves foram preservados.",
                              "Could not update. Your saves were preserved.");
            Render();
        }
    }
    void Home()
    {
        Heading(T("Pronto para jogar", "Ready to play"));
        var ids = AvailableGameLanguages();
        var names =
            ids.Select(id => id < 0 ? T("Automático (Windows)", "Automatic (Windows)")
                                    : LauncherLanguages
                                          .GameNames[Array.IndexOf(LauncherLanguages.GameIds, id)])
                .ToArray();
        Choice(T("Idioma do jogo", "Game language"), names,
               Math.Max(0, ids.IndexOf(prefs.GameLanguage)),
               delegate(int i) { prefs.GameLanguage = ids[i]; });
        var card = new StackPanel { Margin = new Thickness(0, 8, 0, 0) };
        card.Children.Add(Text(T("Tela", "Display"), 13, "#8E919B"));
        card.Children.Add(Text((prefs.Mode == 0   ? T("Janela", "Windowed")
                                : prefs.Mode == 1 ? T("Tela cheia", "Fullscreen")
                                                  : T("Sem bordas", "Borderless")) +
                                   " · " + prefs.Width + " × " + prefs.Height,
                               18, "#DFDFE5"));
        var configure = Button(T("Configurar gráficos", "Graphics settings"), delegate {
            page = 1;
            Render();
        });
        configure.Margin = new Thickness(0, 20, 0, 0);
        card.Children.Add(configure);
        content.Children.Add(new Border { Background = Brush("#1B1D23"),
                                          CornerRadius = new CornerRadius(10),
                                          Padding = new Thickness(20),
                                          Margin = new Thickness(0, 12, 0, 0), Child = card });
        if (!install.Ready)
        {
            var error =
                Text(T("Os arquivos preparados do jogo não foram encontrados nesta instalação.",
                       "Prepared game files were not found in this installation."),
                     13, "#E1A79E");
            error.Margin = new Thickness(0, 20, 0, 0);
            content.Children.Add(error);
        }
    }
    void Graphics()
    {
        Heading(T("Gráficos", "Graphics"));
        Choice(T("Modo de tela", "Display mode"),
               new[] { T("Janela", "Windowed"), T("Tela cheia", "Fullscreen"),
                       T("Janela sem bordas", "Borderless") },
               prefs.Mode, delegate(int i) {
                   prefs.Mode = i;
                   Render();
               });
        var modes = DisplayModes.Read(prefs.Mode == 1);
        string current = prefs.Width + " × " + prefs.Height;
        int selected = modes.IndexOf(current);
        if (selected < 0)
        {
            selected = 0;
            var size = modes[0].Split('×');
            prefs.Width = Int32.Parse(size[0]);
            prefs.Height = Int32.Parse(size[1]);
        }
        var resolution =
            Choice(T("Resolução de saída", "Output resolution"), modes.ToArray(), selected,
                   delegate(int i) {
                       var size = modes[i].Split('×');
                       prefs.Width = Int32.Parse(size[0]);
                       prefs.Height = Int32.Parse(size[1]);
                   },
                   T("A resolução interna do jogo permanece original.",
                     "The game's internal rendering resolution remains unchanged."));
        resolution.IsEnabled = prefs.Mode != 2 && !running;
        Choice(T("Sincronização vertical", "Vertical sync"),
               new[] { T("Ligada", "On"), "Mailbox", T("Desligada", "Off") }, prefs.Present,
               delegate(int i) { prefs.Present = i; });
        Choice(T("Ampliação da imagem", "Image upscaling"),
               new[] { T("Padrão", "Default"), "AMD FSR 1" }, prefs.Upscale, delegate(int i) {
                   prefs.Upscale = i;
                   Render();
               });
        if (prefs.Upscale == 1)
        {
            Choice(T("Nitidez RCAS", "RCAS sharpening"),
                   new[] { T("Desligada", "Off"), T("Ligada", "On") }, prefs.Sharpen,
                   delegate(int i) {
                       prefs.Sharpen = i;
                       Render();
                   });
            if (prefs.Sharpen == 1)
                Choice(T("Intensidade", "Strength"),
                       new[] { T("Suave", "Soft"), T("Moderada", "Medium"), T("Forte", "Strong"),
                               T("Máxima", "Maximum") },
                       Math.Max(0, Array.IndexOf(sharpValues, prefs.Sharpness)),
                       delegate(int i) { prefs.Sharpness = sharpValues[i]; });
        }
        Choice(T("Gama", "Gamma"), new[] { "80%", "90%", "100%", "110%", "120%" },
               Math.Max(0, Array.IndexOf(gammaValues, prefs.Gamma)),
               delegate(int i) { prefs.Gamma = gammaValues[i]; });
        var reset = Button(T("Restaurar gráficos padrão", "Reset graphics defaults"), delegate {
            prefs.Mode = 1;
            prefs.Width = 1920;
            prefs.Height = 1080;
            prefs.Present = 0;
            prefs.Upscale = 0;
            prefs.Sharpen = 1;
            prefs.Sharpness = 250;
            prefs.Gamma = 100;
            Save();
            Render();
        });
        content.Children.Add(reset);
    }
    void Controls()
    {
        Heading(T("Controles", "Controls"));
        Choice(T("Ícones do controle", "Controller icons"),
               new[] { T("Automático", "Automatic"), "PlayStation", "Xbox", "Nintendo" },
               ControllerTemplate(), delegate(int i) { SaveControllerTemplate(i); });
        Choice(T("Sensibilidade do mouse", "Mouse sensitivity"),
               new[] { "50%", "75%", "100%", "150%", "200%" },
               Math.Max(0, Array.IndexOf(mouseValues, prefs.MouseSensitivity)),
               delegate(int i) { prefs.MouseSensitivity = mouseValues[i]; });
        var info =
            Text(T("Teclado e mouse prontos para jogar. Os avisos acompanham o dispositivo em uso.",
                   "Keyboard and mouse are ready. Prompts follow the active input device."),
                 14, "#BFC0C8");
        info.Margin = new Thickness(0, 0, 0, 25);
        content.Children.Add(info);
        string[,] mapping = { { "W / S", T("Mover", "Move") },
                              { "Mouse", T("Câmera", "Camera") },
                              { "Mouse 1 / Z", T("Ataque normal", "Normal attack") },
                              { "Mouse 2 / X", T("Ataque forte", "Powerful attack") },
                              { "C / Mouse 3", T("Sopro atômico", "Atomic breath") },
                              { "Shift", T("Investida", "Charge") },
                              { "A / D, Q / E", T("Girar", "Turn") },
                              { "R", T("Rugido", "Roar") },
                              { T("Espaço", "Space"),
                                T("Esquiva de emergência", "Emergency evade") },
                              { "F", T("Coletar dados", "Collect data") },
                              { "Esc", T("Pausa / Pular vídeo", "Pause / Skip movie") },
                              { "Tab", T("Comando secundário", "Secondary command") },
                              { "F10", T("Modo foto", "Photo mode") },
                              { "Enter", T("Confirmar", "Confirm") },
                              { "Esc", T("Voltar", "Back") },
                              { T("Setas", "Arrow keys"), T("Selecionar", "Select") },
                              { "F7", T("Soltar / prender mouse", "Release / capture mouse") },
                              { "F12", T("Salvar foto PNG", "Save PNG photo") } };
        for (int i = 0; i < mapping.GetLength(0); ++i)
        {
            var row = new Grid { Margin = new Thickness(0, 0, 0, 10) };
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(165) });
            row.ColumnDefinitions.Add(new ColumnDefinition());
            row.Children.Add(Text(mapping[i, 0], 14, "#E4E2DE"));
            var action = Text(mapping[i, 1], 14, "#979BA8");
            Grid.SetColumn(action, 1);
            row.Children.Add(action);
            content.Children.Add(row);
        }
    }
    void Open(string path)
    {
        try
        {
            if (!Directory.Exists(path))
                throw new IOException();
            Process.Start(new ProcessStartInfo(path) { UseShellExecute = true });
        }
        catch
        {
            status.Text =
                T("Esta pasta ainda não está disponível.", "This folder is not available yet.");
        }
    }
    static void Default(ProcessStartInfo start, string name, string value)
    {
        if (String.IsNullOrEmpty(start.EnvironmentVariables[name]))
            start.EnvironmentVariables[name] = value;
    }
    public ProcessStartInfo StartInfo(string log)
    {
        var start = new ProcessStartInfo(Path.Combine(root, "gzport.exe"),
                                         "\"" + Path.Combine(install.Work, "out") + "\" --user \"" +
                                             install.User + "\" --log \"" + log + "\"");
        start.UseShellExecute = false;
        start.CreateNoWindow = true;
        start.WorkingDirectory = root;
        start.RedirectStandardOutput = true;
        start.RedirectStandardError = true;
        Default(start, "GZ_WINDOW_WIDTH", prefs.Width.ToString());
        Default(start, "GZ_WINDOW_HEIGHT", prefs.Height.ToString());
        Default(start, "GZ_FULLSCREEN", prefs.Mode.ToString());
        Default(start, "GZ_LANGUAGE", EffectiveGameLanguage().ToString());
        Default(start, "GZ_PRESENT_MODE",
                new[] { "Fifo", "Mailbox", "Immediate" }[Math.Max(0, Math.Min(2, prefs.Present))]);
        Default(start, "GZ_FSR1", prefs.Upscale.ToString());
        Default(start, "GZ_RCAS", prefs.Sharpen.ToString());
        Default(start, "GZ_RCAS_ATTENUATION", prefs.Sharpness.ToString());
        Default(start, "GZ_GAMMA", prefs.Gamma.ToString());
        Default(start, "GZ_MOUSE_SENSITIVITY",
                (Math.Max(20, Math.Min(500, prefs.MouseSensitivity)) / 100.0)
                    .ToString("0.00", CultureInfo.InvariantCulture));
        string fonts = Path.Combine(root, "fonts");
        if (!Directory.Exists(fonts))
            fonts = Path.GetFullPath(Path.Combine(root, "..", "third_party", "shadPS4", "src",
                                                  "imgui", "renderer", "fonts"));
        if (File.Exists(Path.Combine(fonts, "NotoSans-Regular.ttf")))
            Default(start, "GZ_SYSTEM_FONT", Path.Combine(fonts, "NotoSans-Regular.ttf"));
        if (File.Exists(Path.Combine(fonts, "NotoSansCJK-Regular.ttc")))
            Default(start, "GZ_SYSTEM_FONT_JP", Path.Combine(fonts, "NotoSansCJK-Regular.ttc"));
        return start;
    }
    async void Launch()
    {
        if (running || updating)
            return;
        if (!install.Ready)
        {
            status.Text = T("Arquivos do jogo não encontrados.", "Game files not found.");
            return;
        }
        running = true;
        Save();
        Render();
        try
        {
            string logs = Path.Combine(install.User, "logs");
            Directory.CreateDirectory(logs);
            lastLog = Path.Combine(logs, "play_" + DateTime.Now.ToString("yyyyMMdd_HHmmss_fff") +
                                             "_" + Process.GetCurrentProcess().Id + ".log");
            var writerLock = new object();
            using (var writer = new StreamWriter(lastLog + ".console.log", false, Encoding.UTF8))
            {
                writer.AutoFlush = true;
                using (var child = new Process())
                {
                    child.StartInfo = StartInfo(lastLog);
                    DataReceivedEventHandler record =
                        delegate(object sender, DataReceivedEventArgs e)
                    {
                        if (e.Data != null)
                            lock (writerLock) writer.WriteLine(e.Data);
                    };
                    child.OutputDataReceived += record;
                    child.ErrorDataReceived += record;
                    child.Start();
                    child.BeginOutputReadLine();
                    child.BeginErrorReadLine();
                    WindowState = WindowState.Minimized;
                    await Task.Run(delegate { child.WaitForExit(); });
                    if (child.ExitCode != 0)
                        throw new IOException(
                            T("O jogo encerrou inesperadamente. O registro está disponível abaixo.",
                              "The game closed unexpectedly. Its log is available below."));
                }
            }
            running = false;
            WindowState = WindowState.Normal;
            Render();
        }
        catch (Exception error)
        {
            try
            {
                if (lastLog != null)
                    File.AppendAllText(lastLog + ".console.log",
                                       Environment.NewLine + error.ToString());
            }
            catch
            {
            }
            running = false;
            WindowState = WindowState.Normal;
            Render();
            status.Text = T("Falha ao iniciar. Consulte Registros.", "Could not start. Open Logs.");
        }
    }
    ComboBox FindChoice(string name)
    {
        return options.OfType<ComboBox>().First(c => AutomationProperties.GetName(c) == name);
    }
    int ControllerTemplate()
    {
        try
        {
            int value;
            if (Int32.TryParse(
                    File.ReadAllText(Path.Combine(install.User, "pc-controller.txt")).Trim(),
                    out value) &&
                value >= 0 && value <= 3)
                return value;
        }
        catch
        {
        }
        return 0;
    }
    void SaveControllerTemplate(int value)
    {
        try
        {
            Directory.CreateDirectory(install.User);
            string file = Path.Combine(install.User, "pc-controller.txt"),
                   temp = file + ".launcher.tmp";
            File.WriteAllText(temp, value.ToString(CultureInfo.InvariantCulture) + "\n",
                              new UTF8Encoding(false));
            if (File.Exists(file))
                File.Replace(temp, file, null);
            else
                File.Move(temp, file);
        }
        catch
        {
            status.Text =
                T("Não foi possível salvar o controle.", "Could not save controller preference.");
        }
    }
    string RunFixture()
    {
        var start = StartInfo(Path.Combine(install.User, "fixture.log"));
        using (var process = Process.Start(start))
        {
            string output = process.StandardOutput.ReadToEnd();
            string error = process.StandardError.ReadToEnd();
            if (!process.WaitForExit(10000) || process.ExitCode != 0)
                throw new Exception("Fixture failed: " + error);
            return output.Trim();
        }
    }
    void SelfTest()
    {
        prefs.InterfaceLanguage = "en";
        page = 2;
        Render();
        for (int template = 0; template <= 3; ++template)
        {
            FindChoice("Controller icons").SelectedIndex = template;
            if (ControllerTemplate() != template)
                throw new Exception("Controller template did not persist");
        }
        FindChoice("Controller icons").SelectedIndex = 0;
        FindChoice("Mouse sensitivity").SelectedIndex = 3;
        if (StartInfo("fixture.log").EnvironmentVariables["GZ_MOUSE_SENSITIVITY"] != "2.70")
            throw new Exception("Mouse sensitivity did not reach child environment");
        prefs.MouseSensitivity = 180;
        prefs.GameLanguage = 17;
        interfaceLanguage.SelectedIndex = 2;
        if (prefs.InterfaceLanguage != "en" || (string)play.Content != "Play")
            throw new Exception("Interface did not translate");
        if (prefs.GameLanguage != 17)
            throw new Exception("Interface language changed game language");
        page = 1;
        Render();
        FindChoice("Display mode").SelectedIndex = 0;
        var res = FindChoice("Output resolution");
        res.SelectedItem = "1920 × 1080";
        FindChoice("Vertical sync").SelectedIndex = 1;
        FindChoice("Image upscaling").SelectedIndex = 1;
        FindChoice("Gamma").SelectedIndex = 3;
        string first = RunFixture();
        if (first != "width=1920 height=1080 fullscreen=0 fsr=1 gamma=110 present=Mailbox " +
                         "language=17 rcas=1 sharpness=250")
            throw new Exception("First child options: " + first);
        FindChoice("Output resolution").SelectedItem = "1600 × 900";
        FindChoice("Image upscaling").SelectedIndex = 0;
        FindChoice("Gamma").SelectedIndex = 2;
        page = 0;
        Render();
        FindChoice("Game language").SelectedItem = "Français";
        string second = RunFixture();
        if (second != "width=1600 height=900 fullscreen=0 fsr=0 gamma=100 present=Mailbox " +
                          "language=2 rcas=1 sharpness=250")
            throw new Exception("Second child options: " + second);
        if (!Save())
            throw new Exception("Preferences did not save");
        var loaded = Load();
        if (loaded.Width != 1600 || loaded.GameLanguage != 2 || loaded.InterfaceLanguage != "en")
            throw new Exception("Preferences did not reload");
        interfaceLanguage.SelectedIndex = 1;
        if ((string)play.Content != "Jogar" || prefs.GameLanguage != 2)
            throw new Exception("Portuguese translation failed");
        page = 1;
        Render();
        FindChoice("Modo de tela").SelectedIndex = 1;
        if (!FindChoice("Resolução de saída").IsEnabled || DisplayModes.Read(true).Count == 0)
            throw new Exception("Fullscreen resolution disabled");
        FindChoice("Modo de tela").SelectedIndex = 2;
        if (FindChoice("Resolução de saída").IsEnabled)
            throw new Exception("Borderless resolution must follow desktop");
        if (LauncherLanguages.Resolve("auto", "ru-RU") != "en" ||
            LauncherLanguages.Resolve("auto", "fr-CA") != "fr" ||
            LauncherLanguages.Resolve("auto", "es-MX") != "es-419" ||
            LauncherLanguages.Resolve("auto", "pt-BR") != "pt-BR")
            throw new Exception("System language fallback failed");
        foreach (string code in LauncherLanguages.Codes.Where(c => c != "auto"))
        {
            prefs.InterfaceLanguage = code;
            for (int p = 0; p < 3; ++p)
            {
                page = p;
                Render();
            }
        }
        if (LauncherLanguages.Missing.Count != 0)
            throw new Exception("Missing translations: " +
                                String.Join(", ", LauncherLanguages.Missing));
        // Exercise every offered value individually through the UI and child
        // process, then reload preferences. This does not certify GPU effects.
        prefs.InterfaceLanguage = "en";
        page = 1;
        Render();
        foreach (string name in new[] { "Display mode", "Vertical sync", "Image upscaling",
                                        "Gamma" })
        {
            int count = FindChoice(name).Items.Count;
            for (int i = 0; i < count; ++i)
            {
                FindChoice(name).SelectedIndex = i;
                CheckFixturePreferences();
            }
        }
        FindChoice("Display mode").SelectedIndex = 0;
        var sizes = FindChoice("Output resolution")
                        .Items.Cast<object>()
                        .Select(x => x.ToString())
                        .ToArray();
        foreach (string size in sizes)
        {
            FindChoice("Output resolution").SelectedItem = size;
            CheckFixturePreferences();
        }
        FindChoice("Image upscaling").SelectedIndex = 1;
        FindChoice("RCAS sharpening").SelectedIndex = 0;
        CheckFixturePreferences();
        FindChoice("RCAS sharpening").SelectedIndex = 1;
        int strengths = FindChoice("Strength").Items.Count;
        for (int i = 0; i < strengths; ++i)
        {
            FindChoice("Strength").SelectedIndex = i;
            CheckFixturePreferences();
        }
        page = 0;
        Render();
        int languages = FindChoice("Game language").Items.Count;
        for (int i = 0; i < languages; ++i)
        {
            FindChoice("Game language").SelectedIndex = i;
            CheckFixturePreferences();
        }
    }
    void CheckFixturePreferences()
    {
        string expected = "width=" + prefs.Width + " height=" + prefs.Height +
                          " fullscreen=" + prefs.Mode + " fsr=" + prefs.Upscale +
                          " gamma=" + prefs.Gamma +
                          " present=" + new[] { "Fifo", "Mailbox", "Immediate" }[prefs.Present] +
                          " language=" + EffectiveGameLanguage() + " rcas=" + prefs.Sharpen +
                          " sharpness=" + prefs.Sharpness;
        string actual = RunFixture();
        if (actual != expected)
            throw new Exception("Option transport: " + actual + " expected " + expected);
        if (!Save())
            throw new Exception("Option persistence failed");
        var serializer = new JavaScriptSerializer();
        if (serializer.Serialize(prefs) != serializer.Serialize(Load()))
            throw new Exception("Option reload mismatch");
        File.AppendAllText(Path.Combine(root, "self-test-options.jsonl"), serializer.Serialize(new {
            expected = expected, actual = actual, persisted = true
        }) + Environment.NewLine);
    }
    public void Preview(string path, int selectedPage, string language)
    {
        prefs.InterfaceLanguage = language;
        page = selectedPage;
        Render();
        Show();
        UpdateLayout();
        var image = new RenderTargetBitmap((int)ActualWidth, (int)ActualHeight, 96, 96,
                                           PixelFormats.Pbgra32);
        image.Render(this);
        var png = new PngBitmapEncoder();
        png.Frames.Add(BitmapFrame.Create(image));
        using (var output = File.Create(path)) png.Save(output);
        Close();
    }
    [STAThread]
    public static int Main(string[] args)
    {
        bool owned;
        using (var mutex = new Mutex(true, "Local\\GodzillaLauncherWpf", out owned))
        {
            if (!owned)
                return 3;
            var app = new Application();
            bool testing = args.Length == 2 && args[0] == "--self-test";
            var window = new GodzillaLauncher(testing ? Path.GetFullPath(args[1])
                                                      : AppDomain.CurrentDomain.BaseDirectory);
            if (testing)
            {
                string report = Path.Combine(Path.GetFullPath(args[1]), "self-test.txt");
                try
                {
                    window.SelfTest();
                    window.Close();
                    File.WriteAllText(
                        report,
                        "PASS: interface languages, independent game language, every " +
                            "offered graphics value, child environment, persistence and " +
                            "fullscreen resolution UI. GPU effects require separate tests.");
                    return 0;
                }
                catch (Exception error)
                {
                    window.Close();
                    File.WriteAllText(report, error.ToString());
                    return 1;
                }
            }
            if (args.Length == 4 && args[0] == "--preview")
            {
                window.Preview(args[1], Int32.Parse(args[2]), args[3]);
                return 0;
            }
            try
            {
                NativeRuntime.Ensure(window.root);
            }
            catch
            {
                MessageBox.Show(
                    "Não foi possível preparar as bibliotecas do aplicativo. Extraia novamente o " +
                        "pacote completo em uma pasta gravável. Seus saves foram " +
                        "preservados.\n\nCould not prepare application libraries. Extract the " +
                        "complete package again into a writable folder. Your saves were preserved.",
                    "Godzilla", MessageBoxButton.OK, MessageBoxImage.Information);
                return 1;
            }
            try
            {
                PromptArtworkUpdate.Apply(window.root, window.install.User,
                                          window.install.GameFile("game_bp.pac"));
            }
            catch
            {
                MessageBox.Show(
                    "Não foi possível aplicar a correção dos símbolos. Os arquivos anteriores e " +
                        "seus saves foram preservados.\n\nCould not apply the tutorial symbols " +
                        "update. Previous files and saves were preserved.",
                    "Godzilla", MessageBoxButton.OK, MessageBoxImage.Information);
                return 1;
            }
            window.Loaded += delegate
            {
                window.CheckUpdatesQuietly();
            };
            app.Run(window);
            return 0;
        }
    }
    const string Styles =
        @"<ResourceDictionary xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' xmlns:x='http://schemas.microsoft.com/winfx/2006/xaml'>
<Style TargetType='ScrollBar'><Setter Property='Width' Value='10'/><Setter Property='Background' Value='#17191F'/><Setter Property='Template'><Setter.Value><ControlTemplate TargetType='ScrollBar'><Grid Background='{TemplateBinding Background}'><Track x:Name='PART_Track' IsDirectionReversed='True'><Track.DecreaseRepeatButton><RepeatButton Command='ScrollBar.PageUpCommand' Opacity='0' Focusable='False'/></Track.DecreaseRepeatButton><Track.Thumb><Thumb><Thumb.Template><ControlTemplate TargetType='Thumb'><Border Background='#4A4D59' CornerRadius='4' Margin='2,0'/></ControlTemplate></Thumb.Template></Thumb></Track.Thumb><Track.IncreaseRepeatButton><RepeatButton Command='ScrollBar.PageDownCommand' Opacity='0' Focusable='False'/></Track.IncreaseRepeatButton></Track></Grid></ControlTemplate></Setter.Value></Setter></Style>
<Style TargetType='Button'><Setter Property='Background' Value='#22242C'/><Setter Property='Foreground' Value='#F2F0ED'/><Setter Property='BorderThickness' Value='0'/><Setter Property='FontSize' Value='14'/><Setter Property='Cursor' Value='Hand'/><Setter Property='Template'><Setter.Value><ControlTemplate TargetType='Button'><Border x:Name='Frame' Background='{TemplateBinding Background}' CornerRadius='7' Padding='{TemplateBinding Padding}'><ContentPresenter HorizontalAlignment='Center' VerticalAlignment='Center'/></Border><ControlTemplate.Triggers><Trigger Property='IsMouseOver' Value='True'><Setter TargetName='Frame' Property='Opacity' Value='.8'/></Trigger><Trigger Property='IsKeyboardFocused' Value='True'><Setter TargetName='Frame' Property='BorderBrush' Value='#C9A598'/><Setter TargetName='Frame' Property='BorderThickness' Value='1'/></Trigger><Trigger Property='IsEnabled' Value='False'><Setter TargetName='Frame' Property='Opacity' Value='.45'/></Trigger></ControlTemplate.Triggers></ControlTemplate></Setter.Value></Setter></Style>
<Style TargetType='ComboBoxItem'><Setter Property='Foreground' Value='#E6E5EA'/><Setter Property='Padding' Value='12,9'/><Setter Property='Template'><Setter.Value><ControlTemplate TargetType='ComboBoxItem'><Border x:Name='Item' Background='Transparent' Padding='{TemplateBinding Padding}'><ContentPresenter/></Border><ControlTemplate.Triggers><Trigger Property='IsHighlighted' Value='True'><Setter TargetName='Item' Property='Background' Value='#393C49'/></Trigger></ControlTemplate.Triggers></ControlTemplate></Setter.Value></Setter></Style>
<Style TargetType='ComboBox'><Setter Property='Foreground' Value='#E8E7EC'/><Setter Property='Background' Value='#22242D'/><Setter Property='FontSize' Value='13'/><Setter Property='Template'><Setter.Value><ControlTemplate TargetType='ComboBox'><Grid><ToggleButton x:Name='Toggle' Focusable='False' IsChecked='{Binding IsDropDownOpen,RelativeSource={RelativeSource TemplatedParent},Mode=TwoWay}'><ToggleButton.Template><ControlTemplate TargetType='ToggleButton'><Border Background='#22242D' BorderBrush='#343741' BorderThickness='1' CornerRadius='6'><TextBlock Text='⌄' Foreground='#A2A5AF' HorizontalAlignment='Right' VerticalAlignment='Center' Margin='0,0,12,4'/></Border></ControlTemplate></ToggleButton.Template></ToggleButton><ContentPresenter IsHitTestVisible='False' Content='{TemplateBinding SelectionBoxItem}' Margin='12,0,32,0' VerticalAlignment='Center'/><Popup Name='PART_Popup' IsOpen='{TemplateBinding IsDropDownOpen}' Placement='Bottom' AllowsTransparency='True' Focusable='False' PopupAnimation='Fade'><Border MinWidth='{Binding ActualWidth,RelativeSource={RelativeSource TemplatedParent}}' MaxHeight='320' Background='#272A33' BorderBrush='#454956' BorderThickness='1' CornerRadius='6' Margin='0,4,0,0'><ScrollViewer CanContentScroll='True'><ItemsPresenter KeyboardNavigation.DirectionalNavigation='Contained'/></ScrollViewer></Border></Popup></Grid><ControlTemplate.Triggers><Trigger Property='IsEnabled' Value='False'><Setter Property='Opacity' Value='.45'/></Trigger></ControlTemplate.Triggers></ControlTemplate></Setter.Value></Setter></Style>
</ResourceDictionary>";
}
public static class DisplayModes
{
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct Mode
    {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
        public string Device;
        public short Spec, Driver, Size, Extra;
        public int Fields, X, Y, Orientation, Fixed;
        public short Color, Duplex, YRes, TT, Collate;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
        public string Form;
        public short Dpi;
        public int Bits, Width, Height, Flags, Frequency, ICM, Intent, Media, Dither, R1, R2, PanW,
            PanH;
    }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern bool EnumDisplaySettings(string device, int index, ref Mode mode);
    public static List<string> Read(bool fullscreen)
    {
        var sizes = new List<string>();
        if (fullscreen)
        {
            for (int i = 0; i < 1000; ++i)
            {
                var m = new Mode();
                m.Size = (short)Marshal.SizeOf(m);
                if (!EnumDisplaySettings(null, i, ref m))
                    break;
                if (m.Bits >= 24 && m.Width >= 1280 && m.Height >= 720)
                {
                    string label = m.Width + " × " + m.Height;
                    if (!sizes.Contains(label))
                        sizes.Add(label);
                }
            }
        }
        else
            sizes.AddRange(
                new[] { "1280 × 720", "1600 × 900", "1920 × 1080", "2560 × 1440", "3840 × 2160" });
        if (sizes.Count == 0)
            sizes.Add("1920 × 1080");
        return sizes.OrderByDescending(s => Int32.Parse(s.Split('×')[0])).ToList();
    }
}
