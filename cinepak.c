#include "sdl3_3do.h"
#include "castle.h"
#include "app_proto.h"

#define CINE_MAX_WIDTH 320u
#define CINE_MAX_HEIGHT 240u
#define CINE_BLOCK_SIZE 4u
#define CINE_STREAM_HEADER_SIZE 20u
#define CINE_CHUNK_HEADER_SIZE 24u
#define CINE_FRAME_HEADER_SIZE 16u
#define CINE_STRIP_HEADER_SIZE 12u
#define CINE_FRAME_FLAG_SEPARATE_STRIP_CODEBOOKS 0x01u
#define CINE_KEYFRAME_V4_CODEBOOK 0x2000u
#define CINE_DELTAFRAME_V4_CODEBOOK 0x2100u
#define CINE_KEYFRAME_V1_CODEBOOK 0x2200u
#define CINE_DELTAFRAME_V1_CODEBOOK 0x2300u
#define CINE_KEYFRAME_VECTORS 0x3000u
#define CINE_DELTAFRAME_VECTORS 0x3100u
#define CINE_KEYFRAME_V1_VECTORS 0x3200u

typedef struct CineReader
{
    const uint8 *data;
    size_t size;
    size_t position;
} CineReader;

typedef struct CineVector
{
    uint8 y0;
    uint8 y1;
    uint8 y2;
    uint8 y3;
    int8 u;
    int8 v;
} CineVector;

typedef struct CineCodebook
{
    CineVector vectors[256];
} CineCodebook;

typedef struct CineDecoder
{
    uint8 *movieData;
    size_t movieDataSize;
    size_t frameOffset;
    uint32 frameNumber;
    uint32 frameCount;
    CineCodebook codebooks[2][2];
    uint32 width;
    uint32 height;
    uint32 blockWidth;
    uint32 blockHeight;
    uint32 blockCount;
    uint32 stripBlockWidth;
    uint32 stripOriginX;
    uint32 stripOriginY;
    uint32 activeStrip;
    uint32 frameRate;
    uint32 pixels[CINE_MAX_WIDTH * CINE_MAX_HEIGHT];
} CineDecoder;

static uint16 cine_be16(const uint8 *p)
{
    return (uint16)(((uint16)p[0] << 8) | p[1]);
}

static uint32 cine_be32(const uint8 *p)
{
    return ((uint32)p[0] << 24) | ((uint32)p[1] << 16) |
           ((uint32)p[2] << 8) | p[3];
}

static int cine_read_u8(CineReader *reader, uint8 *value)
{
    if (!reader || !value || reader->position >= reader->size)
        return 0;
    *value = reader->data[reader->position++];
    return 1;
}

static int cine_read_be16(CineReader *reader, uint16 *value)
{
    if (!reader || !value || reader->position > reader->size ||
        reader->size - reader->position < 2u)
        return 0;
    *value = cine_be16(reader->data + reader->position);
    reader->position += 2u;
    return 1;
}

static int cine_read_be32(CineReader *reader, uint32 *value)
{
    if (!reader || !value || reader->position > reader->size ||
        reader->size - reader->position < 4u)
        return 0;
    *value = cine_be32(reader->data + reader->position);
    reader->position += 4u;
    return 1;
}

static int cine_read_vector(CineReader *reader, CineVector *vector)
{
    uint8 value;
    if (!vector)
        return 0;
    if (!cine_read_u8(reader, &vector->y0) ||
        !cine_read_u8(reader, &vector->y1) ||
        !cine_read_u8(reader, &vector->y2) ||
        !cine_read_u8(reader, &vector->y3) ||
        !cine_read_u8(reader, &value))
        return 0;
    vector->u = (int8)value;
    if (!cine_read_u8(reader, &value))
        return 0;
    vector->v = (int8)value;
    return 1;
}

