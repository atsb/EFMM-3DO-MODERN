#ifndef EFMM_SDL3_3DO_H
#define EFMM_SDL3_3DO_H

#include <SDL3/SDL.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifdef __cplusplus
extern "C"
{
#endif

    typedef int8_t int8;
    typedef int16_t int16;
    typedef int32_t int32;
    typedef int64_t int64;
    typedef uint8_t uint8;
    typedef uint16_t uint16;
    typedef uint32_t uint32;
    typedef uint64_t uint64;
    typedef uint8 ubyte;
    typedef int32 Item;
    typedef int32 Err;
    typedef int32 Coord;
    typedef uint8 CelData;
    typedef uint16 Pixel;

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#define MEMTYPE_DRAM 0x00000001u
#define MEMTYPE_VRAM 0x00000002u
#define MEMTYPE_CEL 0x00000004u
#define MEMTYPE_DMA 0x00000008u
#define MEMTYPE_FILL 0x00000010u

    /* 3DO compatible CCB. */
    typedef struct CCB
    {
        uint32 ccb_Flags;
        struct CCB *ccb_NextPtr;
        CelData *ccb_SourcePtr;
        void *ccb_PLUTPtr;
        Coord ccb_XPos;
        Coord ccb_YPos;
        int32 ccb_HDX, ccb_HDY;
        int32 ccb_VDX, ccb_VDY;
        int32 ccb_HDDX, ccb_HDDY;
        uint32 ccb_PIXC;
        uint32 ccb_PRE0, ccb_PRE1;
        int32 ccb_Width, ccb_Height;
        int32 ccb_SDLFade16;
        int32 ccb_SDLAssetBE;
        int32 ccb_SDLPLUT32Pairs;
        int32 ccb_SDLSourceHasPreamble;
    } CCB;

    typedef struct Point
    {
        int32 pt_X, pt_Y;
    } Point;
    typedef struct Rect
    {
        union
        {
            int32 r_XMin;
            int32 rect_XLeft;
        };
        union
        {
            int32 r_YMin;
            int32 rect_YTop;
        };
        union
        {
            int32 r_XMax;
            int32 rect_XRight;
        };
        union
        {
            int32 r_YMax;
            int32 rect_YBottom;
        };
    } Rect;

    typedef struct Bitmap
    {
        Item bm_nItem;
        struct
        {
            Item n_Item;
        } bm;
        int32 bm_Width, bm_Height, bm_ClipWidth, bm_ClipHeight;
        uint32 *bm_Buffer;
        uint32 bm_CEControl;
    } Bitmap;
    typedef struct Screen
    {
        Item scr_nItem;
        Bitmap *scr_TempBitmap;
    } Screen;

    typedef struct GrafCon
    {
        int32 gc_Pen;
        int32 gc_Flags;
    } GrafCon;
    typedef struct FontChar
    {
        uint32 dummy;
    } FontChar;
    typedef struct Font
    {
        uint8 font_Height;
        uint8 font_Flags;
        CCB *font_CCB;
        FontChar *font_FontEntries;
    } Font;

    typedef struct TagArg
    {
        uint32 ta_Tag;
        void *ta_Arg;
    } TagArg;
    typedef struct IOBuf
    {
        void *iob_Buffer;
        int32 iob_Len;
    } IOBuf;
    typedef struct IOInfo
    {
        uint32 ioi_Flags, ioi_Flags2;
        uint32 ioi_Unit, ioi_Command;
        int32 ioi_Offset;
        IOBuf ioi_Recv, ioi_Send;
    } IOInfo;
    typedef struct IOReq
    {
        int32 io_Error;
    } IOReq;
    typedef struct Stream
    {
        FILE *fp;
        void *vfs;
        int32 kind;
        int32 st_FileLength;
    } Stream;
    typedef struct FileStatus
    {
        int32 fs_ByteCount;
    } FileStatus;
    typedef struct Node
    {
        Item n_Item;
    } Node;
    typedef struct List
    {
        Node *l_Head;
        Node *l_Tail;
    } List;
    typedef struct MemList
    {
        int32 ml_NumNodes;
    } MemList;
    typedef struct Task
    {
        Node t;
        struct Task *t_ThreadTask;
    } Task;
    typedef struct KernelBaseType
    {
        uint32 kb_CPUFlags;
        Task *kb_CurrentTask;
    } KernelBaseType;
    typedef struct FileFolio
    {
        void *ff_Data;
    } FileFolio;
    typedef struct BitmapItems
    {
        Item dummy;
    } BitmapItems;

    extern KernelBaseType *KernelBase;
    typedef struct GrafBaseType
    {
        uint32 gf_VBLNumber;
        uint32 gf_VRAMPageSize;
        uint32 gf_ZeroPage;
    } GrafBaseType;
    extern GrafBaseType *GrafBase;

#define MKNODEID(a, b) 0
#define GETBANKBITS(x) (0)
#define KERNELNODE 1
#define DEVICENODE 2
#define IOREQNODE 3
#define TAG_END 0
#define TAG_NOP 0
#define TAG_ITEM 1
#define CREATEIOREQ_TAG_DEVICE 2
#define CSG_TAG_SPORTBITS 3
#define CSG_TAG_SCREENCOUNT 4
#define CSG_TAG_DONE 5
#define LC_FocusListener 0
#define TIMER_UNIT_USEC 1
#define TIMERCMD_DELAY 2
#define CMD_READ 3
#define CMD_WRITE 4
#define CMD_STATUS 5
#define VBLANK 1
#define KB_RED 1
#define KB_REDWW 2
#define KB_GREEN 4
#define KB_GREENWW 8

#define ControlA (1u << 0)
#define ControlB (1u << 1)
#define ControlC (1u << 2)
#define ControlX (1u << 3)
#define ControlStart (1u << 4)
#define ControlLeftShift (1u << 5)
#define ControlRightShift (1u << 6)
#define ControlLeft (1u << 7)
#define ControlRight (1u << 8)
#define ControlUp (1u << 9)
#define ControlDown (1u << 10)

    typedef struct ControlPadEventData
    {
        uint32 cped_ButtonBits;
    } ControlPadEventData;

#define FONT_ASCII 1
#define FONT_ASCII_UPPERCASE 2
#define ONE_12_20 (1 << 20)
#define ONE_16_16 (1 << 16)
#define PMODE_PDC 0x00000000u
#define PMODE_ZERO 0x00000100u
#define PMODE_ONE 0x00000180u

    /* 3DO CCB/preamble/PIXC bit definitions. */
#define CCB_SKIP 0x80000000u
#define CCB_LAST 0x40000000u
#define CCB_NPABS 0x20000000u
#define CCB_SPABS 0x10000000u
#define CCB_PPABS 0x08000000u
#define CCB_LDSIZE 0x04000000u
#define CCB_LDPRS 0x02000000u
#define CCB_LDPPMP 0x01000000u
#define CCB_LDPLUT 0x00800000u
#define CCB_CCBPRE 0x00400000u
#define CCB_YOXY 0x00200000u
#define CCB_ACSC 0x00100000u
#define CCB_ALSC 0x00080000u
#define CCB_ACW 0x00040000u
#define CCB_ACCW 0x00020000u
#define CCB_TWD 0x00010000u
#define CCB_LCE 0x00008000u
#define CCB_ACE 0x00004000u
#define CCB_MARIA 0x00001000u
#define CCB_PXOR 0x00000800u
#define CCB_USEAV 0x00000400u
#define CCB_PACKED 0x00000200u
#define CCB_PLUTPOS 0x00000040u
#define CCB_BGND 0x00000020u
#define CCB_NOBLK 0x00000010u
#define CCB_PLUTA_MASK 0x0000000Fu
#define CCB_POVER_MASK 0x00000180u
#define CCB_POVER_SHIFT 7
#define CCB_NXTPTRTYPE 0x20000000u
#define CCB_SRCPTRTYPE 0x10000000u
#define CCB_PLUTPTRTYPE 0x08000000u

#define PRE0_LITERAL 0x80000000u
#define PRE0_BGND 0x40000000u
#define PRE0_SKIPX_MASK 0x0F000000u
#define PRE0_VCNT_MASK 0x0000FFC0u
#define PRE0_LINEAR 0x00000010u
#define PRE0_REP8 0x00000008u
#define PRE0_BPP_MASK 0x00000007u
#define PRE0_BPP_1 0x00000001u
#define PRE0_BPP_2 0x00000002u
#define PRE0_BPP_4 0x00000003u
#define PRE0_BPP_6 0x00000004u
#define PRE0_BPP_8 0x00000005u
#define PRE0_BPP_16 0x00000006u
#define PRE0_VCNT_SHIFT 6
#define PRE0_VCNT_PREFETCH 1
#define PRE1_WOFFSET8_MASK 0xFF000000u
#define PRE1_WOFFSET10_MASK 0x03FF0000u
#define PRE1_NOSWAP 0x00004000u
#define PRE1_TLLSB_MASK 0x00003000u
#define PRE1_TLLSB_PDC0 0x00001000u
#define PRE1_TLLSB_PDC4 0x00002000u
#define PRE1_TLLSB_PDC5 0x00003000u
#define PRE1_LRFORM 0x00000800u
#define PRE1_TLHPCNT_MASK 0x000007FFu
#define PRE1_TLLSB_SHIFT 12
#define PRE1_WOFFSET10_SHIFT 16
#define PRE1_WOFFSET8_SHIFT 24
#define PRE1_TLHPCNT_SHIFT 0
#define PRE1_TLHPCNT_PREFETCH 1
#define PRE1_WOFFSET_PREFETCH 2
#define PRE1_TLLSB_ZERO 0x00000000u

#define PPMP_MODE_NORMAL 0
#define PPMP_MODE_AVERAGE 1
#define PPMP_0_SHIFT 0
#define PPMP_1_SHIFT 16
#define PPMPC_MODE_AVERAGE 1u
#define PPMPC_1S_MASK 0x00008000u
#define PPMPC_MS_MASK 0x00006000u
#define PPMPC_MF_MASK 0x00001C00u
#define PPMPC_SF_MASK 0x00000300u
#define PPMPC_2S_MASK 0x000000C0u
#define PPMPC_AV_MASK 0x0000003Eu
#define PPMPC_2D_MASK 0x00000001u
#define PPMPC_MS_SHIFT 13
#define PPMPC_MF_SHIFT 10
#define PPMPC_SF_SHIFT 8
#define PPMPC_2S_SHIFT 6
#define PPMPC_AV_SHIFT 1
#define PPMPC_1S_PDC 0x00000000u
#define PPMPC_1S_CFBD 0x00008000u
#define PPMPC_MS_CCB 0x00000000u
#define PPMPC_MS_PIN 0x00002000u
#define PPMPC_MS_PDC_MFONLY 0x00006000u
#define PPMPC_MS_PDC 0x00004000u
#define PPMPC_MF_1 0x00000000u
#define PPMPC_MF_2 0x00000400u
#define PPMPC_MF_3 0x00000800u
#define PPMPC_MF_4 0x00000C00u
#define PPMPC_MF_5 0x00001000u
#define PPMPC_MF_6 0x00001400u
#define PPMPC_MF_7 0x00001800u
#define PPMPC_MF_8 0x00001C00u
#define PPMPC_SF_16 0x00000000u
#define PPMPC_SF_2 0x00000100u
#define PPMPC_SF_4 0x00000200u
#define PPMPC_SF_8 0x00000300u
#define PPMPC_2S_0 0x00000000u
#define PPMPC_2S_CCB 0x00000040u
#define PPMPC_2S_CFBD 0x00000080u
#define PPMPC_2S_PDC 0x000000C0u
#define PPMPC_2D_1 0x00000000u
#define PPMPC_2D_2 0x00000001u

#define CHUNK_ANIM 0x414e494du /* ANIM */
#define CHUNK_CCB 0x43434220u  /* CCB  */
#define CHUNK_PLUT 0x504c5554u /* PLUT */
#define CHUNK_PDAT 0x50444154u /* PDAT */
#define CHUNK_IMAG 0x494d4147u
#define CHUNK_PIPT 0x50495054u
#define CHUNK_SPRH 0x53505248u

    typedef struct CCC
    {
        uint32 chunkType;
        uint32 chunkSize;
        CCB ccb;
    } CCC;
    typedef struct PLUTChunk
    {
        uint32 chunkType;
        uint32 chunkSize;
        uint32 numentries;
        uint16 PLUT[1];
    } PLUTChunk;
    typedef struct AnimChunk
    {
        uint32 chunkType;
        uint32 chunkSize;
        uint32 numFrames;
        uint32 animType;
    } AnimChunk;

#define CheckErr(x) (x)
#define CHECKRESULT(x, y) ((void)(x))

    /* 3DO colour helpers */
    static inline uint16 MakeRGB15(int r, int g, int b)
    {
        if (r < 0)
            r = 0;
        if (r > 31)
            r = 31;
        if (g < 0)
            g = 0;
        if (g > 31)
            g = 31;
        if (b < 0)
            b = 0;
        if (b > 31)
            b = 31;
        return (uint16)((r << 10) | (g << 5) | b);
    }
    static inline uint32 MakeRGB15Pair(int r, int g, int b)
    {
        uint16 c = MakeRGB15(r, g, b);
        return ((uint32)c << 16) | c;
    }
    static inline uint32 MakeCLUTColorEntry(int idx, int r, int g, int b)
    {
        (void)idx;
        return MakeRGB15(r >> 3, g >> 3, b >> 3);
    }

    /* fixed-point operamath replacements */
    static inline int32 Convert32_F16(int32 x) { return x << 16; }
    static inline int32 ConvertF16_32(int32 x) { return x >> 16; }
    static inline int32 MulSF16(int32 a, int32 b) { return (int32)(((int64)a * (int64)b) >> 16); }
    static inline int32 DivSF16(int32 a, int32 b) { return b ? (int32)(((int64)a << 16) / b) : 0; }
    static inline int32 SquareSF16(int32 a) { return MulSF16(a, a); }
    static inline int32 SinF16(int32 a)
    {
        double rad = ((double)a / 65536.0) * (2.0 * M_PI / 256.0);
        return (int32)llround(sin(rad) * 65536.0);
    }
    static inline int32 CosF16(int32 a)
    {
        double rad = ((double)a / 65536.0) * (2.0 * M_PI / 256.0);
        return (int32)llround(cos(rad) * 65536.0);
    }
    static inline int32 Atan2F16(int32 x, int32 z)
    {
        double a = atan2((double)z / 65536.0, (double)x / 65536.0);
        double u = a * (256.0 / (2.0 * M_PI));
        return (int32)llround(u * 65536.0);
    }

    static inline void MulManyVec3Mat33_F16(void *dstv, const void *srcv, const void *matp, int32 n)
    {
        const int32 *s = (const int32 *)srcv, *m = (const int32 *)matp;
        int32 *d = (int32 *)dstv;
        int32 i;
        for (i = 0; i < n; i++)
        {
            int64 x = s[0], y = s[1], z = s[2];
            d[0] = (int32)((x * m[0] + y * m[3] + z * m[6]) >> 16);
            d[1] = (int32)((x * m[1] + y * m[4] + z * m[7]) >> 16);
            d[2] = (int32)((x * m[2] + y * m[5] + z * m[8]) >> 16);
            s += 3;
            d += 3;
        }
    }
    static inline void MulMat33Mat33_F16(void *dstp, const void *ap, const void *bp)
    {
        const int32 *a = (const int32 *)ap, *b = (const int32 *)bp;
        int32 *d = (int32 *)dstp;
        int i, j, k;
        int64 v;
        for (i = 0; i < 3; i++)
            for (j = 0; j < 3; j++)
            {
                v = 0;
                for (k = 0; k < 3; k++)
                    v += (int64)a[i * 3 + k] * b[k * 3 + j];
                d[i * 3 + j] = (int32)(v >> 16);
            }
    }

    void *AllocMem(int32 size, uint32 typebits);
    void FreeMem(void *ptr, int32 size);
#define ALLOCMEM(sz, t) AllocMem((sz), (t))
#define FREEMEM(p, sz) FreeMem((p), (sz))

    int32 kprintf(const char *fmt, ...);
    void PrintfSysErr(int32 err);

    Item FindNamedItem(int node, const char *name);
    Item OpenItem(Item item, uint32 tags);
    Item CreateItem(int node, TagArg *tags);
    void *LookupItem(Item item);
    Item CreateThread(const char *name, int prio, void (*func)(void), int stack);
    void DeleteThread(Item item);
    int32 DoIO(Item ioItem, IOInfo *ioi);
    void WaitVBL(Item vblIO, int n);
    Item GetVBLIOReq(void);
    Item GetVRAMIOReq(void);
    int32 OpenGraphicsFolio(void);
    int32 OpenMathFolio(void);
    int32 OpenAudioFolio(void);
    void CloseAudioFolio(void);
    void DisplayScreen(Item screenItem, int waitvbl);
    void SetVRAMPages(Item io, void *buf, uint32 val, int32 pages, uint32 mask);
    void CopyVRAMPages(Item io, void *dst, void *src, int32 pages, uint32 mask);
    int32 DrawCels(Item bitmapItem, CCB *ccb);
    int32 CreateScreenGroup(Item *items, TagArg *tags);
    void DeleteScreenGroup(Item item);
    void SetFGPen(GrafCon *gc, int32 pen);
    void MoveTo(GrafCon *gc, int32 x, int32 y);
    void DrawText8(GrafCon *gc, Item bitmapItem, const char *text);
    void FillRect(Item bitmapItem, GrafCon *gc, Rect *r);
    int32 WritePixel(Item bitmapItem, GrafCon *gc, int32 x, int32 y);
    int32 ReadPixel(Item bitmapItem, GrafCon *gc, int32 x, int32 y);
    void SetScreenColor(Item screen, int32 entry);
    void ResetScreenColors(Item screen);
    void EnableHAVG(Item screen);
    void EnableVAVG(Item screen);
    void SetCEControl(Item bitmap, uint32 x);
    void InitFileFolioGlue(void);
    void ChangeDirectory(const char *path);
    int32 GetDirectory(char *buf, int32 len);
    int32 CreateAlias(const char *name, const char *path);

    Stream *OpenDiskStream(const char *path, int mode);
    int32 ReadDiskStream(Stream *s, void *buf, int32 len);
    int32 CloseDiskStream(Stream *s);
    void SDL3_3DO_SetImagePath(const char *path);
    int SDL3_3DO_MountImage(const char *path);
    int SDL3_3DO_MountImageAuto(void);
    void SDL3_3DO_UnmountImage(void);
    FILE *OpenDiskFile(const char *path, int mode);
    int32 ReadDiskFile(FILE *f, void *buf, int32 len);
    int32 CloseDiskFile(FILE *f);

    void InitEventUtility(int a, int b, int c);
    int GetControlPad(int pad, int wait, ControlPadEventData *cped);

    typedef struct FontStruct FontStruct;
    void SetCurrentFont(Font *font);
    void ResetCurrentFont(void);
    Font *GetCurrentFont(void);
    void SetCurrentFontCCB(CCB *ccb);

    /* Optional platform entry points used by the converted source. */
    int SDL3_3DO_Init(int w, int h, const char *title);
    void SDL3_3DO_Shutdown(void);
    void SDL3_3DO_PumpInput(void);
    void SDL3_3DO_FrameSync(void);
    void SDL3_3DO_AudioPump(void);
    int SDL3_3DO_StartMovieAudio(const uint8 *data, size_t size);
    int SDL3_3DO_IsMovieAudioPlaying(void);
    void SDL3_3DO_StopMovieAudio(void);
    uint64 SDL3_3DO_GetMovieAudioDurationUs(void);
    uint32 SDL3_3DO_GetInputBits(void);
    int32 SDL3_3DO_GetMouseTurnF16(void);
    void SDL3_3DO_AdvanceInput(int32 frames);
    void SDL3_3DO_SetFade(Item screen, int32 level16);
    void SDL3_3DO_Present(Item screen);
    Bitmap *SDL3_3DO_GetBitmap(Item bitmapItem);
    Bitmap *SDL3_3DO_GetScreenBitmap(Item screenItem);
    void SDL3_3DO_BlitText(Item bitmapItem, int x, int y, const char *text, uint16 color);
    int SDL3_3DO_RunSmokeTest(void);
    int SDL3_3DO_LoadFile(const char *path, uint8 **data, size_t *size);
    void SDL3_3DO_SetFrame(Bitmap *bm, uint32 *pixels, int pitch);

#ifdef __cplusplus
}
#endif
#endif
