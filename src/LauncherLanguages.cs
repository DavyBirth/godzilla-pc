// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.Collections.Generic;
using System.Globalization;

public static class LauncherLanguages
{
    public static readonly string[] Codes = { "auto", "pt-BR", "en",     "fr", "de",
                                              "it",   "es-ES", "es-419", "ja" };
    public static readonly string[] Names = { "Automatic (Windows)",
                                              "Português (Brasil)",
                                              "English",
                                              "Français",
                                              "Deutsch",
                                              "Italiano",
                                              "Español (España)",
                                              "Español (Latinoamérica)",
                                              "日本語" };
    public static readonly int[] GameIds = { 17, 1, 2, 5, 4, 3, 20, 0 };
    public static readonly string[] Suffixes = { "bp", "en", "fr", "de", "it", "es", "ns", "" };
    public static readonly string[] GameNames = {
        "Português (Brasil)",      "English", "Français", "Deutsch", "Italiano", "Español (España)",
        "Español (Latinoamérica)", "日本語"
    };
    static readonly string[] TranslatedCodes = { "en", "fr", "de", "it", "es-ES", "es-419", "ja" };
    static readonly Dictionary<string, string[]> Catalog = ReadCatalog();
    public static readonly HashSet<string> Missing = new HashSet<string>();
    public static string Resolve(string selected, string system)
    {
        if (selected != "auto" && Array.IndexOf(Codes, selected) >= 1)
            return selected;
        string name = (system ?? "").ToLowerInvariant();
        if (name.StartsWith("pt"))
            return "pt-BR";
        if (name.StartsWith("en"))
            return "en";
        if (name.StartsWith("fr"))
            return "fr";
        if (name.StartsWith("de"))
            return "de";
        if (name.StartsWith("it"))
            return "it";
        if (name == "es-es")
            return "es-ES";
        if (name.StartsWith("es"))
            return "es-419";
        if (name.StartsWith("ja"))
            return "ja";
        return "en";
    }
    public static int SystemGameId(string system)
    {
        string code = Resolve("auto", system);
        if (code == "pt-BR")
            return 17;
        if (code == "fr")
            return 2;
        if (code == "de")
            return 5;
        if (code == "it")
            return 4;
        if (code == "es-ES")
            return 3;
        if (code == "es-419")
            return 20;
        if (code == "ja")
            return 0;
        return 1;
    }
    public static string Text(string code, string english, string portuguese)
    {
        if (code == "pt-BR")
            return portuguese;
        if (code == "en")
            return english;
        string[] row;
        if (!Catalog.TryGetValue(english.Replace("\n", "\\n"), out row))
        {
            Missing.Add(english);
            return english;
        }
        int language = Array.IndexOf(TranslatedCodes, code);
        return language < 0 ? english : row[language].Replace("\\n", "\n");
    }
    static Dictionary<string, string[]> ReadCatalog()
    {
        var result = new Dictionary<string, string[]>();
        foreach (string source in Rows.Split('\n'))
        {
            string line = source.Trim();
            if (line.Length == 0)
                continue;
            var cells = line.Split('|');
            if (cells.Length != TranslatedCodes.Length)
                throw new Exception("Invalid translation row: " + cells[0]);
            result.Add(cells[0], cells);
        }
        return result;
    }
    const string Rows = @"
Updates|Mises à jour|Updates|Aggiornamenti|Actualizaciones|Actualizaciones|更新
Pause / Skip movie|Pause / Passer la vidéo|Pause / Video überspringen|Pausa / Salta filmato|Pausa / Saltar vídeo|Pausa / Saltar video|ポーズ／映像スキップ
Secondary command|Commande secondaire|Sekundärer Befehl|Comando secondario|Comando secundario|Comando secundario|サブコマンド
Photo mode|Mode photo|Fotomodus|Modalità foto|Modo foto|Modo foto|フォトモード
Update the game and launcher while keeping your saves.|Mettez à jour le jeu et le lanceur en conservant vos sauvegardes.|Spiel und Launcher aktualisieren, Spielstände behalten.|Aggiorna gioco e launcher conservando i salvataggi.|Actualiza el juego y el lanzador conservando tus partidas.|Actualiza el juego y el launcher conservando tus partidas.|セーブデータを保持してゲームとランチャーを更新します。
Check for updates|Rechercher des mises à jour|Nach Updates suchen|Cerca aggiornamenti|Buscar actualizaciones|Buscar actualizaciones|更新を確認
New version:|Nouvelle version :|Neue Version:|Nuova versione:|Nueva versión:|Nueva versión:|新しいバージョン：
Update and restart|Mettre à jour et relancer|Aktualisieren und neu starten|Aggiorna e riavvia|Actualizar y reiniciar|Actualizar y reiniciar|更新して再起動
Checking for updates…|Recherche de mises à jour…|Updates werden gesucht…|Ricerca aggiornamenti…|Buscando actualizaciones…|Buscando actualizaciones…|更新を確認中…
You have the latest version.|Vous avez la dernière version.|Die neueste Version ist installiert.|Hai la versione più recente.|Tienes la última versión.|Tienes la última versión.|最新のバージョンです。
Update available.|Mise à jour disponible.|Update verfügbar.|Aggiornamento disponibile.|Actualización disponible.|Actualización disponible.|更新があります。
Could not check for updates. Try again later.|Recherche impossible. Réessayez plus tard.|Updatesuche fehlgeschlagen. Später erneut versuchen.|Ricerca non riuscita. Riprova più tardi.|No se pudo buscar actualizaciones. Inténtalo más tarde.|No se pudo buscar actualizaciones. Inténtalo más tarde.|更新を確認できませんでした。後でもう一度お試しください。
Downloading and verifying the update…|Téléchargement et vérification…|Update wird geladen und geprüft…|Download e verifica dell'aggiornamento…|Descargando y verificando la actualización…|Descargando y verificando la actualización…|更新をダウンロードして検証中…
Could not update. Your saves were preserved.|Mise à jour impossible. Sauvegardes conservées.|Update fehlgeschlagen. Spielstände bleiben erhalten.|Aggiornamento non riuscito. Salvataggi conservati.|No se pudo actualizar. Tus partidas se han conservado.|No se pudo actualizar. Tus partidas se conservaron.|更新できませんでした。セーブデータは保持されています。
Mouse sensitivity|Sensibilité de la souris|Mausempfindlichkeit|Sensibilità del mouse|Sensibilidad del ratón|Sensibilidad del mouse|マウス感度
Controller icons|Icônes de manette|Controllersymbole|Icone del controller|Iconos del mando|Iconos del control|コントローラーアイコン
Automatic|Automatique|Automatisch|Automatico|Automático|Automático|自動
Keyboard and mouse are ready. Prompts follow the active input device.|Clavier et souris prêts. Les indications suivent le périphérique utilisé.|Tastatur und Maus sind bereit. Hinweise folgen dem aktiven Eingabegerät.|Tastiera e mouse pronti. I suggerimenti seguono il dispositivo in uso.|Teclado y ratón listos. Los avisos siguen el dispositivo activo.|Teclado y mouse listos. Los avisos siguen el dispositivo activo.|キーボードとマウスに対応。使用中の機器に合わせて操作表示が切り替わります。
Normal attack|Attaque normale|Normaler Angriff|Attacco normale|Ataque normal|Ataque normal|通常攻撃
Powerful attack|Attaque puissante|Starker Angriff|Attacco potente|Ataque potente|Ataque fuerte|強攻撃
Atomic breath|Souffle atomique|Atomarer Atem|Soffio atomico|Aliento atómico|Aliento atómico|熱線
Charge / Confirm|Charge / Confirmer|Ansturm / Bestätigen|Carica / Conferma|Embestida / Confirmar|Embestida / Confirmar|突進／決定
Charge|Charge|Ansturm|Carica|Embestida|Embestida|突進
Turn|Tourner|Drehen|Ruota|Girar|Girar|旋回
Roar|Rugir|Brüllen|Ruggito|Rugir|Rugir|咆哮
Emergency evade|Esquive d'urgence|Notausweichen|Schivata d'emergenza|Evasión de emergencia|Evasión de emergencia|緊急回避
Collect data|Collecter les données|Daten sammeln|Raccogli dati|Recopilar datos|Recopilar datos|データ収集
Pause|Pause|Pause|Pausa|Pausa|Pausa|ポーズ
Confirm|Confirmer|Bestätigen|Conferma|Confirmar|Confirmar|決定
Back|Retour|Zurück|Indietro|Volver|Volver|戻る
Select|Sélectionner|Auswählen|Seleziona|Seleccionar|Seleccionar|選択
Release / capture mouse|Libérer / capturer la souris|Maus freigeben / erfassen|Libera / cattura mouse|Liberar / capturar ratón|Liberar / capturar mouse|マウスの解放／固定
Save PNG photo|Enregistrer une photo PNG|PNG-Foto speichern|Salva foto PNG|Guardar foto PNG|Guardar foto PNG|PNG写真を保存

Automatic (Windows)|Automatique (Windows)|Automatisch (Windows)|Automatico (Windows)|Automático (Windows)|Automático (Windows)|自動（Windows）
Interface language|Langue de l'interface|Sprache der Oberfläche|Lingua dell'interfaccia|Idioma de la interfaz|Idioma de la interfaz|表示言語
Play|Jouer|Spielen|Gioca|Jugar|Jugar|プレイ
Graphics|Graphismes|Grafik|Grafica|Gráficos|Gráficos|グラフィック
Controls|Commandes|Steuerung|Comandi|Controles|Controles|操作方法
Save files|Sauvegardes|Spielstände|Salvataggi|Partidas guardadas|Partidas guardadas|セーブデータ
Logs|Journaux|Protokolle|Registri|Registros|Registros|ログ
About|À propos|Über|Informazioni|Acerca de|Acerca de|このソフトについて
Playing|En jeu|Spiel läuft|In gioco|En juego|En juego|プレイ中
Could not save preferences.|Impossible d'enregistrer les préférences.|Einstellungen konnten nicht gespeichert werden.|Impossibile salvare le preferenze.|No se pudieron guardar las preferencias.|No se pudieron guardar las preferencias.|設定を保存できませんでした。
Ready to play|Prêt à jouer|Bereit zum Spielen|Pronto per giocare|Listo para jugar|Listo para jugar|プレイの準備完了
Game language|Langue du jeu|Spielsprache|Lingua del gioco|Idioma del juego|Idioma del juego|ゲームの言語
Display|Écran|Anzeige|Schermo|Pantalla|Pantalla|画面
Windowed|Fenêtré|Fenstermodus|Finestra|Ventana|Ventana|ウィンドウ
Fullscreen|Plein écran|Vollbild|Schermo intero|Pantalla completa|Pantalla completa|フルスクリーン
Borderless|Sans bordures|Randlos|Senza bordi|Sin bordes|Sin bordes|ボーダーレス
Graphics settings|Réglages graphiques|Grafikeinstellungen|Impostazioni grafiche|Ajustes gráficos|Configuración gráfica|グラフィック設定
Prepared game files were not found in this installation.|Les fichiers préparés du jeu sont introuvables dans cette installation.|Die vorbereiteten Spieldateien wurden in dieser Installation nicht gefunden.|I file preparati del gioco non sono stati trovati in questa installazione.|No se encontraron los archivos preparados del juego en esta instalación.|No se encontraron los archivos preparados del juego en esta instalación.|このインストールに準備済みのゲームファイルが見つかりません。
Display mode|Mode d'affichage|Anzeigemodus|Modalità schermo|Modo de pantalla|Modo de pantalla|画面モード
Output resolution|Résolution de sortie|Ausgabeauflösung|Risoluzione di uscita|Resolución de salida|Resolución de salida|出力解像度
The game's internal rendering resolution remains unchanged.|La résolution de rendu interne du jeu reste inchangée.|Die interne Renderauflösung des Spiels bleibt unverändert.|La risoluzione interna di rendering del gioco resta invariata.|La resolución de renderizado interno del juego no cambia.|La resolución interna de renderizado del juego no cambia.|ゲーム内部の描画解像度は変更されません。
Vertical sync|Synchronisation verticale|Vertikale Synchronisation|Sincronizzazione verticale|Sincronización vertical|Sincronización vertical|垂直同期
On|Activée|Ein|Attiva|Activada|Activada|オン
Off|Désactivée|Aus|Disattiva|Desactivada|Desactivada|オフ
Image upscaling|Mise à l'échelle de l'image|Bildskalierung|Upscaling dell'immagine|Reescalado de imagen|Escalado de imagen|画像のアップスケーリング
Default|Par défaut|Standard|Predefinito|Predeterminado|Predeterminado|標準
RCAS sharpening|Netteté RCAS|RCAS-Schärfung|Nitidezza RCAS|Nitidez RCAS|Nitidez RCAS|RCASシャープニング
Strength|Intensité|Stärke|Intensità|Intensidad|Intensidad|強度
Soft|Douce|Sanft|Leggera|Suave|Suave|弱い
Medium|Modérée|Mittel|Media|Media|Media|中
Strong|Forte|Stark|Forte|Fuerte|Fuerte|強い
Maximum|Maximale|Maximal|Massima|Máxima|Máxima|最大
Gamma|Gamma|Gamma|Gamma|Gamma|Gamma|ガンマ
Reset graphics defaults|Rétablir les réglages graphiques|Grafikeinstellungen zurücksetzen|Ripristina grafica predefinita|Restablecer ajustes gráficos|Restablecer gráficos predeterminados|グラフィック設定を初期化
Controllers are detected automatically.\nFollow the game's button prompts.|Les manettes sont détectées automatiquement.\nSuivez les indications du jeu.|Controller werden automatisch erkannt.\nFolge den Tastenhinweisen im Spiel.|I controller vengono rilevati automaticamente.\nSegui le indicazioni dei tasti nel gioco.|Los mandos se detectan automáticamente.\nSigue las indicaciones de botones del juego.|Los controles se detectan automáticamente.\nSigue las indicaciones de botones del juego.|コントローラーは自動で認識されます。\nゲーム内のボタン表示に従ってください。
Move|Déplacement|Bewegen|Movimento|Mover|Mover|移動
Camera|Caméra|Kamera|Visuale|Cámara|Cámara|カメラ
Arrow keys|Flèches|Pfeiltasten|Frecce|Flechas|Flechas|矢印キー
D-pad|Croix directionnelle|Steuerkreuz|Croce direzionale|Cruceta|Cruceta|方向キー
Space|Espace|Leertaste|Spazio|Espacio|Espacio|スペース
This folder is not available yet.|Ce dossier n'est pas encore disponible.|Dieser Ordner ist noch nicht verfügbar.|Questa cartella non è ancora disponibile.|Esta carpeta aún no está disponible.|Esta carpeta todavía no está disponible.|このフォルダーはまだ利用できません。
Game files not found.|Fichiers du jeu introuvables.|Spieldateien nicht gefunden.|File del gioco non trovati.|No se encontraron los archivos del juego.|No se encontraron los archivos del juego.|ゲームファイルが見つかりません。
The game closed unexpectedly. Its log is available below.|Le jeu s'est fermé de façon inattendue. Son journal est disponible ci-dessous.|Das Spiel wurde unerwartet beendet. Das Protokoll ist unten verfügbar.|Il gioco si è chiuso inaspettatamente. Il registro è disponibile qui sotto.|El juego se cerró inesperadamente. Su registro está disponible abajo.|El juego se cerró inesperadamente. El registro está disponible abajo.|ゲームが予期せず終了しました。以下からログを確認できます。
Could not start. Open Logs.|Échec du démarrage. Consultez les journaux.|Start fehlgeschlagen. Protokolle öffnen.|Avvio non riuscito. Apri i registri.|Error al iniciar. Abre Registros.|No se pudo iniciar. Abre Registros.|起動できませんでした。ログを確認してください。
Based on bbport and shadPS4.|Basé sur bbport et shadPS4.|Basiert auf bbport und shadPS4.|Basato su bbport e shadPS4.|Basado en bbport y shadPS4.|Basado en bbport y shadPS4.|bbportとshadPS4を基にしています。
License GPL-2.0-or-later.|Licence GPL-2.0-or-later.|Lizenz GPL-2.0-or-later.|Licenza GPL-2.0-or-later.|Licencia GPL-2.0-or-later.|Licencia GPL-2.0-or-later.|ライセンス：GPL-2.0-or-later。
Image unavailable|Image indisponible|Bild nicht verfügbar|Immagine non disponibile|Imagen no disponible|Imagen no disponible|画像を表示できません
Collected data|Données collectées|Gesammelte Daten|Dati raccolti|Datos recopilados|Datos recopilados|収集データ
";
}