static uint32 cine_yuv_to_argb(uint8 yValue, int8 uValue, int8 vValue)
{
    int32 y = (int32)yValue;
    int32 u = (int32)uValue;
    int32 v = (int32)vValue;
    int32 red = y + v * 2;
    int32 green = y - u / 2 - v;
    int32 blue = y + u * 2;
    if (red < 0)
        red = 0;
    else if (red > 255)
        red = 255;
    if (green < 0)
        green = 0;
    else if (green > 255)
        green = 255;
    if (blue < 0)
        blue = 0;
    else if (blue > 255)
        blue = 255;
    return 0xFF000000u | ((uint32)red << 16) | ((uint32)green << 8) | (uint32)blue;
}

static void cine_decode_block(CineDecoder *decoder, uint32 blockIndex,
                              const CineCodebook *codebook, uint8 v0Index, uint8 v1Index,
                              uint8 v2Index, uint8 v3Index)
{
    uint32 blockX = decoder->stripOriginX +
                    (blockIndex % decoder->stripBlockWidth) * CINE_BLOCK_SIZE;
    uint32 blockY = decoder->stripOriginY +
                    (blockIndex / decoder->stripBlockWidth) * CINE_BLOCK_SIZE;
    CineVector v0 = codebook->vectors[v0Index];
    CineVector v1 = codebook->vectors[v1Index];
    CineVector v2 = codebook->vectors[v2Index];
    CineVector v3 = codebook->vectors[v3Index];
    uint32 *row0 = decoder->pixels + (blockY + 0u) * CINE_MAX_WIDTH + blockX;
    uint32 *row1 = decoder->pixels + (blockY + 1u) * CINE_MAX_WIDTH + blockX;
    uint32 *row2 = decoder->pixels + (blockY + 2u) * CINE_MAX_WIDTH + blockX;
    uint32 *row3 = decoder->pixels + (blockY + 3u) * CINE_MAX_WIDTH + blockX;
    row0[0] = cine_yuv_to_argb(v0.y0, v0.u, v0.v);
    row0[1] = cine_yuv_to_argb(v0.y1, v0.u, v0.v);
    row0[2] = cine_yuv_to_argb(v1.y0, v1.u, v1.v);
    row0[3] = cine_yuv_to_argb(v1.y1, v1.u, v1.v);
    row1[0] = cine_yuv_to_argb(v0.y2, v0.u, v0.v);
    row1[1] = cine_yuv_to_argb(v0.y3, v0.u, v0.v);
    row1[2] = cine_yuv_to_argb(v1.y2, v1.u, v1.v);
    row1[3] = cine_yuv_to_argb(v1.y3, v1.u, v1.v);
    row2[0] = cine_yuv_to_argb(v2.y0, v2.u, v2.v);
    row2[1] = cine_yuv_to_argb(v2.y1, v2.u, v2.v);
    row2[2] = cine_yuv_to_argb(v3.y0, v3.u, v3.v);
    row2[3] = cine_yuv_to_argb(v3.y1, v3.u, v3.v);
    row3[0] = cine_yuv_to_argb(v2.y2, v2.u, v2.v);
    row3[1] = cine_yuv_to_argb(v2.y3, v2.u, v2.v);
    row3[2] = cine_yuv_to_argb(v3.y2, v3.u, v3.v);
    row3[3] = cine_yuv_to_argb(v3.y3, v3.u, v3.v);
}

static int cine_decode_codebook(CineReader *reader, CineCodebook *codebook, int delta)
{
    uint32 startIndex;
    if (!delta)
    {
        uint32 entryCount = (uint32)(reader->size - reader->position) / 6u;
        if (entryCount > 256u)
            return 0;
        for (startIndex = 0; startIndex < entryCount; ++startIndex)
            if (!cine_read_vector(reader, &codebook->vectors[startIndex]))
                return 0;
        return 1;
    }
    if (reader->position == reader->size)
        return 1;
    for (startIndex = 0; startIndex < 256u; startIndex += 32u)
    {
        uint32 updateFlags;
        uint32 vectorIndex;
        if (!cine_read_be32(reader, &updateFlags))
            return 0;
        for (vectorIndex = startIndex; vectorIndex < startIndex + 32u; ++vectorIndex)
        {
            if (updateFlags & 0x80000000u)
                if (!cine_read_vector(reader, &codebook->vectors[vectorIndex]))
                    return 0;
            updateFlags <<= 1;
        }
    }
    return 1;
}

