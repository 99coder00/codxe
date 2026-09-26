#include "pch.h"
#include "usermaps.h"

#include "image/xenos_texture.h"

#include <algorithm>
#include <cstdlib>

namespace t4
{
namespace sp
{
namespace
{
// Must match the menu (tools/t4ff/t4ff/menu.py): the rows it shows and the dvars it reads and sets.
const int LIST_ROWS = 13;
const char *const LIST_MENU = "levels_unlock";
const char *const USERMAPS_DIRECTORY = "usermaps";

// ui_codxe_map<row>, ui_codxe_mapcmd<row>: the name and the command of each row ("" hides the row).
// ui_codxe_mapoffset, ui_codxe_mapmore: the list's position (the scroll catchers show when there is more to see).
// ui_codxe_maprange: "14-26 / 40". ui_codxe_maptitle, ui_codxe_mapdesc, ui_codxe_mapimage: the focused map.
// The menu sets ui_codxe_focus (the focused row) and ui_codxe_scroll (rows to scroll, reset here).
const char *const DVAR_FOCUS = "ui_codxe_focus";
const char *const DVAR_SCROLL = "ui_codxe_scroll";

// The picture of a map without one in the menu zone: its preview.bin (written by t4ff, a texture tiled for the
// console) is copied into this image of the menu zone, whose material the preview shows.
const char *const PREVIEW_SLOT = "codxe_map_preview";
const char *const PREVIEW_FILE = "preview.bin";

// preview.bin: this header (big endian), then the texture data.
struct PreviewHeader
{
    char magic[4]; // "CXPV"
    uint16_t version;
    uint16_t width;
    uint16_t height;
    uint16_t reserved;
    uint32_t format; // GPUTEXTUREFORMAT
    uint32_t size;
};
static_assert(sizeof(PreviewHeader) == 20, "");

// Played from their own menu rows, never listed twice.
const char *const STOCK_MAPS[] = {"nazi_zombie_prototype", "nazi_zombie_asylum", "nazi_zombie_sumpf",
                                  "nazi_zombie_factory"};

struct UsermapEntry
{
    std::string name;
    std::string directory;
    std::string displayName;
    std::string description;
    std::string image;
};

std::vector<UsermapEntry> usermaps;
int listOffset = 0;
int focusedRow = -1;
bool dvarsCreated = false;
std::string previewInSlot; // the map whose preview.bin the slot holds

std::string Trim(const std::string &text)
{
    size_t first = 0;
    while (first < text.length() && static_cast<unsigned char>(text[first]) <= ' ')
        ++first;

    size_t last = text.length();
    while (last > first && static_cast<unsigned char>(text[last - 1]) <= ' ')
        --last;

    return text.substr(first, last - first);
}

// Lines of a text file, trimmed, empty ones dropped.
std::vector<std::string> ReadLines(const std::string &path)
{
    std::vector<std::string> lines;
    if (!filesystem::FileExists(path.c_str()))
        return lines;

    const std::string content = filesystem::ReadFileToString(path);
    size_t start = 0;
    while (start <= content.length())
    {
        size_t end = content.find_first_of("\r\n", start);
        if (end == std::string::npos)
            end = content.length();

        const std::string line = Trim(content.substr(start, end - start));
        if (!line.empty())
            lines.push_back(line);

        start = end + 1;
    }

    return lines;
}

// "nazi_zombie_wh" -> "Nazi Zombie Wh", for maps without a description.txt.
std::string PrettyName(const std::string &name)
{
    std::string pretty = name;
    bool wordStart = true;
    for (size_t i = 0; i < pretty.length(); ++i)
    {
        if (pretty[i] == '_')
        {
            pretty[i] = ' ';
            wordStart = true;
        }
        else
        {
            if (wordStart && pretty[i] >= 'a' && pretty[i] <= 'z')
                pretty[i] = static_cast<char>(pretty[i] - 'a' + 'A');
            wordStart = false;
        }
    }

    return pretty;
}

bool IsStockMap(const std::string &name)
{
    for (size_t i = 0; i < ARRAYSIZE(STOCK_MAPS); ++i)
    {
        if (_stricmp(name.c_str(), STOCK_MAPS[i]) == 0)
            return true;
    }

    return false;
}

// Menu text: no quotes (the dvars are also set through the command buffer) and no line breaks.
std::string MenuText(const std::string &text)
{
    std::string clean = text;
    for (size_t i = 0; i < clean.length(); ++i)
    {
        if (clean[i] == '"')
            clean[i] = '\'';
        else if (clean[i] == '\r' || clean[i] == '\n')
            clean[i] = ' ';
    }

    return clean;
}

void ScanUsermaps()
{
    usermaps.clear();

    const std::string directory = Config::ResolveDataDirectory(USERMAPS_DIRECTORY);
    if (directory.empty())
    {
        DbgPrint("[codxe][T4 SP][UsermapList] No usermaps folder\n");
        return;
    }

    WIN32_FIND_DATAA findData;
    HANDLE findHandle = FindFirstFileA(filesystem::JoinPath(directory.c_str(), "*").c_str(), &findData);
    if (findHandle == INVALID_HANDLE_VALUE)
        return;

    do
    {
        const std::string name = findData.cFileName;
        if (name == "." || name == ".." || (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            IsStockMap(name))
        {
            continue;
        }

        const std::string mapDirectory = filesystem::JoinPath(directory.c_str(), name.c_str());
        const std::string fastfile = filesystem::JoinPath(mapDirectory.c_str(), (name + ".ff").c_str());
        if (!filesystem::FileExists(fastfile.c_str()))
            continue;

        // description.txt: the map's name on the first line, its description on the next ones.
        // preview.txt: a material the menu zone has (CoD Xenon's map pictures), for the preview.
        UsermapEntry entry;
        entry.name = name;
        entry.directory = mapDirectory;
        const std::vector<std::string> description =
            ReadLines(filesystem::JoinPath(mapDirectory.c_str(), "description.txt"));
        entry.displayName = MenuText(description.empty() ? PrettyName(name) : description[0]);
        for (size_t i = 1; i < description.size(); ++i)
            entry.description += (i > 1 ? " " : "") + MenuText(description[i]);

        const std::vector<std::string> preview = ReadLines(filesystem::JoinPath(mapDirectory.c_str(), "preview.txt"));
        if (!preview.empty())
            entry.image = MenuText(preview[0]);

        usermaps.push_back(entry);
    } while (FindNextFileA(findHandle, &findData) != 0);

    FindClose(findHandle);

    std::sort(usermaps.begin(), usermaps.end(), [](const UsermapEntry &left, const UsermapEntry &right)
              { return _stricmp(left.displayName.c_str(), right.displayName.c_str()) < 0; });

    DbgPrint("[codxe][T4 SP][UsermapList] Found %u usermap(s) in %s\n", static_cast<unsigned int>(usermaps.size()),
             directory.c_str());
}

int MaxOffset()
{
    const int count = static_cast<int>(usermaps.size());
    return count > LIST_ROWS ? count - LIST_ROWS : 0;
}

void SetDvar(const char *name, const std::string &value)
{
    Dvar_SetFromStringByName(name, value.c_str());

    // The first time, also through the command buffer: "set" creates the dvars the menu reads.
    if (!dvarsCreated)
    {
        const std::string command = std::string("set ") + name + " \"" + value + "\"\n";
        Cbuf_AddText(0, command.c_str());
    }
}

// Copy the map's preview.bin into the preview slot of the menu zone. Only a file made for this very image (its
// size and texture format) is copied.
bool LoadPreview(const UsermapEntry &entry)
{
    if (_stricmp(previewInSlot.c_str(), entry.name.c_str()) == 0)
        return true;

    const std::string path = filesystem::JoinPath(entry.directory.c_str(), PREVIEW_FILE);
    if (!filesystem::FileExists(path.c_str()))
        return false;

    GfxImage *image = DB_FindXAssetHeader(ASSET_TYPE_IMAGE, PREVIEW_SLOT, false, 0).image;
    if (!image || !image->name || _stricmp(image->name, PREVIEW_SLOT) != 0 || !image->texture.basemap)
    {
        DbgPrint("[codxe][T4 SP][UsermapList] The menu has no %s image (update the menu with t4ff)\n", PREVIEW_SLOT);
        return false;
    }

    const std::string data = filesystem::ReadFileToString(path);
    if (data.size() < sizeof(PreviewHeader))
        return false;

    PreviewHeader header;
    memcpy(&header, data.data(), sizeof(header));
    const uint32_t format = static_cast<uint32_t>(image->texture.basemap->Format.DataFormat);
    if (memcmp(header.magic, "CXPV", 4) != 0 || header.width != image->width || header.height != image->height ||
        header.format != format || header.size != image->baseSize || data.size() != sizeof(header) + header.size ||
        (image->cardMemory.platform[0] > 0 && header.size > static_cast<uint32_t>(image->cardMemory.platform[0])))
    {
        DbgPrint("[codxe][T4 SP][UsermapList] %s does not fit the %s image\n", path.c_str(), PREVIEW_SLOT);
        return false;
    }

    unsigned char *pixels = image::xenos_texture::GetTextureBase(image->texture.basemap, image->pixels);
    if (!pixels)
        return false;

    memcpy(pixels, data.data() + sizeof(header), header.size);
    previewInSlot = entry.name;
    return true;
}

void PublishPreview()
{
    const int index = listOffset + focusedRow;
    const bool valid = focusedRow >= 0 && index >= 0 && index < static_cast<int>(usermaps.size());
    std::string picture;
    if (valid)
    {
        // A picture of the menu zone (CoD Xenon's maps, preview.txt), else the map's own preview.bin.
        picture = usermaps[index].image;
        if (picture.empty() && LoadPreview(usermaps[index]))
            picture = PREVIEW_SLOT;
    }

    SetDvar("ui_codxe_maptitle", valid ? usermaps[index].displayName : "");
    SetDvar("ui_codxe_mapdesc", valid ? usermaps[index].description : "");
    SetDvar("ui_codxe_mapimage", picture);
}

void PublishRows()
{
    char dvarName[32];
    const int count = static_cast<int>(usermaps.size());
    for (int row = 0; row < LIST_ROWS; ++row)
    {
        const int index = listOffset + row;
        const bool valid = index < count;

        _snprintf_s(dvarName, ARRAYSIZE(dvarName), _TRUNCATE, "ui_codxe_map%d", row);
        SetDvar(dvarName, valid ? usermaps[index].displayName : "");

        _snprintf_s(dvarName, ARRAYSIZE(dvarName), _TRUNCATE, "ui_codxe_mapcmd%d", row);
        SetDvar(dvarName, valid ? "devmap " + usermaps[index].name : "");
    }

    char value[64];
    _snprintf_s(value, ARRAYSIZE(value), _TRUNCATE, "%d", listOffset);
    SetDvar("ui_codxe_mapoffset", value);
    SetDvar("ui_codxe_mapmore", listOffset < MaxOffset() ? "1" : "0");

    value[0] = '\0';
    if (count > LIST_ROWS)
    {
        const int last = listOffset + LIST_ROWS < count ? listOffset + LIST_ROWS : count;
        _snprintf_s(value, ARRAYSIZE(value), _TRUNCATE, "%d-%d / %d", listOffset + 1, last, count);
    }
    SetDvar("ui_codxe_maprange", value);

    PublishPreview();
    dvarsCreated = true;
}

void Scroll(int delta)
{
    int offset = listOffset + delta;
    if (offset > MaxOffset())
        offset = MaxOffset();
    if (offset < 0)
        offset = 0;

    if (offset == listOffset)
        return;

    listOffset = offset;
    PublishRows();
}
} // namespace

void UsermapList::OnMenuOpen(const char *menuName)
{
    if (!menuName || _stricmp(menuName, LIST_MENU) != 0)
        return;

    // Maps copied onto the drive since the last visit show up without restarting.
    ScanUsermaps();
    previewInSlot.clear();
    listOffset = 0;
    focusedRow = -1;
    SetDvar(DVAR_SCROLL, "0");
    SetDvar(DVAR_FOCUS, "");
    PublishRows();
}

void UsermapList::OnUIRefresh()
{
    if (!dvarsCreated)
        return;

    const char *scroll = Dvar_GetVariantString(DVAR_SCROLL);
    const int delta = scroll ? std::atoi(scroll) : 0;
    if (delta)
    {
        Dvar_SetFromStringByName(DVAR_SCROLL, "0");
        Scroll(delta);
    }

    const char *focus = Dvar_GetVariantString(DVAR_FOCUS);
    const int row = focus && *focus ? std::atoi(focus) : -1;
    if (row != focusedRow)
    {
        focusedRow = row;
        PublishPreview();
    }
}

UsermapList::UsermapList()
{
    // The DLL remains resident across title launches, so explicitly reset title-lifetime state.
    usermaps.clear();
    listOffset = 0;
    focusedRow = -1;
    dvarsCreated = false;
    previewInSlot.clear();
}

UsermapList::~UsermapList()
{
    usermaps.clear();
}
} // namespace sp
} // namespace t4
