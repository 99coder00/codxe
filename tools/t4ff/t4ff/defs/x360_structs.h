// Xbox 360 specific T4 structure definitions.
//
// Every struct/union defined here replaces the definition with the same name
// in OpenAssetTools' T4_Assets.h when computing the Xbox 360 layouts (see
// layout.py). Types between the prelude markers are added to the header.
//
// The definitions were recovered by comparing PC fastfiles against Xbox 360
// fastfiles produced by the CoD Xenon converter.

// @prelude-begin
    // D3DBaseTexture as stored in 360 zones: D3DResource (Common, ReferenceCount, Fence,
    // ReadFence, Identifier, BaseFlush), MipFlush and the GPU texture fetch constant.
    // Note: these 13 dwords are stored little endian in the zone.
    struct D3DBaseTexture360
    {
        unsigned int common;
        unsigned int referenceCount;
        unsigned int fence;
        unsigned int readFence;
        unsigned int identifier;
        unsigned int baseFlush;
        unsigned int mipFlush;
        unsigned int format[6];
    };
// @prelude-end

    struct GfxImageLoadDef
    {
        unsigned char levelCount;
        unsigned char flags;
        uint16_t dimensions[3];
        int format; // D3DFORMAT
        D3DBaseTexture360* texture;
    };

    struct CardMemory
    {
        int platform[1];
    };

    struct GfxImage
    {
        MapType mapType;
        GfxTexture texture;
        unsigned char semantic;
        CardMemory cardMemory;
        uint16_t width;
        uint16_t height;
        uint16_t depth;
        unsigned char category;
        bool delayLoadPixels;
        unsigned char* pixels;
        unsigned int baseSize;
        uint16_t streamSlot;
        bool streaming;
        const char* name;
    };

    struct MaterialInfo
    {
        const char* name;
        unsigned char gameFlags;
        unsigned char sortKey;
        unsigned char textureAtlasRowCount;
        unsigned char textureAtlasColumnCount;
        GfxDrawSurf drawSurf;
        unsigned int surfaceTypeBits;
        unsigned int layeredSurfaceTypes;
    };

    struct Material
    {
        MaterialInfo info;
        unsigned char stateBitsEntry[51];
        unsigned char textureCount;
        unsigned char constantCount;
        unsigned char stateBitsCount;
        unsigned char stateFlags;
        unsigned char cameraRegion;
        MaterialTechniqueSet* techniqueSet;
        MaterialTextureDef* textureTable;
        MaterialConstantDef* constantTable;
        GfxStateBits* stateBitsTable;
    };

    struct MaterialTechniqueSet
    {
        const char* name;
        MaterialWorldVertexFormat worldVertFormat;
        bool hasBeenUploaded;
        char unused[1];
        MaterialTechniqueSet* remappedTechniqueSet;
        MaterialTechnique* techniques[51];
    };

    // Menus keep per local client state for the 4 splitscreen players on console.
    struct windowDef_t
    {
        const char* name;
        rectDef_s rect;
        rectDef_s rectClient;
        const char* group;
        int style;
        int border;
        int ownerDraw;
        int ownerDrawFlags;
        float borderSize;
        unsigned int staticFlags;
        unsigned int dynamicFlags[4];
        int nextTime;
        float foreColor[4];
        float backColor[4];
        float borderColor[4];
        float outlineColor[4];
        Material* background;
    };

    struct itemDef_s
    {
        windowDef_t window;
        rectDef_s textRect[4];
        int type;
        int dataType;
        int alignment;
        int fontEnum;
        int textAlignMode;
        float textalignx;
        float textaligny;
        float textscale;
        int textStyle;
        int gameMsgWindowIndex;
        int gameMsgWindowMode;
        const char* text;
        unsigned int itemFlags;
        menuDef_t* parent;
        const char* mouseEnterText;
        const char* mouseExitText;
        const char* mouseEnter;
        const char* mouseExit;
        const char* action;
        const char* onAccept;
        const char* onFocus;
        const char* leaveFocus;
        const char* dvar;
        const char* dvarTest;
        const char* onListboxSelectionChange;
        ItemKeyHandler* onKey;
        const char* enableDvar;
        int dvarFlags;
        snd_alias_list_t* focusSound;
        float special;
        int cursorPos[4];
        itemDefData_t typeData;
        int imageTrack;
        statement_s visibleExp;
        statement_s textExp;
        statement_s materialExp;
        statement_s rectXExp;
        statement_s rectYExp;
        statement_s rectWExp;
        statement_s rectHExp;
        statement_s forecolorAExp;
    };

    struct menuDef_t
    {
        windowDef_t window;
        const char* font;
        int fullScreen;
        int itemCount;
        int fontIndex;
        int cursorItem[4];
        int fadeCycle;
        float fadeClamp;
        float fadeAmount;
        float fadeInAmount;
        float blurRadius;
        const char* onOpen;
        const char* onFocus;
        const char* onClose;
        const char* onESC;
        ItemKeyHandler* onKey;
        statement_s visibleExp;
        const char* allowedBinding;
        const char* soundName;
        int imageTrack;
        float focusColor[4];
        float disableColor[4];
        statement_s rectXExp;
        statement_s rectYExp;
        itemDef_s** items;
    };

    // Xenos shaders are stored as a cached part (constant/metadata, virtual memory) and a physical
    // part (microcode, physical memory). There is no runtime shader pointer in the zone.
    struct GfxPixelShaderLoadDef
    {
        unsigned int* cachedPart;
        unsigned int* physicalPart;
        uint16_t cachedPartSize;
        uint16_t physicalPartSize;
    };

    struct MaterialPixelShaderProgram
    {
        GfxPixelShaderLoadDef loadDef;
    };

    struct GfxVertexShaderLoadDef
    {
        unsigned int* cachedPart;
        unsigned int* physicalPart;
        uint16_t cachedPartSize;
        uint16_t physicalPartSize;
    };

    struct MaterialVertexShaderProgram
    {
        GfxVertexShaderLoadDef loadDef;
    };

    struct MaterialVertexStreamRouting
    {
        MaterialStreamRouting data[16];
        void* decl[8];
    };

    // One vertex shader per vertex format (generic, packed, world, ...).
    struct MaterialPass
    {
        MaterialVertexDeclaration* vertexDecl;
        MaterialVertexShader* vertexShaderArray[17];
        MaterialPixelShader* pixelShader;
        unsigned char perPrimArgCount;
        unsigned char perObjArgCount;
        unsigned char stableArgCount;
        unsigned char customSamplerFlags;
        unsigned int unknown;
        MaterialShaderArgument* args;
    };

    struct listBoxDef_s
    {
        int mousePos;
        int startPos[4];
        int endPos[4];
        // no drawPadding on console (36 bytes precede elementWidth)
        float elementWidth;
        float elementHeight;
        int elementStyle;
        int numColumns;
        columnInfo_s columnInfo[16];
        const char* onDoubleClick;
        int notselectable;
        int noScrollBars;
        int usePaging;
        float selectBorder[4];
        float disableColor[4];
        float focusColor[4];
        Material* selectIcon;
        Material* backgroundItemListbox;
        Material* highlightTexture;
    };

    // Console animations have 12 part types (6 rotation, 5 translation, all): PART_TYPE_ALL is 11.
    struct XAnimParts
    {
        const char* name;
        uint16_t dataByteCount;
        uint16_t dataShortCount;
        uint16_t dataIntCount;
        uint16_t randomDataByteCount;
        uint16_t randomDataIntCount;
        uint16_t numframes;
        bool bLoop;
        bool bDelta;
        unsigned char boneCount[12];
        unsigned char notifyCount;
        unsigned char assetType;
        bool isDefault;
        unsigned int randomDataShortCount;
        unsigned int indexCount;
        float framerate;
        float frequency;
        ScriptString* names;
        unsigned char* dataByte;
        int16_t* dataShort;
        int* dataInt;
        int16_t* randomDataShort;
        unsigned char* randomDataByte;
        int* randomDataInt;
        XAnimIndices indices;
        XAnimNotifyInfo* notify;
        XAnimDeltaPart* deltaPart;
    };

    // UNVERIFIED: only reference (name only) models were available. The 360 XModel is 16 bytes
    // smaller than on PC; the PC only static model cache fields of the lod infos are assumed missing.
    struct XModelLodInfo
    {
        float dist;
        uint16_t numsurfs;
        uint16_t surfIndex;
        int partBits[4];
    };