static int cine_decode_keyframe_vectors(CineDecoder *decoder, CineReader *reader)
{
    uint32 batchStart;
    for (batchStart = 0; batchStart < decoder->blockCount; batchStart += 32u)
    {
        uint32 flags;
        uint32 blockIndex;
        uint32 endBlock = batchStart + 32u;
        if (endBlock > decoder->blockCount)
            endBlock = decoder->blockCount;
        if (!cine_read_be32(reader, &flags))
            return 0;
        for (blockIndex = batchStart; blockIndex < endBlock; ++blockIndex)
        {
            if (flags & 0x80000000u)
            {
                uint8 v0, v1, v2, v3;
                if (!cine_read_u8(reader, &v0) || !cine_read_u8(reader, &v1) ||
                    !cine_read_u8(reader, &v2) || !cine_read_u8(reader, &v3))
                    return 0;
                cine_decode_block(decoder, blockIndex, &decoder->codebooks[decoder->activeStrip][1], v0, v1, v2, v3);
            }
            else
            {
                uint8 vectorIndex;
                if (!cine_read_u8(reader, &vectorIndex))
                    return 0;
                cine_decode_block(decoder, blockIndex, &decoder->codebooks[decoder->activeStrip][0], vectorIndex,
                                  vectorIndex, vectorIndex, vectorIndex);
            }
            flags <<= 1;
        }
    }
    return 1;
}

static int cine_decode_keyframe_v1_vectors(CineDecoder *decoder, CineReader *reader)
{
    uint32 blockIndex;
    for (blockIndex = 0; blockIndex < decoder->blockCount; ++blockIndex)
    {
        uint8 vectorIndex;
        if (!cine_read_u8(reader, &vectorIndex))
            return 0;
        cine_decode_block(decoder, blockIndex, &decoder->codebooks[decoder->activeStrip][0], vectorIndex,
                          vectorIndex, vectorIndex, vectorIndex);
    }
    return 1;
}

static int cine_decode_delta_vectors(CineDecoder *decoder, CineReader *reader)
{
    uint32 flags = 0;
    uint32 flagBits = 0;
    uint32 blockIndex;
    for (blockIndex = 0; blockIndex < decoder->blockCount; ++blockIndex)
    {
        int updated;
        int v4Coded;
        if (!flagBits)
        {
            if (!cine_read_be32(reader, &flags))
                return 0;
            flagBits = 32u;
        }
        updated = (flags & 0x80000000u) != 0;
        flags <<= 1;
        --flagBits;
        if (!updated)
            continue;
        if (!flagBits)
        {
            if (!cine_read_be32(reader, &flags))
                return 0;
            flagBits = 32u;
        }
        v4Coded = (flags & 0x80000000u) != 0;
        flags <<= 1;
        --flagBits;
        if (v4Coded)
        {
            uint8 v0, v1, v2, v3;
            if (!cine_read_u8(reader, &v0) || !cine_read_u8(reader, &v1) ||
                !cine_read_u8(reader, &v2) || !cine_read_u8(reader, &v3))
                return 0;
            cine_decode_block(decoder, blockIndex, &decoder->codebooks[decoder->activeStrip][1], v0, v1, v2, v3);
        }
        else
        {
            uint8 vectorIndex;
            if (!cine_read_u8(reader, &vectorIndex))
                return 0;
            cine_decode_block(decoder, blockIndex, &decoder->codebooks[decoder->activeStrip][0], vectorIndex,
                              vectorIndex, vectorIndex, vectorIndex);
        }
    }
    return 1;
}

