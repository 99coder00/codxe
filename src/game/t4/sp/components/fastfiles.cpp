#include "pch.h"
#include "fastfiles.h"

namespace t4
{
namespace sp
{
namespace
{
const char *const CODXE_ZONE_DIRECTORY = "zone";
const char *const USERMAPS_DIRECTORY = "usermaps";
const char *const SOUND_REQUEST_PREFIX = "D:\\sounds\\";
const char *const HIGHMIP_REQUEST_PREFIX = "D:\\highmip";
const char *const IMAGES_PAK_FILE = "images.pak";
const unsigned int IMAGES_PAK_VERSION = 2;
const unsigned int IMAGES_PAK_VERSION_EIGHTH = 3; // with IMAGES_PAK_EIGHTH entries
const unsigned int IMAGES_PAK_MAX_INDEX = 16 * 1024 * 1024;

char activeUsermap[64] = "";

Detour DB_LoadXAssets_Detour;
Detour Sys_CreateFile_Detour;

struct ExpandedXAssetPool
{
    XAssetType type;
    void *originalPool;
    int originalSize;
    void *expandedPool;
};

ExpandedXAssetPool expandedXAssetPools[3] = {};
unsigned int expandedXAssetPoolCount = 0;

bool EndsWithIgnoreCase(const std::string &value, const char *suffix)
{
    const size_t suffixLength = std::strlen(suffix);
    return value.length() >= suffixLength && _stricmp(value.c_str() + value.length() - suffixLength, suffix) == 0;
}

bool IsSafeZoneName(const char *name)
{
    if (!name || !*name || std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0)
        return false;

    for (const char *cursor = name; *cursor; ++cursor)
    {
        const unsigned char c = static_cast<unsigned char>(*cursor);
        if (c < ' ' || c == '/' || c == '\\' || c == ':')
            return false;
    }

    return true;
}

bool IsSafeRelativePath(const char *path)
{
    if (!path || !*path || *path == '\\' || *path == '/')
        return false;

    const char *component = path;
    for (const char *cursor = path;; ++cursor)
    {
        const unsigned char c = static_cast<unsigned char>(*cursor);
        if (c == ':' || (c && c < ' '))
            return false;

        if (!c || c == '\\' || c == '/')
        {
            const size_t componentLength = static_cast<size_t>(cursor - component);
            if (!componentLength || (componentLength == 1 && component[0] == '.') ||
                (componentLength == 2 && component[0] == '.' && component[1] == '.'))
            {
                return false;
            }

            if (!c)
                return true;

            component = cursor + 1;
        }
    }
}

std::string GetUsermapName(const char *zoneName)
{
    if (!IsSafeZoneName(zoneName))
        return std::string();

    std::string usermapName = zoneName;
    if (EndsWithIgnoreCase(usermapName, "_load"))
        usermapName.erase(usermapName.length() - 5);
    else if (EndsWithIgnoreCase(usermapName, "_patch"))
        usermapName.erase(usermapName.length() - 6);

    return usermapName;
}

std::string GetUsermapFastfilePath(const char *zoneName, const char *rootDirectory)
{
    const std::string usermapName = GetUsermapName(zoneName);
    if (usermapName.empty())
        return std::string();

    const std::string usermapDirectory = filesystem::JoinPath(rootDirectory, usermapName.c_str());
    const std::string filename = std::string(zoneName) + ".ff";
    return filesystem::JoinPath(usermapDirectory.c_str(), filename.c_str());
}

std::string GetZoneNameFromFastfileRequest(const char *filename)
{
    static const char extension[] = ".ff";

    if (!filename || !*filename)
        return std::string();

    const char *basename = filename;
    for (const char *cursor = filename; *cursor; ++cursor)
    {
        if (*cursor == '\\' || *cursor == '/')
            basename = cursor + 1;
    }

    std::string zoneName = basename;
    if (!EndsWithIgnoreCase(zoneName, extension))
        return std::string();

    zoneName.erase(zoneName.length() - (sizeof(extension) - 1));
    return IsSafeZoneName(zoneName.c_str()) ? zoneName : std::string();
}

std::string GetZoneFastfilePath(const char *zoneName, const char *rootDirectory)
{
    if (!IsSafeZoneName(zoneName))
        return std::string();

    return filesystem::JoinPath(rootDirectory, (std::string(zoneName) + ".ff").c_str());
}

std::string ResolveFastfilePath(const char *filename)
{
    const std::string zoneName = GetZoneNameFromFastfileRequest(filename);
    if (zoneName.empty())
        return std::string();

    // Only redirect the file open. The database keeps the requested zone's stock name, flags and lifetime.
    // Future active-mod replacements belong here, before usermap and global replacements.

    const std::string usermapPath = GetUsermapFastfilePath(zoneName.c_str(), USERMAPS_DIRECTORY);
    if (!usermapPath.empty())
    {
        const std::string resolved = Config::ResolveDataPathForGameFile(usermapPath.c_str());
        if (!resolved.empty())
            return resolved;
    }

    const std::string codxeZonePath = GetZoneFastfilePath(zoneName.c_str(), CODXE_ZONE_DIRECTORY);
    if (!codxeZonePath.empty())
        return Config::ResolveDataPathForGameFile(codxeZonePath.c_str());

    return std::string();
}

bool UsermapExists(const char *usermapName)
{
    const std::string path = GetUsermapFastfilePath(usermapName, USERMAPS_DIRECTORY);
    return !path.empty() && !Config::ResolveDataPath(path.c_str()).empty();
}

void SetActiveUsermap(const std::string &usermapName)
{
    if (_stricmp(activeUsermap, usermapName.c_str()) == 0)
        return;

    _snprintf_s(activeUsermap, sizeof(activeUsermap), _TRUNCATE, "%s", usermapName.c_str());
}

void ClearActiveUsermap()
{
    if (!activeUsermap[0])
        return;

    activeUsermap[0] = '\0';
}

void UpdateActiveUsermap(const XZoneInfo *zoneInfo, unsigned int zoneCount)
{
    bool containsLoadscreenZone = false;

    for (unsigned int i = 0; i < zoneCount; ++i)
    {
        const std::string usermapName = GetUsermapName(zoneInfo[i].name);
        if (usermapName.empty())
            continue;

        if (EndsWithIgnoreCase(zoneInfo[i].name, "_load"))
            containsLoadscreenZone = true;

        if (UsermapExists(usermapName.c_str()))
        {
            SetActiveUsermap(usermapName);
            return;
        }
    }

    // A new stock loadscreen marks the transition away from the previous custom map.
    if (containsLoadscreenZone)
        ClearActiveUsermap();
}

bool RequiresPriorityOverride(const char *name)
{
    // Loadscreen zones use a temporary link-time priority below; changing their allocation group breaks unloading.
    return name && !EndsWithIgnoreCase(name, "_load") && UsermapExists(name);
}

std::string ResolveLooseSoundPath(const char *filename)
{
    if (!filename || !activeUsermap[0] ||
        _strnicmp(filename, SOUND_REQUEST_PREFIX, std::strlen(SOUND_REQUEST_PREFIX)) != 0)
    {
        return std::string();
    }

    const char *relativeSoundPath = filename + std::strlen(SOUND_REQUEST_PREFIX);
    if (!IsSafeRelativePath(relativeSoundPath))
        return std::string();

    const std::string usermapDirectory = filesystem::JoinPath(USERMAPS_DIRECTORY, activeUsermap);
    const std::string soundDirectory = filesystem::JoinPath(usermapDirectory.c_str(), "sounds");
    const std::string soundPath = filesystem::JoinPath(soundDirectory.c_str(), relativeSoundPath);
    return Config::ResolveDataPathForGameFile(soundPath.c_str());
}

// The image streamer reads a streamed image's top mip level from "D:\highmip[\h0]\<image>.hi" when
// a model using it is drawn close by (Title Update 7's path; the disc executable's has no subfolder). A
// custom map's own (t4ff writes them next to its fastfile) are in its folder's highmip folder, which
// keeps the game's files untouched; images the map has none of stay the disc's. Only the game's request
// is matched, never the path it is redirected to (the existence check opens files too).
std::string ResolveHighmipPath(const char *filename)
{
    const size_t prefixLength = std::strlen(HIGHMIP_REQUEST_PREFIX);
    if (!filename || !activeUsermap[0] || _strnicmp(filename, HIGHMIP_REQUEST_PREFIX, prefixLength) != 0 ||
        !EndsWithIgnoreCase(filename, ".hi"))
    {
        return std::string();
    }

    const char *imageFile = std::strrchr(filename, '\\') + 1;
    if (!IsSafeRelativePath(imageFile))
        return std::string();

    const std::string usermapDirectory = filesystem::JoinPath(USERMAPS_DIRECTORY, activeUsermap);
    const std::string highmipDirectory = filesystem::JoinPath(usermapDirectory.c_str(), "highmip");
    const std::string highmipPath = filesystem::JoinPath(highmipDirectory.c_str(), imageFile);
    const std::string resolved = Config::ResolveDataPathForGameFile(highmipPath.c_str());
    if (!resolved.empty())
        DbgPrint("[codxe][T4 SP][FastFiles] highmip: %s\n", resolved.c_str());
    return resolved;
}

// A custom map's highmip files in one pack, usermaps\<map>\images.pak (t4ff's PakWriter, big endian):
// a 32 byte header (magic "T4FFPAK1", version, entry count, index offset, index size), the entries'
// data at 4 KiB aligned offsets (the game reads them unbuffered), then the index: per entry (24
// bytes) the name's offset from the index's start, its length, flags, the data's offset and size,
// the offset of its level 1 in the data and of its level 2 from there (deep entries: the whole
// texture, see Streaming); then the names (lower case image names, without ".hi"). Its index is
// read when the map first needs it.
typedef ImagesPakImage ImagesPakEntry;

CRITICAL_SECTION imagesPakLock;

char imagesPakUsermap[64] = "";
std::string imagesPakGamePath; // the pack as the game opens it, empty when the map has none
std::map<std::string, ImagesPakEntry> imagesPakEntries;

unsigned int ReadBigEndian32(const unsigned char *bytes)
{
    return (static_cast<unsigned int>(bytes[0]) << 24) | (static_cast<unsigned int>(bytes[1]) << 16) |
           (static_cast<unsigned int>(bytes[2]) << 8) | bytes[3];
}

unsigned int ReadBigEndian16(const unsigned char *bytes)
{
    return (static_cast<unsigned int>(bytes[0]) << 8) | bytes[1];
}

void LoadImagesPak()
{
    if (std::strcmp(imagesPakUsermap, activeUsermap) == 0)
        return;

    strncpy(imagesPakUsermap, activeUsermap, sizeof(imagesPakUsermap) - 1);
    imagesPakUsermap[sizeof(imagesPakUsermap) - 1] = '\0';
    imagesPakGamePath.clear();
    imagesPakEntries.clear();

    const std::string usermapDirectory = filesystem::JoinPath(USERMAPS_DIRECTORY, activeUsermap);
    const std::string relativePath = filesystem::JoinPath(usermapDirectory.c_str(), IMAGES_PAK_FILE);
    const std::string path = Config::ResolveDataPath(relativePath.c_str());
    if (path.empty())
        return;

    HANDLE file =
        CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;

    unsigned char header[32];
    DWORD bytesRead = 0;
    std::vector<unsigned char> index;
    if (ReadFile(file, header, sizeof(header), &bytesRead, nullptr) && bytesRead == sizeof(header) &&
        std::memcmp(header, "T4FFPAK1", 8) == 0 &&
        (ReadBigEndian32(header + 8) == IMAGES_PAK_VERSION || ReadBigEndian32(header + 8) == IMAGES_PAK_VERSION_EIGHTH))
    {
        const unsigned int count = ReadBigEndian32(header + 12);
        const unsigned int indexOffset = ReadBigEndian32(header + 16);
        const unsigned int indexSize = ReadBigEndian32(header + 20);
        if (indexSize >= count * 24 && indexSize <= IMAGES_PAK_MAX_INDEX &&
            SetFilePointer(file, static_cast<LONG>(indexOffset), nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER)
        {
            index.resize(indexSize);
            if (!ReadFile(file, &index[0], indexSize, &bytesRead, nullptr) || bytesRead != indexSize)
                index.clear();
        }
        for (unsigned int i = 0; !index.empty() && i < count; ++i)
        {
            const unsigned char *entry = &index[i * 24];
            const unsigned int nameOffset = ReadBigEndian32(entry);
            const unsigned int nameLength = ReadBigEndian16(entry + 4);
            if (nameOffset + nameLength > indexSize)
                continue;
            ImagesPakEntry value = {ReadBigEndian32(entry + 8), ReadBigEndian32(entry + 12), ReadBigEndian16(entry + 6),
                                    ReadBigEndian32(entry + 16), ReadBigEndian32(entry + 20)};
            imagesPakEntries[std::string(reinterpret_cast<const char *>(&index[nameOffset]), nameLength)] = value;
        }
    }
    CloseHandle(file);

    if (!imagesPakEntries.empty())
        imagesPakGamePath = Config::ResolveDataPathForGameFile(relativePath.c_str());
    DbgPrint("[codxe][T4 SP][FastFiles] images.pak: %u entries in %s\n", static_cast<unsigned int>(imagesPakEntries.size()),
             path.c_str());
}

// The pack's entry of an image (copied: the index changes with the map), if the map has one.
bool FindImagesPakEntry(std::string name, ImagesPakEntry *entry)
{
    if (!activeUsermap[0])
        return false;

    std::transform(name.begin(), name.end(), name.begin(), ::tolower);
    EnterCriticalSection(&imagesPakLock);
    LoadImagesPak();
    const std::map<std::string, ImagesPakEntry>::const_iterator found = imagesPakEntries.find(name);
    const bool exists = found != imagesPakEntries.end();
    if (exists)
        *entry = found->second;
    LeaveCriticalSection(&imagesPakLock);
    return exists;
}

// The pack's entry for a highmip request ("D:\highmip[\h0]\<image>.hi"), if the map has one.
bool FindImagesPakRequest(const char *filename, ImagesPakEntry *entry)
{
    const size_t prefixLength = std::strlen(HIGHMIP_REQUEST_PREFIX);
    if (!filename || !activeUsermap[0] || _strnicmp(filename, HIGHMIP_REQUEST_PREFIX, prefixLength) != 0 ||
        !EndsWithIgnoreCase(filename, ".hi"))
    {
        return false;
    }

    std::string name = std::strrchr(filename, '\\') + 1;
    name.erase(name.length() - 3);
    return FindImagesPakEntry(name, entry);
}

// Title Update 7's Sys_CreateFile is the CRT's CreateFileA: every file the game opens passes here.
int Sys_CreateFile_Hook(const char *filename, int desiredAccess, int shareMode, int securityAttributes,
                        int creationDisposition, int flagsAndAttributes)
{
    // a highmip file in the map's pack: the pack, at the entry (the streamer reads from there)
    ImagesPakEntry pakEntry;
    if (FindImagesPakRequest(filename, &pakEntry))
    {
        EnterCriticalSection(&imagesPakLock);
        const std::string pakPath = imagesPakGamePath;
        LeaveCriticalSection(&imagesPakLock);
        const int handle = Sys_CreateFile_Detour.GetOriginal<Sys_CreateFile_t>()(
            pakPath.c_str(), desiredAccess, shareMode | FILE_SHARE_READ, securityAttributes, creationDisposition,
            flagsAndAttributes);
        if (handle != -1 && SetFilePointer(reinterpret_cast<HANDLE>(handle), static_cast<LONG>(pakEntry.offset), nullptr,
                                           FILE_BEGIN) != INVALID_SET_FILE_POINTER)
        {
            DbgPrint("[codxe][T4 SP][FastFiles] highmip: %s from images.pak (%u bytes at %u)\n",
                     std::strrchr(filename, '\\') + 1, pakEntry.size, pakEntry.offset);
            return handle;
        }
        if (handle != -1)
            CloseHandle(reinterpret_cast<HANDLE>(handle));
    }

    std::string redirectedPath = ResolveFastfilePath(filename);
    if (redirectedPath.empty())
        redirectedPath = ResolveLooseSoundPath(filename);
    if (redirectedPath.empty())
        redirectedPath = ResolveHighmipPath(filename);
    if (!redirectedPath.empty())
        filename = redirectedPath.c_str();

    return Sys_CreateFile_Detour.GetOriginal<Sys_CreateFile_t>()(filename, desiredAccess, shareMode, securityAttributes,
                                                                 creationDisposition, flagsAndAttributes);
}

void DB_LoadXAssets_Hook(XZoneInfo *zoneInfo, unsigned int zoneCount, int sync)
{
    UpdateActiveUsermap(zoneInfo, zoneCount);

    for (unsigned int i = 0; i < zoneCount; ++i)
    {
        if (!RequiresPriorityOverride(zoneInfo[i].name))
            continue;

        // Preserve the map's normal allocation group while giving its intentional overrides priority.
        zoneInfo[i].allocFlags |= DB_ZONE_DEV;

        // Complete delayed asset clones before script compilation starts.
        sync = 1;
    }

    DB_LoadXAssets_Detour.GetOriginal<DB_LoadXAssets_t>()(zoneInfo, zoneCount, sync);
}

void DB_ReallocXAssetPool(XAssetType type, unsigned int newSize)
{
    if (*g_assetPoolsInitialized)
    {
        DbgPrint("[codxe][T4 SP][FastFiles] Cannot expand initialized %s asset pool\n", g_assetNames[type]);
        return;
    }

    const unsigned int oldSize = static_cast<unsigned int>(g_poolSize[type]);
    if (newSize <= oldSize)
        return;

    void *pool = malloc(newSize * DB_GetXAssetTypeSize(type));
    if (!pool)
    {
        DbgPrint("[codxe][T4 SP][FastFiles] Failed to expand %s asset pool from %u to %u entries\n", g_assetNames[type],
                 oldSize, newSize);
        return;
    }

    if (expandedXAssetPoolCount == ARRAYSIZE(expandedXAssetPools))
    {
        DbgPrint("[codxe][T4 SP][FastFiles] Cannot track expanded %s asset pool\n", g_assetNames[type]);
        free(pool);
        return;
    }

    ExpandedXAssetPool &expandedPool = expandedXAssetPools[expandedXAssetPoolCount++];
    expandedPool.type = type;
    expandedPool.originalPool = DB_XAssetPool[type];
    expandedPool.originalSize = g_poolSize[type];
    expandedPool.expandedPool = pool;

    DB_XAssetPool[type] = expandedPool.expandedPool;
    g_poolSize[type] = newSize;
}

void RestoreExpandedXAssetPools()
{
    while (expandedXAssetPoolCount)
    {
        ExpandedXAssetPool &expandedPool = expandedXAssetPools[--expandedXAssetPoolCount];
        DB_XAssetPool[expandedPool.type] = expandedPool.originalPool;
        g_poolSize[expandedPool.type] = expandedPool.originalSize;
        free(expandedPool.expandedPool);
        memset(&expandedPool, 0, sizeof(expandedPool));
    }
}
} // namespace

// With a file named "dump_executable" in the data folder, the running executable's image (Title
// Update 7 under Xenia, whose code differs from the disc's) is written next to it for analysis.
void DumpExecutableImage()
{
    const std::string marker = Config::ResolveDataPath("dump_executable");
    if (marker.empty())
        return;

    const unsigned char *base = reinterpret_cast<const unsigned char *>(0x82000000);
    if (base[0] != 'M' || base[1] != 'Z')
    {
        DbgPrint("[codxe][T4 SP] dump_executable: no image header at 82000000\n");
        return;
    }
    const auto le32 = [base](unsigned int offset) {
        return static_cast<unsigned int>(base[offset]) | (static_cast<unsigned int>(base[offset + 1]) << 8) |
               (static_cast<unsigned int>(base[offset + 2]) << 16) | (static_cast<unsigned int>(base[offset + 3]) << 24);
    };
    const unsigned int ntHeaders = le32(0x3C);
    if (ntHeaders > 0x1000 || base[ntHeaders] != 'P' || base[ntHeaders + 1] != 'E')
    {
        DbgPrint("[codxe][T4 SP] dump_executable: no PE header\n");
        return;
    }
    const unsigned int sizeOfImage = le32(ntHeaders + 24 + 56);
    const std::string path = marker + ".bin";
    FILE *file = fopen(path.c_str(), "wb");
    if (!file)
    {
        DbgPrint("[codxe][T4 SP] dump_executable: cannot create %s\n", path.c_str());
        return;
    }
    // in chunks through a buffer of our own: one write of the whole image fails
    static char chunk[0x10000];
    unsigned int written = 0;
    for (unsigned int offset = 0; offset < sizeOfImage; offset += sizeof(chunk))
    {
        const unsigned int size = min(static_cast<unsigned int>(sizeof(chunk)), sizeOfImage - offset);
        memcpy(chunk, base + offset, size);
        if (fwrite(chunk, 1, size, file) != size)
            break;
        written += size;
    }
    fclose(file);
    DbgPrint("[codxe][T4 SP] dump_executable: wrote %u of %u bytes to %s\n", written, sizeOfImage, path.c_str());
}

bool GetImagesPakImage(const char *imageName, ImagesPakImage *image)
{
    return imageName && FindImagesPakEntry(imageName, image);
}

std::string GetImagesPakPath()
{
    EnterCriticalSection(&imagesPakLock);
    const std::string path = imagesPakGamePath;
    LeaveCriticalSection(&imagesPakLock);
    return path;
}

FastFiles::FastFiles()
{
    DumpExecutableImage();

    // The DLL remains resident across title launches, so explicitly reset title-lifetime state.
    activeUsermap[0] = '\0';
    InitializeCriticalSection(&imagesPakLock);
    imagesPakUsermap[0] = '\0';
    imagesPakGamePath.clear();
    imagesPakEntries.clear();
    expandedXAssetPoolCount = 0;

    DB_ReallocXAssetPool(ASSET_TYPE_MENULIST, 192); // Stock: 128
    DB_ReallocXAssetPool(ASSET_TYPE_MENU, 800);     // Stock: 600
    DB_ReallocXAssetPool(ASSET_TYPE_FX, 600);       // Stock: 400

    Sys_CreateFile_Detour = Detour(Sys_CreateFile, Sys_CreateFile_Hook);
    Sys_CreateFile_Detour.Install();

    DB_LoadXAssets_Detour = Detour(DB_LoadXAssets, DB_LoadXAssets_Hook);
    DB_LoadXAssets_Detour.Install();
}

FastFiles::~FastFiles()
{
    DB_LoadXAssets_Detour.Remove();
    Sys_CreateFile_Detour.Remove();
    DeleteCriticalSection(&imagesPakLock);

    RestoreExpandedXAssetPools();

    activeUsermap[0] = '\0';
}
} // namespace sp
} // namespace t4
