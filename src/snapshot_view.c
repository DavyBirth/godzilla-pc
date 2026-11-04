/* Legacy camera files can contain only zero bytes, including alpha. Display an
 * explicit unavailable marker without changing the file or the collection flags.
 * New captures are fixed by linear GPU image readback, independently of this view.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "gz.h"
#include "snapshot_view.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdio.h>

enum { WIDTH = 480, HEIGHT = 270, IMAGE_BYTES = WIDTH * HEIGHT * 4 };
static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
static unsigned char *placeholder;
static BOOL CALLBACK make_view(PINIT_ONCE unused, void *parameter, void **context) {
    (void)unused;
    (void)context;
    const char *setting = getenv("GZ_LANGUAGE");
    int language = setting ? atoi(setting) : 1;
    /* Local native-lettering marker: fixed dimensions, opaque RGBA, bottom-up.
     * It is a read-only presentation resource, never a replacement save. */
    if (language == 17 && parameter) {
        char path[1024];
        wchar_t wide[1024];
        snprintf(path, sizeof(path), "%s\\patches\\empty-camera.bin", (const char *)parameter);
        if (!gz_widen(path, wide, 1024)) {
            HANDLE source = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL, NULL);
            if (source != INVALID_HANDLE_VALUE) {
                LARGE_INTEGER size;
                unsigned char header[16];
                DWORD got;
                const uint32_t dimensions[2] = {WIDTH, HEIGHT};
                if (GetFileSizeEx(source, &size) && size.QuadPart == 16 + IMAGE_BYTES &&
                    ReadFile(source, header, 16, &got, NULL) && got == 16 &&
                    !memcmp(header, "GZEMPTY1", 8) &&
                    !memcmp(header + 8, dimensions, sizeof(dimensions))) {
                    unsigned char *pixels = malloc(IMAGE_BYTES);
                    if (pixels && ReadFile(source, pixels, IMAGE_BYTES, &got, NULL) &&
                        got == IMAGE_BYTES) {
                        int valid = 1;
                        for (int i = 3; i < IMAGE_BYTES; i += 4)
                            if (pixels[i] != 255) {
                                valid = 0;
                                break;
                            }
                        if (valid)
                            placeholder = pixels;
                        else
                            free(pixels);
                    } else
                        free(pixels);
                }
                CloseHandle(source);
                if (placeholder)
                    return TRUE;
            }
        }
    }
    const wchar_t *title = L"DATA COLLECTED", *detail = L"IMAGE UNAVAILABLE";
    if (language == 17) {
        title = L"DADOS COLETADOS";
        detail = L"IMAGEM INDISPONÍVEL";
    } else if (language == 2) {
        title = L"DONNÉES COLLECTÉES";
        detail = L"IMAGE INDISPONIBLE";
    } else if (language == 3 || language == 20) {
        title = L"DATOS RECOPILADOS";
        detail = L"IMAGEN NO DISPONIBLE";
    } else if (language == 5) {
        title = L"DATEN GESAMMELT";
        detail = L"BILD NICHT VERFÜGBAR";
    } else if (language == 4) {
        title = L"DATI RACCOLTI";
        detail = L"IMMAGINE NON DISPONIBILE";
    } else if (language == 0) {
        title = L"収集データ";
        detail = L"画像を表示できません";
    }
    HDC dc = CreateCompatibleDC(NULL);
    if (!dc)
        return FALSE;
    BITMAPINFO info = {0};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = WIDTH;
    info.bmiHeader.biHeight = -HEIGHT;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP image = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!image) {
        DeleteDC(dc);
        return FALSE;
    }
    HGDIOBJ previous = SelectObject(dc, image);
    RECT full = {0, 0, WIDTH, HEIGHT};
    HBRUSH background = CreateSolidBrush(RGB(23, 29, 40));
    FillRect(dc, &full, background);
    DeleteObject(background);
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(186, 155, 97));
    HGDIOBJ old_pen = SelectObject(dc, pen),
            old_brush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(dc, 3, 3, WIDTH - 3, HEIGHT - 3);
    Rectangle(dc, 210, 43, 270, 86);
    Rectangle(dc, 232, 54, 249, 73);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(pen);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(230, 206, 160));
    HFONT font = CreateFontW(-28, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                             CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HGDIOBJ old_font = SelectObject(dc, font);
    RECT label = {12, 112, WIDTH - 12, 158};
    DrawTextW(dc, title, -1, &label, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, old_font);
    DeleteObject(font);
    font = CreateFontW(-17, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                       0, L"Segoe UI");
    old_font = SelectObject(dc, font);
    SetTextColor(dc, RGB(159, 165, 177));
    label = (RECT){12, 171, WIDTH - 12, 204};
    DrawTextW(dc, detail, -1, &label, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, old_font);
    DeleteObject(font);
    GdiFlush();
    placeholder = malloc(IMAGE_BYTES);
    /* These saved camera textures use bottom-up rows. The DIB above is top-down. */
    if (placeholder)
        for (int i = 0; i < IMAGE_BYTES; i += 4) {
            int pixel_index = i / 4;
            int source = ((HEIGHT - 1 - pixel_index / WIDTH) * WIDTH + pixel_index % WIDTH) * 4;
            const unsigned char *pixel = (const unsigned char *)bits + source;
            placeholder[i] = pixel[2];
            placeholder[i + 1] = pixel[1];
            placeholder[i + 2] = pixel[0];
            placeholder[i + 3] = 255;
        }
    SelectObject(dc, previous);
    DeleteObject(image);
    DeleteDC(dc);
    return placeholder != NULL;
}
const unsigned char *gz_snapshot_view(HANDLE file, const char *path, const char *user_root) {
    const char *disable = getenv("GZ_EMPTY_SNAPSHOT_FALLBACK");
    if (disable && !strcmp(disable, "0"))
        return NULL;
    size_t root_length = strlen(user_root);
    if (_strnicmp(path, user_root, root_length) ||
        _strnicmp(path + root_length, "\\savedata\\", 10))
        return NULL;
    const char *name = strrchr(path, '\\');
    if (!name)
        return NULL;
    ++name;
    if (strlen(name) != 13 || strncmp(name, "area", 4) || name[6] != '_' ||
        strcmp(name + 9, ".dat") || name[4] < '0' || name[4] > '9' || name[5] < '0' ||
        name[5] > '9' || name[7] < '0' || name[7] > '9' || name[8] < '0' || name[8] > '9')
        return NULL;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size) || size.QuadPart != IMAGE_BYTES)
        return NULL;
    unsigned char buffer[16384];
    for (DWORD offset = 0; offset < IMAGE_BYTES;) {
        DWORD count = IMAGE_BYTES - offset;
        if (count > sizeof(buffer))
            count = sizeof(buffer);
        OVERLAPPED at = {0};
        at.Offset = offset;
        DWORD received = 0;
        if (!ReadFile(file, buffer, count, &received, &at) || received != count)
            return NULL;
        for (DWORD i = 0; i < count; ++i)
            if (buffer[i])
                return NULL;
        offset += count;
    }
    if (!InitOnceExecuteOnce(&once, make_view, (void *)user_root, NULL))
        return NULL;
    gz_log("Snapshot: %s contains no pixels; read-only unavailable marker (saved file unchanged)",
           name);
    return placeholder;
}