static int cine_decode_chunk(CineDecoder *decoder, CineReader *reader)
{
    uint16 chunkType;
    uint16 chunkSize;
    CineReader chunkReader;
    if (reader->size - reader->position < 4u)
        return 1;
    if (!cine_read_be16(reader, &chunkType) || !cine_read_be16(reader, &chunkSize) ||
        chunkSize < 4u || (size_t)(chunkSize - 4u) > reader->size - reader->position)
        return 0;
    chunkReader.data = reader->data + reader->position;
    chunkReader.size = chunkSize - 4u;
    chunkReader.position = 0;
    reader->position += chunkReader.size;
    switch (chunkType)
    {
    case CINE_KEYFRAME_V4_CODEBOOK:
        return cine_decode_codebook(&chunkReader, &decoder->codebooks[decoder->activeStrip][1], 0);
    case CINE_DELTAFRAME_V4_CODEBOOK:
        return cine_decode_codebook(&chunkReader, &decoder->codebooks[decoder->activeStrip][1], 1);
    case CINE_KEYFRAME_V1_CODEBOOK:
        return cine_decode_codebook(&chunkReader, &decoder->codebooks[decoder->activeStrip][0], 0);
    case CINE_DELTAFRAME_V1_CODEBOOK:
        return cine_decode_codebook(&chunkReader, &decoder->codebooks[decoder->activeStrip][0], 1);
    case CINE_KEYFRAME_VECTORS:
        return cine_decode_keyframe_vectors(decoder, &chunkReader);
    case CINE_DELTAFRAME_VECTORS:
        return cine_decode_delta_vectors(decoder, &chunkReader);
    case CINE_KEYFRAME_V1_VECTORS:
        return cine_decode_keyframe_v1_vectors(decoder, &chunkReader);
    default:
        return 1;
    }
}

static int cine_copy_cvid(const uint8 *data, size_t size, uint8 **outData, size_t *outSize)
{
    uint8 *copy;
    if (!data || size < CINE_STREAM_HEADER_SIZE || !outData || !outSize)
        return 0;
    copy = (uint8 *)malloc(size);
    if (!copy)
        return 0;
    memcpy(copy, data, size);
    *outData = copy;
    *outSize = size;
    return 1;
}

