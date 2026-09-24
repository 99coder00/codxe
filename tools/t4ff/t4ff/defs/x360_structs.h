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

    // D3D resources embedded in console structures (runtime data, zero in zones).
    struct D3DVertexBuffer360
    {
        unsigned int common;
        unsigned int referenceCount;
        unsigned int fence;
        unsigned int readFence;
        unsigned int identifier;
        unsigned int baseFlush;
        unsigned int format[2];
    };

    struct D3DIndexBuffer360
    {
        unsigned int common;
        unsigned int referenceCount;
        unsigned int fence;
        unsigned int readFence;
        unsigned int identifier;
        unsigned int baseFlush;
        unsigned int address;
        unsigned int size;
    };

    // Per surface bounds (LOD 0) used by console texture streaming.
    struct XModelHighMipBounds
    {
        float mins[3];
        float maxs[3];
    };

    // XMA seek table of console loaded sounds: decoded sample count at the start of every packet.
    struct XmaSeekTable360
    {
        unsigned int unknown;
        unsigned int count;
        unsigned int entries[1];
    };

    // World texture streaming info (empty in converted zones).
    struct GfxWorldStreamInfo360
    {
        int aabbTreeCount;
        unsigned int* aabbTrees;
        int leafRefCount;
        int* leafRefs;
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

    // No static model cache fields in the lod infos on console.
    struct XModelLodInfo
    {
        float dist;
        uint16_t numsurfs;
        uint16_t surfIndex;
        int partBits[4];
    };

    // No baseTriIndex/baseVertIndex; the vertex and index buffers are embedded D3D resources.
    struct XSurface
    {
        char tileMode;
        bool deformed;
        uint16_t vertCount;
        uint16_t triCount;
        char zoneHandle;
        XSurfaceTri16* triIndices;
        XSurfaceVertexInfo vertInfo;
        GfxPackedVertex* verts0;
        D3DVertexBuffer360 vb0;
        unsigned int vertListCount;
        XRigidVertList* vertList;
        D3DIndexBuffer360 indexBuffer;
        int partBits[4];
    };

    struct XModelStreamInfo
    {
        XModelHighMipBounds* highMipBounds;
    };

    // Console model collision surfaces carry no triangles.
    struct XModelCollSurf_s
    {
        float mins[3];
        float maxs[3];
        int boneIdx;
        int contents;
        int surfFlags;
    };

    // Console loaded sounds are XMA2 with a seek table and a format description (being decoded).
    struct snd_asset
    {
        char* data;
        int data_size;
        XmaSeekTable360* seekTable;
        unsigned int format[36];
    };

    // The world has an embedded index buffer, embedded vertex buffers and a larger stream info.
    struct GfxWorldVertexData
    {
        GfxWorldVertex* vertices;
        D3DVertexBuffer360 worldVb;
    };

    struct GfxWorldVertexLayerData
    {
        char* data;
        D3DVertexBuffer360 layerVb;
    };

    struct GfxWorld
    {
        const char* name;
        const char* baseName;
        int planeCount;
        int nodeCount;
        int indexCount;
        uint16_t* indices;
        D3DIndexBuffer360 indexBuffer;
        int surfaceCount;
        GfxWorldStreamInfo360 streamInfo;
        int skySurfCount;
        int* skyStartSurfs;
        GfxImage* skyImage;
        char skySamplerState;
        const char* skyBoxModel;
        unsigned int vertexCount;
        GfxWorldVertexData vd;
        unsigned int vertexLayerDataSize;
        GfxWorldVertexLayerData vld;
        unsigned int vertexStream2DataSize;
        SunLightParseParams sunParse;
        GfxLight* sunLight;
        float sunColorFromBsp[3];
        unsigned int sunPrimaryLightIndex;
        unsigned int primaryLightCount;
        int cullGroupCount;
        unsigned int reflectionProbeCount;
        GfxReflectionProbe* reflectionProbes;
        GfxTexture* reflectionProbeTextures;
        unsigned int coronaCount;
        GfxLightCorona* coronas;
        GfxWorldDpvsPlanes dpvsPlanes;
        int cellBitsCount;
        GfxCell* cells;
        int lightmapCount;
        GfxLightmapArray* lightmaps;
        GfxLightGrid lightGrid;
        GfxTexture* lightmapPrimaryTextures;
        GfxTexture* lightmapSecondaryTextures;
        int modelCount;
        GfxBrushModel* models;
        float mins[3];
        float maxs[3];
        unsigned int checksum;
        int materialMemoryCount;
        MaterialMemory* materialMemory;
        sunflare_t sun;
        float outdoorLookupMatrix[4][4];
        GfxImage* outdoorImage;
        unsigned int* cellCasterBits;
        GfxSceneDynModel4* sceneDynModel;
        GfxSceneDynBrush* sceneDynBrush;
        unsigned int* primaryLightEntityShadowVis;
        unsigned int* primaryLightDynEntShadowVis[2];
        char* nonSunPrimaryLightForModelDynEnt;
        GfxShadowGeometry* shadowGeom;
        GfxLightRegion* lightRegion;
        GfxWorldDpvsStatic dpvs;
        GfxWorldDpvsDynamic dpvsDyn;
        unsigned int worldLodChainCount;
        GfxWorldLodChain* worldLodChains;
        unsigned int worldLodInfoCount;
        GfxWorldLodInfo* worldLodInfos;
        unsigned int worldLodSurfaceCount;
        unsigned int* worldLodSurfaces;
        float waterDirection;
        GfxWaterBuffer waterBuffers[2];
        Material* waterMaterial;
    };

    // Console world surfaces keep a second copy of the bounds before the material.
    struct GfxSurface
    {
        srfTriangles_t tris;
        float boundsCopy[2][3];
        int pad;
        Material* material;
        char lightmapIndex;
        char reflectionProbeIndex;
        char primaryLightIndex;
        char flags;
        float bounds[2][3];
    };

    // Static model placements store the rotation as three DEC3N packed unit vectors (signed 10 bit
    // components scaled by 511 in bits 0-9, 10-19 and 20-29); no lod fade / cached light setting.
    struct GfxStaticModelDrawInst
    {
        float cullDist;
        float origin[3];
        unsigned int axis[3];
        float scale;
        XModel* model;
        char field_44;
        char field_45;
        unsigned char reflectionProbeIndex;
        unsigned char primaryLightIndex;
        int flags;
        unsigned int smodelCacheIndex[4];
    };