static int cine_extract_video_stream(const uint8 *fileData, size_t fileSize,
                                     uint8 **outData, size_t *outSize)
{
    size_t position = 0;
    size_t totalSize = 20u;
    uint32 frameCount = 0;
    uint8 *streamData;
    size_t streamPosition = 20u;
    const uint8 *header = NULL;
    int foundVideo = 0;

    if (!fileData || !outData || !outSize)
        return 0;

    while (position + 8u <= fileSize)
    {
        uint32 chunkSize = cine_be32(fileData + position + 4u);
        if (chunkSize < 8u || position + chunkSize > fileSize)
            return 0;
        if (memcmp(fileData + position, "FILM", 4) == 0 && chunkSize >= 28u)
        {
            const uint8 *payload = fileData + position + 24u;
            size_t payloadSize = chunkSize - 24u;
            if (payloadSize >= 20u &&
                (memcmp(payload, "cvid", 4) == 0 || memcmp(payload, "CVID", 4) == 0))
            {
                if (!header)
                {
                    header = payload;
                }
                foundVideo = 1;
            }
            else if (foundVideo && chunkSize >= 44u &&
                     memcmp(fileData + position + 16u, "FRME", 4) == 0)
            {
                size_t frameSize = chunkSize - 28u;
                const uint8 *frame = fileData + position + 28u;
                uint16 width;
                uint16 height;
                uint16 strips;
                if (frameSize < 16u)
                    return 0;
                width = cine_be16(frame + 4u);
                height = cine_be16(frame + 6u);
                strips = cine_be16(frame + 8u);
                if ((width != 280u && width != 320u) ||
                    (height != 200u && height != 240u) ||
                    strips == 0u || strips > 2u)
                    return 0;
                totalSize += 4u + frameSize;
                ++frameCount;
            }
        }
        position += chunkSize;
    }

    if (!header || !frameCount || totalSize < 36u)
        return 0;
    streamData = (uint8 *)malloc(totalSize);
    if (!streamData)
        return 0;
    memcpy(streamData, header, 20u);
    streamData[16] = (uint8)(frameCount >> 24);
    streamData[17] = (uint8)(frameCount >> 16);
    streamData[18] = (uint8)(frameCount >> 8);
    streamData[19] = (uint8)frameCount;

    position = 0;
    while (position + 8u <= fileSize)
    {
        uint32 chunkSize = cine_be32(fileData + position + 4u);
        if (memcmp(fileData + position, "FILM", 4) == 0 && chunkSize >= 44u &&
            memcmp(fileData + position + 16u, "FRME", 4) == 0)
        {
            const uint8 *payload = fileData + position + 24u;
            size_t payloadSize = chunkSize - 24u;
            if (payloadSize < 20u ||
                (memcmp(payload, "cvid", 4) != 0 && memcmp(payload, "CVID", 4) != 0))
            {
                size_t frameSize = chunkSize - 28u;
                const uint8 *frame = fileData + position + 28u;
                uint16 width = cine_be16(frame + 4u);
                uint16 height = cine_be16(frame + 6u);
                uint16 strips = cine_be16(frame + 8u);
                if ((width != 280u && width != 320u) ||
                    (height != 200u && height != 240u) ||
                    strips == 0u || strips > 2u)
                {
                    free(streamData);
                    return 0;
                }
                streamData[streamPosition + 0u] = (uint8)(frameSize >> 24);
                streamData[streamPosition + 1u] = (uint8)(frameSize >> 16);
                streamData[streamPosition + 2u] = (uint8)(frameSize >> 8);
                streamData[streamPosition + 3u] = (uint8)frameSize;
                streamPosition += 4u;
                memcpy(streamData + streamPosition, frame, frameSize);
                streamPosition += frameSize;
            }
        }
        position += chunkSize;
    }

    if (streamPosition != totalSize)
    {
        free(streamData);
        return 0;
    }
    *outData = streamData;
    *outSize = streamPosition;
    return 1;
}

static int cine_init_decoder(CineDecoder *decoder, const uint8 *fileData, size_t fileSize)
{
    uint8 *streamData = NULL;
    size_t streamSize = 0;
    uint32 width;
    uint32 height;
    uint32 count;
    uint32 scale;

    memset(decoder, 0, sizeof(*decoder));
    if (!cine_extract_video_stream(fileData, fileSize, &streamData, &streamSize))
        return 0;
    if (memcmp(streamData, "cvid", 4) != 0 && memcmp(streamData, "CVID", 4) != 0)
    {
        free(streamData);
        return 0;
    }
    height = cine_be32(streamData + 4u);
    width = cine_be32(streamData + 8u);
    scale = cine_be32(streamData + 12u);
    count = cine_be32(streamData + 16u);
    if ((width != 280u && width != 320u) ||
        (height != 200u && height != 240u) ||
        (width & 3u) || (height & 3u) || !count || !scale)
    {
        free(streamData);
        return 0;
    }
    decoder->movieData = streamData;
    decoder->movieDataSize = streamSize;
    decoder->frameOffset = CINE_STREAM_HEADER_SIZE;
    decoder->frameNumber = 0;
    decoder->frameCount = count;
    decoder->width = width;
    decoder->height = height;
    decoder->blockWidth = width / 4u;
    decoder->blockHeight = height / 4u;
    decoder->blockCount = decoder->blockWidth * decoder->blockHeight;
    decoder->stripBlockWidth = decoder->blockWidth;
    decoder->stripOriginX = 0;
    decoder->stripOriginY = 0;
    decoder->frameRate = scale;
    return 1;
}

static void cine_shutdown_decoder(CineDecoder *decoder)
{
    free(decoder->movieData);
    memset(decoder, 0, sizeof(*decoder));
}

static int cine_decode_next_frame(CineDecoder *decoder)
{
    const uint8 *frameData;
    size_t frameEnd;
    uint32 frameSize;
    uint8 frameFlags;
    uint16 width;
    uint16 height;
    uint16 stripCount;
    size_t stripPosition;
    uint16 stripSize;
    uint16 stripIndex;
    uint32 stripBottom = 0;
    CineReader stripReader;

    if (decoder->frameNumber >= decoder->frameCount ||
        decoder->frameOffset + 4u + CINE_FRAME_HEADER_SIZE > decoder->movieDataSize)
        return 0;

    frameSize = cine_be32(decoder->movieData + decoder->frameOffset);
    frameData = decoder->movieData + decoder->frameOffset + 4u;
    frameFlags = frameData[0];
    width = cine_be16(frameData + 4u);
    height = cine_be16(frameData + 6u);
    stripCount = cine_be16(frameData + 8u);
    frameEnd = decoder->frameOffset + 4u + frameSize;
    if (frameEnd > decoder->movieDataSize)
        return 0;

    if (frameSize < CINE_FRAME_HEADER_SIZE ||
        width != decoder->width || height != decoder->height ||
        stripCount == 0u || stripCount > 2u)
        return 0;

    stripPosition = decoder->frameOffset + 4u + CINE_FRAME_HEADER_SIZE;
    for (stripIndex = 0; stripIndex < stripCount; ++stripIndex)
    {
        if (stripPosition + CINE_STRIP_HEADER_SIZE > frameEnd)
            return 0;
        stripSize = cine_be16(decoder->movieData + stripPosition + 2u);
        if (stripSize < CINE_STRIP_HEADER_SIZE ||
            stripPosition + stripSize > frameEnd)
            return 0;
        {
            uint16 stripY1Value = cine_be16(decoder->movieData + stripPosition + 4u);
            uint16 stripX1 = cine_be16(decoder->movieData + stripPosition + 6u);
            uint16 stripY2Value = cine_be16(decoder->movieData + stripPosition + 8u);
            uint16 stripX2 = cine_be16(decoder->movieData + stripPosition + 10u);
            uint32 stripY1 = stripY1Value ? stripY1Value : stripBottom;
            uint32 stripY2 = stripY1Value ? stripY2Value : stripY1 + stripY2Value;
            if (stripX2 > decoder->width || stripY2 > decoder->height ||
                stripX1 >= stripX2 || stripY1 >= stripY2 ||
                ((stripX2 - stripX1) & 3u) || ((stripY2 - stripY1) & 3u))
                return 0;
            decoder->activeStrip = stripIndex;
            decoder->stripBlockWidth = (uint32)(stripX2 - stripX1) / CINE_BLOCK_SIZE;
            decoder->blockCount = decoder->stripBlockWidth *
                                  ((uint32)(stripY2 - stripY1) / CINE_BLOCK_SIZE);
            decoder->stripOriginX = stripX1;
            decoder->stripOriginY = stripY1;
            stripBottom = stripY2;
        }
        if (stripIndex > 0u &&
            !(frameFlags & CINE_FRAME_FLAG_SEPARATE_STRIP_CODEBOOKS))
        {
            memcpy(decoder->codebooks[stripIndex],
                   decoder->codebooks[stripIndex - 1u],
                   sizeof(decoder->codebooks[stripIndex]));
        }
        stripReader.data = decoder->movieData + stripPosition + CINE_STRIP_HEADER_SIZE;
        stripReader.size = stripSize - CINE_STRIP_HEADER_SIZE;
        stripReader.position = 0;
        while (stripReader.position < stripReader.size)
        {
            if (stripReader.size - stripReader.position < 4u)
                break;
            if (!cine_decode_chunk(decoder, &stripReader))
                return 0;
        }
        stripPosition += stripSize;
    }

    decoder->frameOffset = frameEnd;
    ++decoder->frameNumber;
    return 1;
}

static int cine_movie_events(void)
{
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
        if (event.type == SDL_EVENT_QUIT)
            return 1;
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat)
        {
            if (event.key.scancode == SDL_SCANCODE_ESCAPE ||
                event.key.scancode == SDL_SCANCODE_RETURN ||
                event.key.scancode == SDL_SCANCODE_SPACE)
                return 1;
        }
        if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
            (event.gbutton.button == SDL_GAMEPAD_BUTTON_START ||
             event.gbutton.button == SDL_GAMEPAD_BUTTON_BACK))
            return 1;
    }
    return 0;
}

static void cine_wait(uint64 target)
{
    uint64 now = SDL_GetPerformanceCounter();
    uint64 frequency = SDL_GetPerformanceFrequency();
    if (!frequency || now >= target)
        return;
    {
        uint64 remaining = target - now;
        uint64 milliseconds = (remaining * 1000u) / frequency;
        if (milliseconds > 1u)
            SDL_Delay((uint32)(milliseconds - 1u));
    }
}

static void cine_present(RastPort *rp, const uint32 *pixels, uint32 width, uint32 height)
{
    uint32 frame[CINE_MAX_WIDTH * CINE_MAX_HEIGHT];
    uint32 y;
    uint32 xOffset;
    uint32 yOffset;
    Bitmap *bm;
    if (!rp || !pixels)
        return;
    bm = SDL3_3DO_GetScreenBitmap(rp->rp_ScreenItem);
    if (!bm)
        return;
    memset(frame, 0, sizeof(frame));
    xOffset = (320u - width) / 2u;
    yOffset = (240u - height) / 2u;
    for (y = 0; y < height; ++y)
        memcpy(frame + (y + yOffset) * 320u + xOffset,
               pixels + y * CINE_MAX_WIDTH, width * sizeof(uint32));
    SDL3_3DO_SetFrame(bm, frame, 320 * (int)sizeof(uint32));
    SDL3_3DO_Present(rp->rp_ScreenItem);
}

static int cine_load_direct(const char *path, uint8 **data, size_t *size)
{
    FILE *file;
    long fileSize;
    uint8 *buffer;
    if (!path || !data || !size)
        return 0;
    file = fopen(path, "rb");
    if (!file)
        return 0;
    if (fseek(file, 0, SEEK_END) != 0)
    {
        fclose(file);
        return 0;
    }
    fileSize = ftell(file);
    if (fileSize <= 0 || fseek(file, 0, SEEK_SET) != 0)
    {
        fclose(file);
        return 0;
    }
    buffer = (uint8 *)malloc((size_t)fileSize);
    if (!buffer)
    {
        fclose(file);
        return 0;
    }
    if (fread(buffer, 1, (size_t)fileSize, file) != (size_t)fileSize)
    {
        free(buffer);
        fclose(file);
        return 0;
    }
    fclose(file);
    *data = buffer;
    *size = (size_t)fileSize;
    return 1;
}

static int cine_load_file(const char *filename, uint8 **data, size_t *size)
{
    char basePath[1024];
    char path[1024];
    const char *base;
    const char *relative;
    const char *slash;
    const char *root = NULL;
    if (!filename || !data || !size)
        return 0;
    if (SDL3_3DO_LoadFile(filename, data, size))
        return 1;
    base = filename;
    if (base[0] == '$' && !strncmp(base, "$progdir", 8))
    {
        base += 8;
        if (*base == '/')
            ++base;
    }
    relative = base;
    if (cine_load_direct(relative, data, size))
        return 1;
    if (base[0])
    {
        root = SDL_GetBasePath();
        if (root)
        {
            snprintf(basePath, sizeof(basePath), "%s%s", root, relative);
            if (cine_load_direct(basePath, data, size))
                return 1;
        }
    }
    if (!cine_load_direct(filename, data, size))
    {
        if (strlen(filename) + 5u < sizeof(path))
        {
            snprintf(path, sizeof(path), "%s.cine", filename);
            if (SDL3_3DO_LoadFile(path, data, size))
                return 1;
            if (cine_load_direct(path, data, size))
                return 1;
        }
    }
    slash = strrchr(relative, '/');
    base = slash ? slash + 1 : relative;
    if (base && *base && strlen(base) + 6u < sizeof(path))
    {
        snprintf(path, sizeof(path), "%s.cine", base);
        if (SDL3_3DO_LoadFile(path, data, size))
            return 1;
        if (root)
        {
            snprintf(basePath, sizeof(basePath), "%s%s", root, path);
            if (cine_load_direct(basePath, data, size))
                return 1;
        }
    }
    return 0;
}

void initstreaming(void) {}
void shutdownstreaming(void) { SDL3_3DO_StopMovieAudio(); }
void freebufferlist(buf) struct DSDataBuf *buf;
{
    (void)buf;
}
void openstream(filename) char *filename;
{
    (void)filename;
}
void closestream(void) {}
int endofstream(void) { return 1; }
struct CCB *getstreamccb(void) { return NULL; }
void uncpaktorp(rp) struct RastPort *rp;
{
    (void)rp;
}
void startstream(void) {}
void stopstream(void) { SDL3_3DO_StopMovieAudio(); }

int playcpak(filename)
char *filename;
{
    uint8 *fileData = NULL;
    size_t fileSize = 0;
    CineDecoder decoder;
    RastPort *rp = NULL;
    uint64 frequency;
    uint64 frameStep;
    uint64 playbackStart;
    uint64 audioDurationUs;
    int result = 0;
    int movieAudioPlaying = 0;
    extern RastPort *rpvis;

    if (!filename)
        return 0;
    if (!cine_load_file(filename, &fileData, &fileSize))
        return 0;
    if (!cine_init_decoder(&decoder, fileData, fileSize))
    {
        free(fileData);
        return 0;
    }
    rp = rpvis;
    if (rp)
        fadetolevel(rp, ONE_F16);
    movieAudioPlaying = SDL3_3DO_StartMovieAudio(fileData, fileSize);
    free(fileData);
    audioDurationUs = SDL3_3DO_GetMovieAudioDurationUs();
    frequency = SDL_GetPerformanceFrequency();
    if (!frequency)
        frequency = 1;
    frameStep = frequency / (decoder.frameRate ? decoder.frameRate : 15u);
    if (!frameStep)
        frameStep = 1;
    playbackStart = SDL_GetPerformanceCounter();
    while (decoder.frameNumber < decoder.frameCount)
    {
        uint64 frameIndex = decoder.frameNumber;
        uint64 frameTarget;
        if (movieAudioPlaying && audioDurationUs)
        {
            uint64 durationTicks = (frequency / 1000000u) * audioDurationUs +
                                   ((frequency % 1000000u) * audioDurationUs) / 1000000u;
            frameTarget = playbackStart +
                          (durationTicks * frameIndex) / decoder.frameCount;
        }
        else
        {
            frameTarget = playbackStart + frameStep * frameIndex;
        }
        if (cine_movie_events())
        {
            result = 1;
            break;
        }
        cine_wait(frameTarget);
        if (cine_movie_events())
        {
            result = 1;
            break;
        }
        if (!cine_decode_next_frame(&decoder))
        {
            result = 1;
            break;
        }
        cine_present(rp, decoder.pixels, decoder.width, decoder.height);
    }
    while (movieAudioPlaying && decoder.frameNumber >= decoder.frameCount && !cine_movie_events())
    {
        if (!SDL3_3DO_IsMovieAudioPlaying())
            break;
        SDL_Delay(5);
    }
    SDL3_3DO_StopMovieAudio();
    cine_shutdown_decoder(&decoder);
    return result;
}
