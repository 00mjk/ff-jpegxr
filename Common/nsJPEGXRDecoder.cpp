// Copyright � Microsoft Corp.
// All rights reserved.
// 
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
// 
// � Redistributions of source code must retain the above copyright notice,
//   this list of conditions and the following disclaimer.
// � Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.


#include "stdafx.h"

#ifdef PR_LOGGING
static PRLogModuleInfo *
GetJPEGXRLog()
{
  static PRLogModuleInfo *sJPEGXRLog;
  if (!sJPEGXRLog)
    sJPEGXRLog = PR_NewLogModule("JPEGXRDecoder");
  return sJPEGXRLog;
}
#endif

inline void * moz_malloc(size_t size)
{
    void *result;

    try
    {
        result = new unsigned char[size];
    }
    catch(...)
    {
        result = nullptr;
    }

    return result;
}

inline void moz_free(void *ptr)
{
    delete [] ptr;
}

#ifdef USE_CHAIN_BUFFER

static void * MyAlloc(size_t cBytes)
{
    return moz_malloc(cBytes);
}

static void MyFree(void *ptr)
{
    moz_free(ptr);
}

#endif

///////////////////////////////////

static void qcms_transform_release(qcms_transform *transform)
{
    moz_free(transform);
}

static void qcms_profile_release(qcms_profile *inProfile)
{
    moz_free(inProfile);
}

static void qcms_transform_data(qcms_transform *transform, void *pSrc, void *pDest, size_t width)
{
    // !!!TMP!!!
}

//////////////////////////////////

nsJPEGXRDecoder::nsJPEGXRDecoder(RasterImage &aImage, bool hasBeenDecoded) : nsDecoder(aImage), 
    m_decodeAtEnd(hasBeenDecoded), 
    m_totalReceived(0), 
    m_pDecoder(nullptr), m_pConverter(nullptr), m_pStream(nullptr), 
    m_decoderInitialized(false), 
    m_startedDecodingMBRows(false), m_startedDecodingMBRows_Alpha(false), 
    m_decodingAlpha(false), 
    m_mbRowBuf(nullptr), m_mbRowBufStride(0), 
    m_currLine(0), m_scale(1), 
    m_numAvailableBands(0), m_currentTileRow(0), m_currentSubBand(0), 
    m_mainPlaneFinished(false), m_progressiveDecoding(false), 
    m_tileRowBandInfos(nullptr), m_tileRowInfos(nullptr), 
    m_outPixelFormat(pfNone), m_inProfile(nullptr), m_transform(nullptr), m_xfBuf(nullptr), m_xfBufRowStride(0), m_xfPixelFormat(pfNone), 
    m_skippedTileRows(false), 
    m_alphaBitDepth(0)
{
#ifdef USE_CHAIN_BUFFER
    m_chainBuf.SetMemAllocProc(MyAlloc, MyFree);
#endif
}

nsJPEGXRDecoder::~nsJPEGXRDecoder()
{
    DestroyJXRStuff();
    FreeMBRowBuffers();

    moz_free(m_tileRowBandInfos);
    moz_free(m_tileRowInfos);

    if (m_transform)
        qcms_transform_release(m_transform);

    if (m_inProfile)
        qcms_profile_release(m_inProfile);
}

EXTERN_C ERR CreateWS_List(struct WMPStream** ppWS);

bool nsJPEGXRDecoder::CreateJXRStuff()
{
    ERR err = WMP_errSuccess;

    // Create a JPEG-XR file decoder
    Call(PKImageDecode_Create_WMP(&m_pDecoder));

    // Create a pixel format converter
    Call(PKCodecFactory_CreateFormatConverter(&m_pConverter));

    // Some converters will need a pointer to decoder, although they use a bad design - 
    // they (for instance, BlackWhite_Gray8()) assume that main image is being decode, but it could be alpha too.
    // It would be better to have additional properties in the converter itself.
    m_pConverter->pDecoder = m_pDecoder;

    // Create a stream
#if 0
    Call(CreateWS_ChainBuf(&m_pStream, &m_chainBuf));
#else
    Call(CreateWS_List(&m_pStream));
#endif

Cleanup:

    if (WMP_errSuccess != err)
        DestroyJXRStuff();

    return true;
}

void nsJPEGXRDecoder::DestroyJXRStuff()
{
    if (nullptr != m_pDecoder)
        m_pDecoder->Release(&m_pDecoder);

    if (nullptr != m_pConverter)
        m_pConverter->Release(&m_pConverter);

    if (nullptr != m_pStream)
        m_pStream->Close(&m_pStream);

    m_decoderInitialized = false;
}

void nsJPEGXRDecoder::InitializeJXRDecoder()
{
    if (DecoderInitialized())
        return;

    m_decoderInitialized = WMP_errSuccess == m_pDecoder->Initialize(m_pDecoder, m_pStream);
}

bool nsJPEGXRDecoder::HasAlpha() const
{
    PKPixelInfo PI;
    PI.pGUIDPixFmt = &m_pDecoder->guidPixFormat;
    ERR err = PixelFormatLookup(&PI, LOOKUP_FORWARD);

    if (WMP_errSuccess != err)
        return false;

    bool hasAlpha = (PI.grBit & PK_pixfmtHasAlpha) != 0;
    return hasAlpha;
}

bool nsJPEGXRDecoder::HasPlanarAlpha() const
{
    return m_pDecoder->WMP.bHasAlpha != 0;
}

bool nsJPEGXRDecoder::GetSize(size_t &width, size_t &height)
{
    if (nullptr == m_pDecoder)
    {
        width = height = 0;
        return false;
    }

    I32 w, h;
#if 0
    m_pDecoder->GetSize(m_pDecoder, &w, &h);
#else
    w = m_pDecoder->uWidth;
    h = m_pDecoder->uHeight;
#endif

    width = (size_t)w;
    height = (size_t)h;
    return true;
}

bool nsJPEGXRDecoder::GetThumbnailSize(size_t &width, size_t &height)
{
    if (nullptr == m_pDecoder)
    {
        width = height = 0;
        return false;
    }

    size_t w, h;
    GetSize(w, h);

    CWMImageInfo wmii;
    wmii.cThumbnailScale = GetScale();
    wmii.cWidth = w;
    wmii.cHeight = h;

    CalcThumbnailSize(&wmii);
    width = wmii.cThumbnailWidth;
    height = wmii.cThumbnailHeight;

    return true;
}

bool nsJPEGXRDecoder::Receive(const uint8_t *buf, uint32_t count)
{
    WMPStream *pWS = m_pStream;
    pWS->SetPos(pWS, GetTotalNumBytesReceived());
    pWS->Write(pWS, buf, count);
    m_totalReceived += count;

    return true;
}

uint32_t nsJPEGXRDecoder::GetPixFmtBitsPP(PixelFormat pixFmt)
{
    switch (pixFmt)
    {
    case pfBGR24:
    case pfRGB24:
        return 24;

    case pfBGR32:
    case pfRGB32:
    case pfBGRA32:
    case pfRGBA32:
        return 32;

    case pfGray:
        return 8;

    case pfCMYK32:
        return 32;

    case pfCMYKA40:
        return 40;

    case pfCMYK64:
        return 64;

    case pfCMYKA80:
        return 80;
    }

    return 0;
}

// Allocate raster buffer for the decoding of macroblock rows
void nsJPEGXRDecoder::AllocateMBRowBuffer(size_t width, bool decodeAlpha)
{
    FreeMBRowBuffers();

    PKPixelFormatGUID srcFmtGUID;
    ERR err = m_pDecoder->GetPixelFormat(m_pDecoder, &srcFmtGUID);

    if (WMP_errSuccess != err)
        return;

    PKPixelFormatGUID outFmt = GUID_PKPixelFormatDontCare; // to avoid a compiler warning
    PixelFormat cmykPF = pfNone;

    if (Y_ONLY == m_pDecoder->WMP.wmiI.cfColorFormat)
    {
        assert(!HasAlpha());

        outFmt = GUID_PKPixelFormat8bppGray;
        m_outPixelFormat = pfGray;

        if (nullptr != m_transform)
        {
            // We need to convert 8-bit gray to 24bpp RGB to feed it to color transformer
            // JPEG-XR does not support grayscale images with alpha, so don't care about that case
            m_xfPixelFormat = pfRGB24;
        }
    }
    // CMYK formats
    else if (IsEqualGUID(GUID_PKPixelFormat32bppCMYK, srcFmtGUID))
    {
        cmykPF = pfCMYK32;
        m_xfPixelFormat = pfRGB32;
    }
    else if (IsEqualGUID(GUID_PKPixelFormat64bppCMYK, srcFmtGUID))
    {
        cmykPF = pfCMYK64;
        m_xfPixelFormat = pfRGB32;
    }
    else if (IsEqualGUID(GUID_PKPixelFormat40bppCMYKAlpha, srcFmtGUID))
    {
        cmykPF = pfCMYKA40;
        m_xfPixelFormat = decodeAlpha ? pfRGBA32 : pfRGB32;
    }
    else if (IsEqualGUID(GUID_PKPixelFormat80bppCMYKAlpha, srcFmtGUID))
    {
        cmykPF = pfCMYKA80;
        m_xfPixelFormat = decodeAlpha ? pfRGBA32 : pfRGB32;
    }
    // All other formats
    else
    {
        assert(CMYK != m_pDecoder->WMP.wmiI.cfColorFormat);

        // We need to perform color transformation
        if (decodeAlpha)
        {
            m_outPixelFormat = pfRGBA32;
            outFmt = GUID_PKPixelFormat32bppRGBA;
        }
        else
        {
            if (HasAlpha())
            {
                outFmt = GUID_PKPixelFormat32bppRGB;
                m_outPixelFormat = pfRGB32;
            }
            else
            {
                m_outPixelFormat = pfRGB24;
                outFmt = GUID_PKPixelFormat24bppRGB;
            }
        }
    }

    if (pfNone != cmykPF)
    {
        outFmt = srcFmtGUID;
        m_outPixelFormat = cmykPF;
    }

    if (nullptr != m_transform)
    {
        // use intermediate pixel format conversion
        m_xfPixelFormat = m_outPixelFormat;
    }

    err = PKFormatConverter_InitializeConvert(m_pConverter, srcFmtGUID, NULL, outFmt);

    if (WMP_errSuccess != err)
    {
        switch (m_outPixelFormat)
        {
        case pfRGBA32:
            outFmt = GUID_PKPixelFormat32bppBGRA;
            m_outPixelFormat = pfBGRA32;
            break;

        case pfRGB32:
            outFmt = GUID_PKPixelFormat32bppBGR;
            m_outPixelFormat = pfBGR32;
            break;

        case pfRGB24:
            outFmt = GUID_PKPixelFormat24bppBGR;
            m_outPixelFormat = pfBGR24;
            break;
        }

        err = PKFormatConverter_InitializeConvert(m_pConverter, srcFmtGUID, NULL, outFmt);

        if (WMP_errSuccess != err)
            return;
    }

    if (m_pDecoder->WMP.wmiI.cThumbnailScale < 1 || m_pDecoder->WMP.wmiI.cThumbnailScale > 16)
        m_pDecoder->WMP.wmiI.cThumbnailScale = 1; // just in case

    const size_t MBR_HEIGHT = 16;
    size_t cLinesPerMBRow = MBR_HEIGHT / m_pDecoder->WMP.wmiI.cThumbnailScale;

    if (pfNone != m_xfPixelFormat)
    {
        // Allocate a buffer for color transformation
        uint32_t xf_bpp = GetPixFmtBitsPP(m_xfPixelFormat);
        m_xfBufRowStride = ((xf_bpp + 7) >> 3) * width;
        m_xfBuf = (uint8_t *)moz_malloc(m_xfBufRowStride * cLinesPerMBRow);

        if (nullptr == m_xfBuf)
            return;
    }

    PKPixelInfo pPIFrom;
    pPIFrom.pGUIDPixFmt = &srcFmtGUID;
    PixelFormatLookup(&pPIFrom, LOOKUP_FORWARD);

    uint32_t cbStrideFrom = (BD_1 == pPIFrom.bdBitDepth ?  ((pPIFrom.cbitUnit * width + 7) >> 3) : (((pPIFrom.cbitUnit + 7) >> 3) * width));
    size_t dest_bpp = GetPixFmtBitsPP(m_outPixelFormat);
    uint32_t cbStrideTo = ((dest_bpp + 7) >> 3) * width;

    if (cbStrideTo > cbStrideFrom)
        cbStrideFrom = cbStrideTo; // we are going to do in-place pixel format conversion

#ifdef ENABLE_OPTIMIZATIONS
    cbStrideTo = (cbStrideTo + 127) / 128 * 128;
#endif

    U8 *pb = NULL;
    err = PKAllocAligned((void **)&pb, cbStrideFrom * cLinesPerMBRow, 128);

    if (WMP_errSuccess == err)
    {
        m_mbRowBuf = pb;
        m_mbRowBufStride = cbStrideFrom;
    }
}

void nsJPEGXRDecoder::AllocateMBRowBuffer_Alpha(size_t width)
{
    FreeMBRowBuffers();

    m_pStream->SetPos(m_pStream, m_pDecoder->WMP.wmiDEMisc.uAlphaOffset);
    CWMImageInfo *pWMII = &m_pDecoder->WMP.wmiI_Alpha;
    Int res = ImageStrDecGetInfo(pWMII, &m_pDecoder->WMP.wmiSCP_Alpha);

    if (ICERR_OK != res)
        return;

    pWMII->oOrientation = O_NONE; // we handle orientation here, not in JXRLib decoder

    size_t cbitUnit;

    switch (pWMII->bdBitDepth)
    {
        case BD_8:
            cbitUnit = 8;
            break;

        case BD_16:
        case BD_16S:
        case BD_16F:
            cbitUnit = 16;
            break;

        case BD_32:
        case BD_32S:
        case BD_32F:
            cbitUnit = 32;
            break;

        default:
            return;
    }

    pWMII->cBitsPerUnit = cbitUnit;

    // Set the pixel fromat GUID for alpha buffer
    PKPixelFormatGUID srcFmtGUID;
    ERR err = m_pDecoder->GetPixelFormat(m_pDecoder, &srcFmtGUID);

    if (WMP_errSuccess != err)
        return;

    if (IsEqualGUID(GUID_PKPixelFormat32bppRGBA, srcFmtGUID) || IsEqualGUID(GUID_PKPixelFormat32bppBGRA, srcFmtGUID) || 
        IsEqualGUID(GUID_PKPixelFormat32bppPRGBA, srcFmtGUID) || IsEqualGUID(GUID_PKPixelFormat32bppPBGRA, srcFmtGUID))
        srcFmtGUID = GUID_PKPixelFormat8bppGray;
    else if (IsEqualGUID(GUID_PKPixelFormat64bppRGBA, srcFmtGUID) || IsEqualGUID(GUID_PKPixelFormat64bppPRGBA, srcFmtGUID))
        srcFmtGUID = GUID_PKPixelFormat16bppGray;
    else if (IsEqualGUID(GUID_PKPixelFormat64bppRGBAFixedPoint, srcFmtGUID))
        srcFmtGUID = GUID_PKPixelFormat16bppGrayFixedPoint;
    else if (//IsEqualGUID(GUID_PKPixelFormat128bppRGBFixedPoint, srcFmtGUID) || 
               IsEqualGUID(GUID_PKPixelFormat128bppRGBAFixedPoint, srcFmtGUID))
        srcFmtGUID = GUID_PKPixelFormat32bppGrayFixedPoint;
    else if (IsEqualGUID(GUID_PKPixelFormat128bppRGBAFloat, srcFmtGUID) || IsEqualGUID(GUID_PKPixelFormat128bppPRGBAFloat, srcFmtGUID))
        srcFmtGUID = GUID_PKPixelFormat32bppGrayFloat;
    else if (IsEqualGUID(GUID_PKPixelFormat80bppCMYKAlpha, srcFmtGUID))
        srcFmtGUID = GUID_PKPixelFormat16bppGray;
    else if (IsEqualGUID(GUID_PKPixelFormat40bppCMYKAlpha, srcFmtGUID))
        srcFmtGUID = GUID_PKPixelFormat8bppGray;
    else if (//IsEqualGUID(GUID_PKPixelFormat64bppRGBHalf, srcFmtGUID) || 
             IsEqualGUID(GUID_PKPixelFormat64bppRGBAHalf, srcFmtGUID))
        srcFmtGUID = GUID_PKPixelFormat16bppGrayHalf;
    else
        return; // format not suported

    err = PKFormatConverter_InitializeConvert(m_pConverter, srcFmtGUID, NULL, GUID_PKPixelFormat8bppGray);

    if (WMP_errSuccess != err)
        return;

    uint32_t cbStride = ((cbitUnit + 7) >> 3) * width;

    // Currently, SSE2 optimization is used only with main image plane.
//#ifdef ENABLE_OPTIMIZATIONS
//    cbStride = (cbStride + 127) / 128 * 128;
//#endif

    if (m_pDecoder->WMP.wmiI.cThumbnailScale < 1 || m_pDecoder->WMP.wmiI.cThumbnailScale > 16)
        m_pDecoder->WMP.wmiI.cThumbnailScale = 1; // just in case

    const size_t MBR_HEIGHT = 16;
    size_t cLinesPerMBRow = MBR_HEIGHT / m_pDecoder->WMP.wmiI.cThumbnailScale;

    U8 *pb = NULL;
    Call(PKAllocAligned((void **)&pb, cbStride * cLinesPerMBRow, 128));

    m_alphaBitDepth = cbitUnit;
    m_mbRowBuf = pb;
    m_mbRowBufStride = cbStride;

Cleanup:
    return;
}

// Main image plane only
bool nsJPEGXRDecoder::FillTileRowBandInfo()
{
    if (!m_pDecoder->WMP.wmiSCP.bProgressiveMode)
        return false;

    if (nullptr != m_tileRowBandInfos)
        return true;

    size_t cTileRows, cTileCols, cBands, cHeaderSize;
    size_t *indexTable = GetIndexTable(m_pDecoder->WMP.ctxSC, &cTileRows, &cTileCols, &cBands, &cHeaderSize);

    if (nullptr == indexTable)
        return false;

    m_tileRowBandInfos = (TileRowBandInfo *)moz_malloc(sizeof(TileRowBandInfo) * cTileRows);

    if (nullptr == m_tileRowBandInfos)
    {
        m_numAvailableBands = 0;
        return false;
    }

    //size_t numTableEntries = cTileRows * cTileCols * cBands;
    size_t width, height;
    GetSize(width, height);

    if (m_pDecoder->WMP.wmiI.cThumbnailScale < 1 || m_pDecoder->WMP.wmiI.cThumbnailScale > 16)
        m_pDecoder->WMP.wmiI.cThumbnailScale = 1; // just in case

    const unsigned int MBR_HEIGHT = 16;
    const size_t mbRowHeight = MBR_HEIGHT / m_pDecoder->WMP.wmiI.cThumbnailScale;
    const size_t imageUpperLimit = m_pDecoder->WMP.wmiSCP.uStreamImageOffset + m_pDecoder->WMP.wmiI.uImageByteCount;

    // Calculate lower subband limits for every tile row
    for (size_t i = 0; i < cTileRows; ++i)
    {
        TileRowBandInfo &info = m_tileRowBandInfos[i];
        info.topMBRow = m_pDecoder->WMP.wmiSCP.uiTileY[i];

        if (i > 0)
            --info.topMBRow;

        info.top = info.topMBRow * mbRowHeight;
        info.height = (cTileRows - 1 == i ? height : m_pDecoder->WMP.wmiSCP.uiTileY[i + 1] * mbRowHeight) - info.top;

        if (0 == i)
            info.height -= mbRowHeight;

        for (size_t k = 0; k < cBands; ++k)
        {
            size_t index = (i * cTileCols) * cBands + k;
            size_t offset = indexTable[index];
            bool subBandPresent = true; // a subband may be missing for a tile row

            // Only the DC subband of the very first tile can be 0
            if (0 == offset && !(0 == i && 0 == k))
            {
                subBandPresent = false;

                // Try to find a subband entry for this tile row that is not 0, i.e. present
                for (size_t n = 1; n < cTileCols; ++n)
                {
                    size_t nextIndex = index + cBands * n;
                    offset = indexTable[nextIndex];

                    if (0 != offset)
                    {
                        subBandPresent = true;
                        break;
                    }
                }
            }

            if (subBandPresent)
                info.lowerLimits[k] = offset + cHeaderSize;
            else
                info.lowerLimits[k] = info.upperLimits[k] = 0; // this will mean the subband is not present
        }
    }

    // Calculate upper subband limits for every tile row
    TileRowBandInfo &topInfo = m_tileRowBandInfos[0];

    for (size_t i = 0 ; i < cTileRows; ++i)
    {
        TileRowBandInfo &info = m_tileRowBandInfos[i];
        size_t *upperLimits = info.upperLimits;

        for (size_t k = 0; k < cBands; ++k)
        {
            if (!(0 == i && 0 == k) && 0 == info.lowerLimits[k]) // subband is not present for all tiles in this row
            {
                //info.upperLimits[k] = 0; // should already have been assigned
                continue; // continue to the next subband (although this is probably the last one, namely flexbits)
            }

            size_t limit = 0;

            if (cTileRows - 1 == i) // if last tile row // next info's subband is not present
            {
                if (cBands - 1 == k) // if last band
                    limit = imageUpperLimit;
                else
                    limit = topInfo.lowerLimits[k + 1];
            }
            else
            {
                for (size_t n = i + 1; n < cTileRows; ++n)
                {
                    TileRowBandInfo &nextGoodInfo = m_tileRowBandInfos[n];
                    limit = nextGoodInfo.lowerLimits[k];

                    if (0 != limit)
                        break;
                }
            }

            if (0 != limit)
                upperLimits[k] = limit;
            else
            {
                if (cBands - 1 == k) // if last band
                    upperLimits[k] = imageUpperLimit;
                else
                {
                    for (size_t n = 0 ; n < cTileRows; ++n)
                    {
                        TileRowBandInfo &goodTopInfo = m_tileRowBandInfos[n];
                        limit = goodTopInfo.lowerLimits[k + 1];

                        if (0 != limit)
                        {
                            upperLimits[k] = limit;
                            break;
                        }
                    }

                    if (0 == limit)
                        upperLimits[k] = imageUpperLimit;
                }
            }
        }

        // Determine the last available subband for every tile row
        for (size_t k = cBands - 1; k != 0; --k) // DC is always present
        {
            //if (!(info.lowerLimits[k] == 0 && info.upperLimits[k] == 0))
            if (info.lowerLimits[k] != 0) // this condition is enough
            {
                info.lastPresentSubband = k + 1;
                break;
            }
        }
    }

    // Assign subband upper limits
    for (size_t i = 0; i < MAX_SUBBANDS; ++i)
    {
        m_subbandUpperLimits[i] = 0; // subband is not present for the entire image

        if (i < cBands)
        {
            for (size_t j = cTileRows; j > 0; --j)
            {
                TileRowBandInfo &info = m_tileRowBandInfos[j - 1];
                size_t limit = info.upperLimits[i];

                if (0 != limit)
                {
                    m_subbandUpperLimits[i] = limit;
                    m_numAvailableBands = i + 1; // so it is possible to have m_numAvailableBands < cBands
                    break;
                }
            }
        }
    }

    return true;
}

bool nsJPEGXRDecoder::FillTileRowInfo()
{
    if (m_pDecoder->WMP.wmiSCP.bProgressiveMode)
        return false;

    if (nullptr != m_tileRowInfos)
        return false;

    size_t cTileRows, cTileCols, cBands, cHeaderSize;
    size_t *indexTable = GetIndexTable(m_pDecoder->WMP.ctxSC, &cTileRows, &cTileCols, &cBands, &cHeaderSize);
    bool spatial = SPATIAL == m_pDecoder->WMP.wmiSCP.bfBitstreamFormat;
    size_t cEntriesPerTile = spatial ? 1 : cBands;

    if (nullptr == indexTable)
        return false;

    m_tileRowInfos = (TileRowInfo *)moz_malloc(sizeof(TileRowInfo) * cTileRows);

    if (nullptr == m_tileRowInfos)
    {
        m_numAvailableBands = 0;
        return false;
    }

    size_t width, height;
    GetSize(width, height);

    if (m_pDecoder->WMP.wmiI.cThumbnailScale < 1 || m_pDecoder->WMP.wmiI.cThumbnailScale > 16)
        m_pDecoder->WMP.wmiI.cThumbnailScale = 1; // just in case

    const unsigned int MBR_HEIGHT = 16;
    const size_t mbRowHeight = MBR_HEIGHT / m_pDecoder->WMP.wmiI.cThumbnailScale;
    const size_t imageUpperLimit = m_pDecoder->WMP.wmiSCP.uStreamImageOffset + m_pDecoder->WMP.wmiI.uImageByteCount;
    size_t nextLowerLimit = imageUpperLimit;
    bool lastRow = true;

#if 0

    // This will probably always work for FREQUENCY mode
    // (and in SPATIAL mode - when all the tilea are in scan order and when there are no tiles shared by 
    // different tile rows
    // !!!This won't work with SPATIAL mode when there are shared tiles or tiles that do not come in "scan order"!!!
    for (int32_t i = (int32_t)cTileRows - 1; i >= 0; --i)
    {
        TileRowInfo &info = m_tileRowInfos[i];
        info.top = m_pDecoder->WMP.wmiSCP.uiTileY[i] * mbRowHeight;

        // We are not interested in the last info, so we set numMBRows to 0.
        // Hopefully, JPEG-XR standard v.2.0 will support macroblock row index tables, 
        // so we won't need this.
        info.numMBRows = lastRow ? 0 : m_pDecoder->WMP.wmiSCP.uiTileY[i + 1] - m_pDecoder->WMP.wmiSCP.uiTileY[i];

        if (i > 0)
            info.top -= mbRowHeight;

        info.height = (lastRow ? height : m_tileRowInfos[i + 1].top) - info.top;

        size_t index = i * cTileCols * cEntriesPerTile; // index of the first entry in the first column of current tile row
        size_t lowerLimit = indexTable[index]  + cHeaderSize;
        info.upperLimit = nextLowerLimit;
        nextLowerLimit = lowerLimit;
        lastRow = false;
    }

#else
    // !!!This code is unfinished!!!
    // In SPATIAL mode, tiles may come in an arbitrary order, and can even be shared
    // We need to caculate maximum possible number of bytes that can be discarded for each tile row

    // Since the tiles are unordered in a general case, we have tosort the entries in order
    // to determine the upper limit of every entry
    size_t numTableEntries = cEntriesPerTile * cTileRows * cTileCols;
    typedef std::set<size_t> TileEntries;
    TileEntries entries;

    for (size_t i = 0; i < numTableEntries; ++i)
    {
        size_t offset = indexTable[i];

        // If an index table entry is 0 in FREQUENCY mode (except teh very first one), 
        // this means the subband is not present in this tile, so do not insert it in the set
        if (spatial || !(i > 0 && 0 == offset))
            entries.insert(offset + cHeaderSize);
    }

    entries.insert(imageUpperLimit);
    size_t cEntriesPerTileRow = cTileCols * cEntriesPerTile;

    for (int32_t i = (int32_t)cTileRows - 1; i >= 0; --i)
    {
        TileRowInfo &info = m_tileRowInfos[i];
        info.nextLowerLimit = nextLowerLimit;
        info.top = m_pDecoder->WMP.wmiSCP.uiTileY[i] * mbRowHeight;

        // We are not interested in the last info, so we set numMBRows to 0.
        // Hopefully, JPEG-XR standard v.2.0 will support macroblock row index tables, 
        // so we won't need this.
        info.numMBRows = lastRow ? 0 : m_pDecoder->WMP.wmiSCP.uiTileY[i + 1] - m_pDecoder->WMP.wmiSCP.uiTileY[i];

        if (i > 0)
            info.top -= mbRowHeight;

        info.height = (lastRow ? height : m_tileRowInfos[i + 1].top) - info.top;

        size_t index = i * cTileCols * cEntriesPerTile; // index of the first entry in the first column of current tile row
        size_t rowLowerLimit = (size_t)-1, rowUpperLimit = 0; // current tile row's limits

        for (size_t j = 0; j < cEntriesPerTileRow; ++j)
        {
            size_t offset = indexTable[index + j]  + cHeaderSize;
            TileEntries::const_iterator result = entries.find(offset);

            if (offset < rowLowerLimit)
                rowLowerLimit = offset;

            ++result;
            offset = *result;

            if (offset > rowUpperLimit)
                rowUpperLimit = offset;
        }

        info.upperLimit = rowUpperLimit;

        if (rowLowerLimit < nextLowerLimit)
            nextLowerLimit = rowLowerLimit;

        lastRow = false;
    }
#endif

    m_numAvailableBands = cBands;

    return true;
}

size_t nsJPEGXRDecoder::GetTileRowForMBRow(size_t mbRow)
{
    if (nullptr == m_tileRowInfos)
        return 0;

    size_t cTileRows = GetNumTileRows();
    size_t numRows = 0;

    // Ignore the last row
    for (size_t i = 0; i < cTileRows - 1; ++i)
    {
        numRows += m_tileRowInfos[i].numMBRows;

        if (mbRow < numRows)
            return i;
    }

    return cTileRows - 1;
}

// Number of subbands that can be decoded with the number of bytes received (in progressive layout)
size_t nsJPEGXRDecoder::GetNumberOfCoveredSubBands()
{
    size_t result = 0;

    for (size_t i = 0; i < GetNumAvailableBands(); ++i)
    {
        if (0 != m_subbandUpperLimits[i]) // ignore empty subbands
        {
            if (GetTotalNumBytesReceived() < m_subbandUpperLimits[i])
                break;

            result = i + 1;
        }
    }

    return result;
}

bool nsJPEGXRDecoder::HasEmptyTileRowSubbands(size_t subband)
{
    for (size_t i = 0; i < GetNumTileRows(); ++i)
    {
        const TileRowBandInfo &info = m_tileRowBandInfos[i];

        if (0 == info.upperLimits[subband])
            return true;
    }

    return false;
}

EXTERN_C Int StartDecodingSubband(CTXSTRCODEC ctxSC, SUBBAND sb, const CWMImageInfo *pWMII);
EXTERN_C Int EndDecodingSubband(CTXSTRCODEC ctxSC);

void nsJPEGXRDecoder::StartDecodingNextSubband()
{
    if (!StartedDecodingMBRows())
        return;

    SUBBAND sb;

    switch (GetCurrentSubBand())
    {
    case 0:
        sb = SB_DC_ONLY;
        break;
    case 1:
        sb = SB_NO_HIGHPASS;
        break;
    case 2:
        sb = SB_NO_FLEXBITS;
        break;
    case 3:
        sb = SB_ALL;
        break;
    default:
        return;
    }

    m_pDecoder->WMP.wmiSCP.sbSubband = sb;
    m_pDecoder->WMP.wmiSCP_Alpha.sbSubband = sb;

    StartDecodingSubband(m_pDecoder->WMP.ctxSC, sb, &m_pDecoder->WMP.wmiI);
    m_pDecoder->WMP.DecoderCurrMBRow = 0;
    m_currentTileRow = 0;
    m_currLine = 0;

    m_startedDecodingSubband = true;
}

void nsJPEGXRDecoder::EndDecodingCurrentSubband()
{
    if (!StartedDecodingMBRows() || !StartedDecodingSubband())
        return;

    EndDecodingSubband(m_pDecoder->WMP.ctxSC);

    m_startedDecodingSubband = false;
}

void nsJPEGXRDecoder::StartProgressiveDecoding(bool decodeAlpha)
{
    if (StartedDecodingMBRows())
        return;

    m_pDecoder->WMP.wmiI.cThumbnailScale = GetScale();

    size_t width, height;
    GetThumbnailSize(width, height);

    AllocateMBRowBuffer(width, decodeAlpha);

    if (nullptr == m_mbRowBuf)
    {
        PostDecoderError(NS_ERROR_FAILURE);
        return;
    }

    m_currentTileRow = 0;

    SUBBAND sb = SB_DC_ONLY;
    m_pDecoder->WMP.wmiSCP.sbSubband = sb;
    m_pDecoder->WMP.wmiSCP_Alpha.sbSubband = sb;
    m_pDecoder->WMP.wmiSCP.uAlphaMode = decodeAlpha ? 2 : 0;
    m_decodingAlpha = decodeAlpha;

    // The assignment below is unnecessary because in this particular decoder
    // the beginnig of the image file is the beginning of the stream.
    // Keep this line as a reminder that offStart should be changed if the stream begins
    // somewhere else (for example, where the image data starts, i.e. at m_pDecoder->WMP.wmiDEMisc.uImageOffset)
    m_pDecoder->offStart = 0; // our stream starts at the beginning of the image file

    ERR err = JXR_BeginDecodingMBRows(m_pDecoder, NULL, m_mbRowBuf, m_mbRowBufStride, FALSE, FALSE); // decoding into a single MB row buffer, not fail-safe

    if (WMP_errSuccess != err)
        return;

    m_startedDecodingMBRows = true;

    // Currently, we can determine whether the layout is progressive only after initializing 
    // the coding context with PKImage_StartDecodingMBRows. So if not progressive, terminate
    // the decoding
    if (!m_pDecoder->WMP.wmiSCP.bProgressiveMode)
    {
        EndDecodingMBRows();
        return;
    }

    m_progressiveDecoding = true;
    m_startedDecodingSubband = true;
}

void nsJPEGXRDecoder::StartDecodingMBRows(bool failSafe, bool decodeAlpha)
{
    if (StartedDecodingMBRows())
        return;

    m_pDecoder->WMP.wmiI.cThumbnailScale = GetScale();

    size_t width, height;
    GetThumbnailSize(width, height);

    AllocateMBRowBuffer(width, decodeAlpha);

    if (nullptr == m_mbRowBuf)
    {
        PostDecoderError(NS_ERROR_FAILURE);
        return;
    }

    SUBBAND sb = SB_ALL;

    m_pDecoder->WMP.wmiSCP.sbSubband = sb;
    m_pDecoder->WMP.wmiSCP_Alpha.sbSubband = sb;
    m_pDecoder->WMP.wmiSCP.uAlphaMode = decodeAlpha ? 2 : 0;
    m_decodingAlpha = decodeAlpha;

    // The assignment below is unnecessary because in this particular decoder
    // the beginnig of the image file is the beginning of the stream.
    // Keep this line as a reminder that offStart should be changed if the stream begins
    // somewhere else (for example, where the image data starts, i.e. at m_pDecoder->WMP.wmiDEMisc.uImageOffset)
    m_pDecoder->offStart = 0; // our stream starts at the beginning of the image file

    ERR err = JXR_BeginDecodingMBRows(m_pDecoder, NULL, m_mbRowBuf, m_mbRowBufStride, FALSE, failSafe ? TRUE : FALSE); // decoding into a single MB row buffer

    if (WMP_errSuccess != err)
        return;

    m_startedDecodingMBRows = true;
}

void nsJPEGXRDecoder::StartDecodingMBRows_Alpha()
{
    if (StartedDecodingMBRows_Alpha())
        return;

    m_pDecoder->WMP.wmiSCP_Alpha.pWStream = m_pStream; // !!! Must be set in m_pDecoder->Initialize() !!!
    m_pDecoder->WMP.wmiI_Alpha.cThumbnailScale = GetScale();

    size_t width, height;
    GetThumbnailSize(width, height);

    if (DecodeAtEnd())
        m_pDecoder->WMP.wmiI_Alpha.cBitsPerUnit = m_pDecoder->WMP.wmiI.cBitsPerUnit; // use the same bufer as main plane
    else
        AllocateMBRowBuffer_Alpha(width); // use an alpha-only buffer

    if (nullptr == m_mbRowBuf)
    {
        PostDecoderError(NS_ERROR_FAILURE);
        return;
    }

    m_pDecoder->WMP.wmiSCP.uAlphaMode = 0; // it does not matter for planar alpha

    // The assignment below is unnecessary because in this particular decoder
    // the beginnig of the image file is the beginning of the stream.
    // Keep this line as a reminder that offStart should be changed if the stream begins
    // somewhere else (for example, where the image data starts, i.e. at m_pDecoder->WMP.wmiDEMisc.uImageOffset)
    m_pDecoder->offStart = 0; // our stream starts at the beginning of the image file

    ERR err = JXR_BeginDecodingMBRows_Alpha(m_pDecoder, NULL, m_mbRowBuf, m_mbRowBufStride, FALSE, FALSE); // decoding into a single MB row buffer, not fail-safe

    m_startedDecodingMBRows_Alpha = WMP_errSuccess == err;
}

EXTERN_C ERR BGR24_RGB24(PKFormatConverter* pFC, const PKRect* pRect, U8* pb, U32 cbStride);
EXTERN_C ERR BGRA32_RGBA32(PKFormatConverter* pFC, const PKRect* pRect, U8* pb, U32 cbStride);

static void CMYK32_RGB32(const void *pCMYK, void *pRGB)
{
    struct CMYK
    {
        unsigned char c;
        unsigned char m;
        unsigned char y;
        unsigned char k;
    };

    struct MyRGB32
    {
        unsigned char r, g, b, x;
    };

    const CMYK &cmyk = *(const CMYK *)pCMYK;
    MyRGB32 &pix = *(MyRGB32 *)pRGB;

    double c = double(cmyk.c) / 255.0;
    double m = double(cmyk.m) / 255.0;
    double y = double(cmyk.y) / 255.0;
    double k = double(cmyk.k) / 255.0;

    double nc = (c * (1.0 - k) + k);
    double nm = (m * (1.0 - k) + k);
    double ny = (y * (1.0 - k) + k);

    double r = (1.0 - nc) * 255.0;
    double g = (1.0 - nm) * 255.0;
    double b = (1.0 - ny) * 255.0;

    pix.r = (unsigned char)(int)r;
    pix.g = (unsigned char)(int)g;
    pix.b = (unsigned char)(int)b;
}

static void CMYK32_RGB32(size_t width, size_t height, const void *pCMYK, size_t srcRowStride, void *pRGB, size_t destRowStride)
{
    const unsigned char *sl = (const unsigned char *)pCMYK;
    unsigned char *dl = (unsigned char *)pRGB;

    for (size_t i = 0; i < height; ++i)
    {
        const unsigned char *s = sl;
        unsigned char *d = dl;

        for (unsigned int j = 0; j < width; ++j)
        {
            CMYK32_RGB32(s, d);
            s += 4;
            d += 4;
        }

        sl += srcRowStride;
        dl += destRowStride;
    }
}

static void CMYKA40_RGBA32(const void *pCMYK, void *pRGB)
{
    struct CMYKA
    {
        unsigned char c;
        unsigned char m;
        unsigned char y;
        unsigned char k;
        unsigned char a;
    };

    struct MyRGBA32
    {
        unsigned char r, g, b, a;
    };

    const CMYKA &cmyk = *(const CMYKA *)pCMYK;
    MyRGBA32 &pix = *(MyRGBA32 *)pRGB;

    double c = double(cmyk.c) / 255.0;
    double m = double(cmyk.m) / 255.0;
    double y = double(cmyk.y) / 255.0;
    double k = double(cmyk.k) / 255.0;

    double nc = (c * (1.0 - k) + k);
    double nm = (m * (1.0 - k) + k);
    double ny = (y * (1.0 - k) + k);

    double r = (1.0 - nc) * 255.0;
    double g = (1.0 - nm) * 255.0;
    double b = (1.0 - ny) * 255.0;

    pix.r = (unsigned char)(int)r;
    pix.g = (unsigned char)(int)g;
    pix.b = (unsigned char)(int)b;
    pix.a = cmyk.a;
}

static void CMYK40Alpha_RGBA32(size_t width, size_t height, const void *pCMYK, size_t srcRowStride, void *pRGB, size_t destRowStride)
{
    const unsigned char *sl = (const unsigned char *)pCMYK;
    unsigned char *dl = (unsigned char *)pRGB;

    for (size_t i = 0; i < height; ++i)
    {
        const unsigned char *s = sl;
        unsigned char *d = dl;

        for (unsigned int j = 0; j < width; ++j)
        {
            CMYKA40_RGBA32(s, d);
            s += 5;
            d += 4;
        }

        sl += srcRowStride;
        dl += destRowStride;
    }
}

static void CMYKA40_RGB32(const void *pCMYK, void *pRGB)
{
    struct CMYKA
    {
        unsigned char c;
        unsigned char m;
        unsigned char y;
        unsigned char k;
        unsigned char a;
    };

    struct MyRGBA32
    {
        unsigned char r, g, b, a;
    };

    const CMYKA &cmyk = *(const CMYKA *)pCMYK;
    MyRGBA32 &pix = *(MyRGBA32 *)pRGB;

    double c = double(cmyk.c) / 255.0;
    double m = double(cmyk.m) / 255.0;
    double y = double(cmyk.y) / 255.0;
    double k = double(cmyk.k) / 255.0;

    double nc = (c * (1.0 - k) + k);
    double nm = (m * (1.0 - k) + k);
    double ny = (y * (1.0 - k) + k);

    double r = (1.0 - nc) * 255.0;
    double g = (1.0 - nm) * 255.0;
    double b = (1.0 - ny) * 255.0;

    pix.r = (unsigned char)(int)r;
    pix.g = (unsigned char)(int)g;
    pix.b = (unsigned char)(int)b;
    //pix.a = cmyk.a;
}

static void CMYK40Alpha_RGB32(size_t width, size_t height, const void *pCMYK, size_t srcRowStride, void *pRGB, size_t destRowStride)
{
    const unsigned char *sl = (const unsigned char *)pCMYK;
    unsigned char *dl = (unsigned char *)pRGB;

    for (size_t i = 0; i < height; ++i)
    {
        const unsigned char *s = sl;
        unsigned char *d = dl;

        for (unsigned int j = 0; j < width; ++j)
        {
            CMYKA40_RGB32(s, d);
            s += 5;
            d += 4;
        }

        sl += srcRowStride;
        dl += destRowStride;
    }
}

static void CMYK64_RGB32(const void *pCMYK, void *pRGB)
{
    struct CMYK
    {
        unsigned short c;
        unsigned short m;
        unsigned short y;
        unsigned short k;
    };

    struct MyRGB32
    {
        unsigned char r, g, b, x;
    };

    const CMYK &cmyk = *(const CMYK *)pCMYK;
    MyRGB32 &pix = *(MyRGB32 *)pRGB;

    double c = double(cmyk.c) / double(0xFFFF);
    double m = double(cmyk.m) / double(0xFFFF);
    double y = double(cmyk.y) / double(0xFFFF);
    double k = double(cmyk.k) / double(0xFFFF);

    double nc = (c * (1.0 - k) + k);
    double nm = (m * (1.0 - k) + k);
    double ny = (y * (1.0 - k) + k);

    double r = (1.0 - nc) * 255.0;
    double g = (1.0 - nm) * 255.0;
    double b = (1.0 - ny) * 255.0;

    pix.r = (unsigned char)(int)r;
    pix.g = (unsigned char)(int)g;
    pix.b = (unsigned char)(int)b;
}

static void CMYK64_RGB32(size_t width, size_t height, const void *pCMYK, size_t srcRowStride, void *pRGB, size_t destRowStride)
{
    const unsigned char *sl = (const unsigned char *)pCMYK;
    unsigned char *dl = (unsigned char *)pRGB;

    for (size_t i = 0; i < height; ++i)
    {
        const unsigned char *s = sl;
        unsigned char *d = dl;

        for (unsigned int j = 0; j < width; ++j)
        {
            CMYK64_RGB32(s, d);
            s += 8;
            d += 4;
        }

        sl += srcRowStride;
        dl += destRowStride;
    }
}

static void CMYKA80_RGBA32(const void *pCMYK, void *pRGB)
{
    struct CMYKA
    {
        unsigned short c;
        unsigned short m;
        unsigned short y;
        unsigned short k;
        unsigned short a;
    };

    struct MyRGBA32
    {
        unsigned char r, g, b, a;
    };

    const CMYKA &cmyk = *(const CMYKA *)pCMYK;
    MyRGBA32 &pix = *(MyRGBA32 *)pRGB;

    double c = double(cmyk.c) / double(0xFFFF);
    double m = double(cmyk.m) / double(0xFFFF);
    double y = double(cmyk.y) / double(0xFFFF);
    double k = double(cmyk.k) / double(0xFFFF);

    double nc = (c * (1.0 - k) + k);
    double nm = (m * (1.0 - k) + k);
    double ny = (y * (1.0 - k) + k);

    double r = (1.0 - nc) * 255.0;
    double g = (1.0 - nm) * 255.0;
    double b = (1.0 - ny) * 255.0;

    pix.r = (unsigned char)(int)r;
    pix.g = (unsigned char)(int)g;
    pix.b = (unsigned char)(int)b;
    pix.a = (unsigned char)(cmyk.a >> 8);
}

static void CMYK80Alpha_RGBA32(size_t width, size_t height, const void *pCMYK, size_t srcRowStride, void *pRGB, size_t destRowStride)
{
    const unsigned char *sl = (const unsigned char *)pCMYK;
    unsigned char *dl = (unsigned char *)pRGB;

    for (size_t i = 0; i < height; ++i)
    {
        const unsigned char *s = sl;
        unsigned char *d = dl;

        for (unsigned int j = 0; j < width; ++j)
        {
            CMYKA80_RGBA32(s, d);
            s += 10;
            d += 4;
        }

        sl += srcRowStride;
        dl += destRowStride;
    }
}

static void CMYKA80_RGB32(const void *pCMYK, void *pRGB)
{
    struct CMYKA
    {
        unsigned short c;
        unsigned short m;
        unsigned short y;
        unsigned short k;
        unsigned short a;
    };

    struct MyRGBA32
    {
        unsigned char r, g, b, a;
    };

    const CMYKA &cmyk = *(const CMYKA *)pCMYK;
    MyRGBA32 &pix = *(MyRGBA32 *)pRGB;

    double c = double(cmyk.c) / double(0xFFFF);
    double m = double(cmyk.m) / double(0xFFFF);
    double y = double(cmyk.y) / double(0xFFFF);
    double k = double(cmyk.k) / double(0xFFFF);

    double nc = (c * (1.0 - k) + k);
    double nm = (m * (1.0 - k) + k);
    double ny = (y * (1.0 - k) + k);

    double r = (1.0 - nc) * 255.0;
    double g = (1.0 - nm) * 255.0;
    double b = (1.0 - ny) * 255.0;

    pix.r = (unsigned char)(int)r;
    pix.g = (unsigned char)(int)g;
    pix.b = (unsigned char)(int)b;
    //pix.a = (unsigned char)(cmyk.a >> 8);
}

// Converting when alpha is not yet available (so no need to convert alpha)
static void CMYK80Alpha_RGB32(size_t width, size_t height, const void *pCMYK, size_t srcRowStride, void *pRGB, size_t destRowStride)
{
    const unsigned char *sl = (const unsigned char *)pCMYK;
    unsigned char *dl = (unsigned char *)pRGB;

    for (size_t i = 0; i < height; ++i)
    {
        const unsigned char *s = sl;
        unsigned char *d = dl;

        for (unsigned int j = 0; j < width; ++j)
        {
            CMYKA80_RGB32(s, d);
            s += 10;
            d += 4;
        }

        sl += srcRowStride;
        dl += destRowStride;
    }
}

void nsJPEGXRDecoder::ConvertAndTransform(uint8_t *pDecoded, size_t width, size_t numLines)
{
    PKRect cr;
    cr.X = 0;
    cr.Y = 0;
    cr.Height = numLines;
    cr.Width = width;

    PixelFormat outPixFmt = m_outPixelFormat;
    uint32_t outRowStride = m_mbRowBufStride;
    bool cmyk = false;

    switch (m_outPixelFormat)
    {
    case pfCMYK32:
        CMYK32_RGB32(width, numLines, pDecoded, m_mbRowBufStride, m_xfBuf, m_xfBufRowStride);
        cmyk = true;
        break;

    case pfCMYKA40:
        if (DecodingAlpha())
            CMYK40Alpha_RGBA32(width, numLines, pDecoded, m_mbRowBufStride, m_xfBuf, m_xfBufRowStride);
        else
            CMYK40Alpha_RGB32(width, numLines, pDecoded, m_mbRowBufStride, m_xfBuf, m_xfBufRowStride);

        cmyk = true;
        break;

    case pfCMYK64:
        CMYK64_RGB32(width, numLines, pDecoded, m_mbRowBufStride, m_xfBuf, m_xfBufRowStride);
        cmyk = true;
        break;

    case pfCMYKA80:
        if (DecodingAlpha())
            CMYK80Alpha_RGBA32(width, numLines, pDecoded, m_mbRowBufStride, m_xfBuf, m_xfBufRowStride);
        else
            CMYK80Alpha_RGB32(width, numLines, pDecoded, m_mbRowBufStride, m_xfBuf, m_xfBufRowStride);

        cmyk = true;
        break;
    }

    if (cmyk)
    {
        outPixFmt = m_xfPixelFormat;
        outRowStride = m_xfBufRowStride;
    }

    m_pConverter->Convert(m_pConverter, &cr, pDecoded, m_mbRowBufStride);

    // Perform color transformation if necessary
    if (m_transform)
    {
        if (nullptr != m_xfBuf)
        {
            const uint8_t *pSrc = pDecoded;
            uint8_t *pDest = m_xfBuf;

            for (size_t i = 0; i < numLines; ++i)
            {
                qcms_transform_data(m_transform, (void *)pSrc, pDest, width);
                pSrc += m_mbRowBufStride;
                pDest += m_xfBufRowStride;
            }

            outPixFmt = m_xfPixelFormat;
            outRowStride = m_xfBufRowStride;
        }
        else
        {
            // Perform in-place pixel format conversion if necessary
            switch (m_xfPixelFormat)
            {
            case pfRGB24:
                BGR24_RGB24(nullptr, &cr, pDecoded, m_mbRowBufStride);
                outPixFmt = m_xfPixelFormat;
                break;

            case pfRGB32:
            case pfRGBA32:
                BGRA32_RGBA32(nullptr, &cr, pDecoded, m_mbRowBufStride);
                outPixFmt = m_xfPixelFormat;
                break;
            }

            uint8_t *pLine = pDecoded;

            // Color image
            for (size_t i = 0; i < numLines; ++i)
            {
                qcms_transform_data(m_transform, pLine, pLine, width);
                pLine += m_mbRowBufStride;
            }
        }
    }
}

bool nsJPEGXRDecoder::DecodeNextMBRow(bool invalidate, bool output)
{
    if (FinishedDecodingMainPlane())
        return false;

    size_t width = m_pDecoder->WMP.wmiI.cThumbnailWidth;
    size_t height = m_pDecoder->WMP.wmiI.cThumbnailHeight;

    if (m_currLine >= height)
        return false;

    size_t numLinesDecoded;
    Bool finished;
    //ERR err = 
        JXR_DecodeNextMBRow(m_pDecoder, m_mbRowBuf, m_mbRowBufStride, &numLinesDecoded, &finished);

    if (0 == numLinesDecoded)
        return false;

    ConvertAndTransform(m_mbRowBuf, width, numLinesDecoded);

    size_t top = m_currLine;
    m_currLine += numLinesDecoded;

    if (output)
    {
        UpdateImage(top, width, numLinesDecoded);

        // Invalidate the rectangle
        if (invalidate)
        {
            nsIntRect r(0, top, (uint32_t)width, numLinesDecoded);
            PostInvalidation(r);
        }
    }

    return true;
}

bool nsJPEGXRDecoder::DecodeNextMBRow_Alpha(bool invalidate)
{
    size_t width = m_pDecoder->WMP.wmiI_Alpha.cThumbnailWidth;
    size_t height = m_pDecoder->WMP.wmiI_Alpha.cThumbnailHeight;

    if (m_currLine >= height)
        return false;

    size_t numLinesDecoded;
    Bool finished;
    //ERR err = 
        JXR_DecodeNextMBRow_Alpha(m_pDecoder, m_mbRowBuf, m_mbRowBufStride, &numLinesDecoded, &finished);

    if (0 == numLinesDecoded)
        return false;

    PKRect cr;
    cr.X = 0;
    cr.Y = 0;
    cr.Height = numLinesDecoded;
    cr.Width = width;

    m_pConverter->Convert(m_pConverter, &cr, m_mbRowBuf, m_mbRowBufStride);

    UpdateImage_AlphaOnly(m_currLine, width, numLinesDecoded);

    if (invalidate)
    {
        nsIntRect r(0, m_currLine, width, numLinesDecoded);
        PostInvalidation(r);
    }

    m_currLine += numLinesDecoded;

    return true;
}

// Decoding with planar alpha. No need to invalidate macroblock rows.
bool nsJPEGXRDecoder::DecodeNextMBRowWithAlpha()
{
    size_t width = m_pDecoder->WMP.wmiI_Alpha.cThumbnailWidth;
    size_t height = m_pDecoder->WMP.wmiI_Alpha.cThumbnailHeight;

    if (m_currLine >= height)
        return false;

    size_t numLinesDecoded, numLinesDecoded1;
    Bool finished;
    //ERR err = 
        JXR_DecodeNextMBRow(m_pDecoder, m_mbRowBuf, m_mbRowBufStride, &numLinesDecoded, &finished);
    JXR_DecodeNextMBRow_Alpha(m_pDecoder, m_mbRowBuf, m_mbRowBufStride, &numLinesDecoded1, &finished);

    if (0 == numLinesDecoded || numLinesDecoded != numLinesDecoded1)
        return false;

    ConvertAndTransform(m_mbRowBuf, width, numLinesDecoded);

    UpdateImage(m_currLine, width, numLinesDecoded);
    m_currLine += numLinesDecoded;

    return true;
}

void nsJPEGXRDecoder::UpdateImage(size_t top, size_t width, size_t height)
{
    const uint8_t *pSrc;
    size_t srcRowStride;
    PixelFormat pixFmt;

    if (nullptr != m_xfBuf)
    {
        pSrc = m_xfBuf;
        srcRowStride = m_xfBufRowStride;
        pixFmt = m_xfPixelFormat;
    }
    else
    {
        pSrc = m_mbRowBuf;
        srcRowStride = m_mbRowBufStride;
        pixFmt = pfNone != m_xfPixelFormat ? m_xfPixelFormat : m_outPixelFormat;
    }

    switch (pixFmt)
    {
    case pfGray:
        {
            const uint8_t *sl = pSrc;

            for (size_t i = 0; i < height; ++i)
            {
                const uint8_t *s = sl;
                uint32_t *dl = GetPixel(0, top + i);

                for (size_t j = 0; j < width; ++j)
                {
                    const uint8_t alpha = 0xFF;
                    SetPixel(dl + j, gfxPackedPixelNoPreMultiply(alpha, *s, *s, *s));
                    ++s;
                }

                sl += srcRowStride;
            }
        }
        break;

    case pfRGB24:
    case pfRGB32:
        {
            struct RGB
            {
                uint8_t r, g, b;
            };

            const uint8_t *sl = pSrc;
            const uint32_t srcBPP = pfRGB24 == pixFmt ? 3 : 4;

            for (size_t i = 0; i < height; ++i)
            {
                const uint8_t *s = sl;
                uint32_t *dl = GetPixel(0, top + i);

                for (size_t j = 0; j < width; ++j)
                {
                    const RGB &sp = *(const RGB *)s;
                    s += srcBPP;
                    const uint8_t alpha = 0xFF;
                    SetPixel(dl + j, gfxPackedPixelNoPreMultiply(alpha, sp.r, sp.g, sp.b));
                }

                sl += srcRowStride;
            }
        }
        break;

    case pfRGBA32:
        {
            struct RGBA
            {
                uint8_t r, g, b, a;
            };

            const uint8_t *sl = pSrc;

            for (size_t i = 0; i < height; ++i)
            {
                const uint8_t *s = sl;
                uint32_t *dl = GetPixel(0, top + i);

                for (size_t j = 0; j < width; ++j)
                {
                    const RGBA &sp = *(const RGBA *)s;
                    s += 4;
                    SetPixelWithAlpha(dl + j, gfxPackedPixelNoPreMultiply(sp.a, sp.r, sp.g, sp.b), j, top + i);
                }

                sl += srcRowStride;
            }
        }
        break;

    case pfBGR24:
    case pfBGR32:
        {
            struct BGR
            {
                uint8_t b, g, r;
            };

            const uint8_t *sl = pSrc;
            const uint32_t srcBPP = pfBGR24 == pixFmt ? 3 : 4;

            for (size_t i = 0; i < height; ++i)
            {
                const uint8_t *s = sl;
                uint32_t *dl = GetPixel(0, top + i);

                for (size_t j = 0; j < width; ++j)
                {
                    const BGR &sp = *(const BGR *)s;
                    s += srcBPP;
                    const uint8_t alpha = 0xFF;
                    SetPixel(dl + j, gfxPackedPixelNoPreMultiply(alpha, sp.r, sp.g, sp.b));
                }

                sl += srcRowStride;
            }
        }
        break;

    case pfBGRA32:
        {
            struct BGRA
            {
                uint8_t b, g, r, a;
            };

            const uint8_t *sl = pSrc;

            for (size_t i = 0; i < height; ++i)
            {
                const uint8_t *s = sl;
                uint32_t *dl = GetPixel(0, top + i);

                for (size_t j = 0; j < width; ++j)
                {
                    const BGRA &sp = *(const BGRA *)s;
                    s += 4;
                    SetPixelWithAlpha(dl + j, gfxPackedPixelNoPreMultiply(sp.a, sp.r, sp.g, sp.b), j, top + i);
                }

                sl += srcRowStride;
            }
        }
        break;
    }
}

void nsJPEGXRDecoder::UpdateImage_AlphaOnly(size_t top, size_t width, size_t height)
{
    const uint8_t *sl = m_mbRowBuf;
    size_t srcRowStride = m_mbRowBufStride;

    for (size_t i = 0; i < height; ++i)
    {
        const uint8_t *s = sl;
        uint32_t *dl = GetPixel(0, top + i);

        for (size_t j = 0; j < width; ++j)
        {
            uint8_t a = *s;
            ++s;
            uint32_t dp = dl[j];
            SetPixelWithAlpha(dl + j, gfxPackedPixel(a, RasterImage::GetPixelR(dp), RasterImage::GetPixelG(dp), RasterImage::GetPixelB(dp)), j, top + i);
        }

        sl += srcRowStride;
    }
}

void nsJPEGXRDecoder::DecodeAllMBRows()
{
    size_t width, height;
    GetSize(width, height);

    for (;;)
    {
        bool decoded = DecodeNextMBRow(false);

        if (!decoded || m_currLine >= height)
            break;
    }

    // Invalidate
    nsIntRect r(0, 0, width, height);
    PostInvalidation(r);
}

// Decode all macroblock rows with planar alpha
void nsJPEGXRDecoder::DecodeAllMBRowsWithAlpha()
{
    size_t width, height;
    GetSize(width, height);

    for (;;)
    {
        bool decoded = DecodeNextMBRowWithAlpha();

        if (!decoded || m_currLine >= height)
            break;
    }

    // Invalidate
    nsIntRect r(0, 0, width, height);
    PostInvalidation(r);
}

void nsJPEGXRDecoder::DecodeAllMBRows_Alpha()
{
    size_t width, height;
    GetSize(width, height);

    for (;;)
    {
        bool decoded = DecodeNextMBRow_Alpha(false);

        if (!decoded || m_currLine >= height)
            break;
    }

    // Invalidate
    nsIntRect r(0, 0, width, height);
    PostInvalidation(r);
}

void nsJPEGXRDecoder::DecodeNextTileRow()
{
    size_t width, height;
    //GetSize(width, height);
    GetThumbnailSize(width, height);
    size_t rowTop, rowHeight;

    if (IsProgressiveDecoding())
    {
        const TileRowBandInfo &tileRowBandInfo = m_tileRowBandInfos[GetCurrentTileRow()];
        rowTop = tileRowBandInfo.top;
        rowHeight = tileRowBandInfo.height;
    }
    else
    {
        const TileRowInfo &tileRowInfo = m_tileRowInfos[GetCurrentTileRow()];
        rowTop = tileRowInfo.top;
        rowHeight = tileRowInfo.height;
    }

    // Since we skipped tile rows, we do not want to output the first row
    // (which is in fact the last row of the preceding tile row)
    // because it will be a wrong one (the last macroblock row of the tile row preceding the first skipped one)
    bool output = !SkippedTileRows();

    for (;;)
    {
        bool decoded = DecodeNextMBRow(false, output);
        output = true;

        if (!decoded || m_currLine >= rowTop + rowHeight)
            break;
    }

    if (SkippedTileRows())
    {
        m_skippedTileRows = false;
        const size_t MBR_HEIGHT = 16;
        rowTop += MBR_HEIGHT / m_pDecoder->WMP.wmiI.cThumbnailScale;
    }

    // Invalidate
    nsIntRect r(0, rowTop, width, m_currLine - rowTop);
    PostInvalidation(r);
}

void nsJPEGXRDecoder::EndDecodingMBRows()
{
    if (!StartedDecodingMBRows())
        return;

    JXR_EndDecodingMBRows(m_pDecoder);
    m_startedDecodingMBRows = false;
    m_currLine = 0;
    FreeMBRowBuffers();
}

void nsJPEGXRDecoder::EndDecodingMBRows_Alpha()
{
    if (!StartedDecodingMBRows_Alpha())
        return;

    JXR_EndDecodingMBRows_Alpha(m_pDecoder);
    m_startedDecodingMBRows_Alpha = false;
    m_currLine = 0;
    FreeMBRowBuffers();
}

void nsJPEGXRDecoder::FreeMBRowBuffers()
{
    if (nullptr != m_mbRowBuf)
    {
        PKFreeAligned((void **)&m_mbRowBuf);
        m_mbRowBuf = nullptr;
        m_mbRowBufStride = 0;
    }

    if (nullptr != m_xfBuf)
    {
        moz_free(m_xfBuf);
        m_xfBuf = nullptr;
    }
}

void nsJPEGXRDecoder::InitInternal()
{
    if (!CreateJXRStuff())
        PostDecoderError(NS_ERROR_FAILURE);
}

void nsJPEGXRDecoder::CreateColorTransform()
{
    m_transform = new char[1]; // !!!Just a dummy !!!
}

void nsJPEGXRDecoder::FixWrongImageSizeTag(size_t maxSize)
{
    U32 byteCount = maxSize - m_pDecoder->WMP.wmiDEMisc.uImageOffset;
    m_pDecoder->WMP.wmiDEMisc.uImageByteCount = byteCount;
    m_pDecoder->WMP.wmiI.uImageByteCount = byteCount;

    // Fix the tile row info if exists
    if (nullptr != m_tileRowInfos)
    {
        for (size_t i = 0; i < GetNumTileRows(); ++i)
        {
            TileRowInfo &info = m_tileRowInfos[i];

            // Workaround for the bug with wrong uImageByteCount in the header
            if (info.upperLimit > maxSize)
                info.upperLimit = maxSize;

            if (info.nextLowerLimit > maxSize)
                info.nextLowerLimit = maxSize;
        }
    }
    // Fix the tile row band info if exists
    else if (nullptr != m_tileRowBandInfos)
    {
        for (size_t i = 0; i < GetNumTileRows(); ++i)
        {
            TileRowBandInfo &info = m_tileRowBandInfos[i];

            for (size_t j = 0; j < GetNumAvailableBands(); ++j)
            {
                if (info.upperLimits[j] > maxSize)
                    info.upperLimits[j] = maxSize;
            }
        }
    }

    if (!FinishedDecodingMainPlane())
        SetCurrentSubbandUpperLimit(m_pDecoder->WMP.ctxSC, maxSize); // will taek care of all other cases and will not harm the above two ones
}

#define DISCARD_HEAD
EXTERN_C Bool SetCurrentTileRow(CTXSTRCODEC ctxSC, size_t tileRow);

void nsJPEGXRDecoder::DoTheDecoding()
{
    if (IsProgressiveDecoding())
    {
NextSubBand:
        if (!StartedDecodingSubband())
            StartDecodingNextSubband();

        // Decode at least one tile row
        size_t numCoveredSubBands = GetNumberOfCoveredSubBands();
        bool bandDecoded = false;

        if (numCoveredSubBands > GetCurrentSubBand() && !(GetCurrentSubBand() + 1 == numCoveredSubBands && 
            GetCurrentTileRow() > 0) && !HasEmptyTileRowSubbands(GetCurrentSubBand()))
        {
            if (numCoveredSubBands > GetCurrentSubBand() + 1)
            {
                // Re-initialize the decoder to decode the available frequency bands
                EndDecodingCurrentSubband();
                m_currentSubBand = numCoveredSubBands - 1;
                StartDecodingNextSubband();
            }

            // We are going to decode all tile rows at once, so set the upper limit of the entire subband
            SetCurrentSubbandUpperLimit(m_pDecoder->WMP.ctxSC, GetCurrentSubbandUpperLimit());
            DecodeAllMBRows();
            m_currentSubBand = numCoveredSubBands;
            bandDecoded = true;
        }
        else if (GetNumTileRows() > 1)
        {
            // Decode as many tile rows as possible
            for (; ;)
            {
                size_t upperLimit = GetCurrentSubbandTileRowUpperLimit();

                // upperLimit == 0 means the subband is empty for this tile row.
                if (0 == upperLimit || GetTotalNumBytesReceived() >= upperLimit)
                {
                    // A little optimisation - see whether we can avoid decoding of the entire tile row
                    bool canSkip = false;

                    if (0 == upperLimit)
                    {
                        if (m_pDecoder->WMP.wmiSCP.bUseHardTileBoundaries || 0 == m_pDecoder->WMP.wmiSCP.olOverlap)
                        {
                            // Either "hard" tiles or over tile boundaries overlap filtering is not used
                            canSkip = true;
                        }
                        else
                        {
                            // If not "hard" tiles or over tile boundaries overlap filtering is used, 
                            // skip only if the next tile row also does not have this subband
                            if (GetCurrentTileRow() < GetNumTileRows() - 1)
                            {
                                TileRowBandInfo &nextInfo = m_tileRowBandInfos[GetCurrentTileRow() + 1];
                                canSkip = 0 == nextInfo.upperLimits[GetCurrentSubBand()];
                            }
                        }
                    }

                    if (canSkip)
                    {
                        // The subband is empty for this tile row, and we can safely skip it.
                        // Decoding this tile row again with an empty subband would make no sense because the output would 
                        // be exactly the same as after decoding the previous subband
                        // This won't take place with DC and LP subbands, so we don't care whether we have "soft" or "hard" tiles
                        if (GetCurrentTileRow() < GetNumTileRows() - 1)
                        {
                            TileRowBandInfo &nextInfo = m_tileRowBandInfos[GetCurrentTileRow() + 1];
                            m_pDecoder->WMP.DecoderCurrMBRow = nextInfo.topMBRow + 1;
                            SetCurrentTileRow(m_pDecoder->WMP.ctxSC, GetCurrentTileRow() + 1);
                            m_currLine = nextInfo.top;

                            // !!!We should instruct the JXRLib decoder not to perform output at all. 
                            // I.e. the decoder should not call Load() method. For now just do not update 
                            // the corresponding rows in Firefox' image!!!
                            m_skippedTileRows = true;
                        }
                    }
                    else
                    {
                        // Ready to decode next subband in one pass without backing up/restoring the decoder state
                        SetCurrentSubbandUpperLimit(m_pDecoder->WMP.ctxSC, upperLimit);
                        DecodeNextTileRow();
                    }

                    ++m_currentTileRow;

                    if (GetCurrentTileRow() >= GetNumTileRows())
                    {
                        bandDecoded = true;
                        ++m_currentSubBand;
                        break;
                    }
                }
                else
                    break;
            }
        }

        if (bandDecoded)
        {
            if (GetCurrentSubBand() >= GetNumAvailableBands())
            {
                EndDecodingMBRows();
                m_mainPlaneFinished = true;
                return; // there are no more bands for progressive and sequential decoding
            }

            EndDecodingCurrentSubband();
            goto NextSubBand;
        }
    }
    else
    {
        if (SPATIAL == m_pDecoder->WMP.wmiSCP.bfBitstreamFormat && 1 == GetNumTileCols())
        {
            // fail-safe macroblock row-by-row decoding
            for (; ;)
            {
                size_t currMBRow = m_pDecoder->WMP.DecoderCurrMBRow;
                bool decoded = DecodeNextMBRow(true);

                if (decoded)
                {
#ifdef DISCARD_HEAD
                    // Now we can discard the head of the stream that has been decoded.
                    // We use the fact that in SPATIAL mode there is only one bitIO object per tile.
                    // Again, since there is no macrobloc row index table, we have to use a complex rule to 
                    // determine the how many bytes we can safely discard at the head of the stream.
                    size_t pos;
                    ERR err = m_pStream->GetPos(m_pStream, &pos);

                    if (WMP_errSuccess == err)
                    {
                        if (nullptr != m_tileRowInfos)
                        {
                            size_t currMBRowTileRow = GetTileRowForMBRow(currMBRow);
                            size_t nextLowerLimit = m_tileRowInfos[currMBRowTileRow].nextLowerLimit;

                            if (pos > nextLowerLimit)
                                pos = nextLowerLimit;
                        }

                        size_t discarded;
                        m_pStream->DiscardHead(m_pStream, pos, &discarded);
                    }
#endif
                }
                else
                    break;
            }
        }
        else // multiple tiles, either SPATIAL or FREQUENCY layout
        {
            for (; ;)
            {
                // For tile rows consisting of 1 macrobock row, wait until the next tile row arrives
                // (unfortunately, we don't know the size of the next macroblock row because there is no macroblock index table)
                const TileRowInfo &info = m_tileRowInfos[GetCurrentTileRow()];
                size_t tileRowUpperLimit = 1 == info.numMBRows ? m_tileRowInfos[GetCurrentTileRow() + 1].upperLimit : info.upperLimit;

                if (GetTotalNumBytesReceived() < tileRowUpperLimit)
                    break;

                SetCurrentSubbandUpperLimit(m_pDecoder->WMP.ctxSC, tileRowUpperLimit);
                DecodeNextTileRow();
                ++m_currentTileRow;

#ifdef DISCARD_HEAD
                // Now we can safely discard the head of the stream and free some heap
                size_t discarded;
                m_pStream->DiscardHead(m_pStream, info.nextLowerLimit, &discarded);
#endif

                if (GetCurrentTileRow() >= GetNumTileRows())
                {
                    m_mainPlaneFinished = true;
                    return; // there are no more tile rows for sequential decoding
                }
            }
        }
    }
}

void nsJPEGXRDecoder::WriteInternal(const char *aBuffer, uint32_t aCount)//, DecodeStrategy)
{
    //@NS_ABORT_IF_FALSE(!HasError(), "Shouldn't call WriteInternal after error!");

    // aCount == 0 means end of image data.
    // In reality, it seems that the function is never called with aCount == 0. Leave it here just in case.
    if (0 == aCount)
    {
        if (!DecoderInitialized() || FinishedDecodingMainPlane())
            return;

        // Workaround for wrong image byte count in the header. This can take place in old images created with a 
        // buggy version of JXRLib
        if (GetTotalNumBytesReceived() < m_pDecoder->WMP.wmiDEMisc.uImageOffset + m_pDecoder->WMP.wmiDEMisc.uImageByteCount)
            FixWrongImageSizeTag(GetTotalNumBytesReceived());
        else
            return;
    }
    else
        Receive((const unsigned char *)aBuffer, aCount);

    if (FinishedDecodingMainPlane())
        return;

    if (!DecoderInitialized())
    {
        m_pStream->SetPos(m_pStream, 0);
        InitializeJXRDecoder();

        if (!DecoderInitialized())
            return;

        size_t width, height;
        //GetSize(width, height);
        GetThumbnailSize(width, height);

        // Let Firefox handle the desired orientation
        m_pDecoder->WMP.wmiI.oOrientation = O_NONE;

#if 0
        if (m_pDecoder->WMP.fOrientationFromContainer && O_NONE != m_pDecoder->WMP.oOrientationFromContainer)
        {

            /* JXRLIB orientation
            // rotation and flip
            typedef enum ORIENTATION {
                // CRW: Clock Wise 90% Rotation; FlipH: Flip Horizontally;  FlipV: Flip Vertically
                // Peform rotation FIRST!
                //                CRW FlipH FlipV
                O_NONE = 0,    // 0    0     0
                O_FLIPV,       // 0    0     1
                O_FLIPH,       // 0    1     0
                O_FLIPVH,      // 0    1     1
                O_RCW,         // 1    0     0
                O_RCW_FLIPV,   // 1    0     1
                O_RCW_FLIPH,   // 1    1     0
                O_RCW_FLIPVH,  // 1    1     1
                // add new ORIENTATION here
                O_MAX
            } ORIENTATION;
                    */
            /* Mozilla orientation
             * A struct that describes an image's orientation as a rotation optionally
             * followed by a reflection. This may be used to be indicate an image's inherent
             * orientation or a desired orientation for the image.
            MOZ_BEGIN_ENUM_CLASS(Angle, uint8_t)
              D0,
              D90,
              D180,
              D270
            MOZ_END_ENUM_CLASS(Angle)

            MOZ_BEGIN_ENUM_CLASS(Flip, uint8_t)
              Unflipped,
              Horizontal
            MOZ_END_ENUM_CLASS(Flip)
            */
            Angle angle = Angle::D0;
            Flip flip = Flip::Unflipped;

            // Mozilla angles are CW (clockwise)
            switch (m_pDecoder->WMP.oOrientationFromContainer)
            {
                                // CWR FlipH FlipV
            case O_FLIPV:       //  0    0     1
                angle = Angle::D180;
                flip = Flip::Horizontal;
                break;

            case O_FLIPH:       //  0    1     0
                //angle = Angle::D0;
                flip = Flip::Horizontal;
                break;

            case O_FLIPVH:      //  0    1     1
                angle = Angle::D180;
                //flip = Flip::Unflipped;
                break;

            case O_RCW:         //  1    0     0
                angle = Angle::D90;
                //flip = Flip::Unflipped;
                break;

            case O_RCW_FLIPV:   //  1    0     1
                angle = Angle::D270;
                flip = Flip::Horizontal;
                break;

            case O_RCW_FLIPH:   //  1    1     0
                angle = Angle::D90;
                flip = Flip::Horizontal;
                break;

            case O_RCW_FLIPVH:  //  1    1     1
                angle = Angle::D270;
                //flip = Flip::Unflipped;
                break;
            }

            Orientation mozOrient(angle, flip);
            PostSize(width, height, mozOrient);
        }
        else
#endif
            PostSize(width, height);

        if (HasError())
        {
            // Setting the size led to an error.
            return;
        }

        if (GetTotalNumBytesReceived() >= m_pDecoder->WMP.wmiDEMisc.uImageOffset + m_pDecoder->WMP.wmiDEMisc.uImageByteCount)
            m_decodeAtEnd = true;

        // We have the size. If we're doing a size decode, we got what
        // we came for.
        if (IsSizeDecode())
            return;
    }

    if (DecodeAtEnd())
    {
        // For now just accumulate the image data. The image will be decoded in FinishInternal().
        // Later this can be optimized either to use a one-piece source buffer (rather than a chain one).
        return;
    }

    if (!StartedDecodingMBRows())
    {
        bool spatial = SPATIAL == m_pDecoder->WMP.wmiSCP.bfBitstreamFormat;
        bool interleavedAlpha = HasAlpha() && !HasPlanarAlpha();

        // Try to do progressive decoding
        if (!spatial)
            StartProgressiveDecoding(interleavedAlpha);

        if (!StartedDecodingMBRows())
        {
            // Do macroblock row-by-row decoding only if there is only one tile column. Otherwise, decode one tile row at a time
            // after we accumulated enough data. Fail-safe is needed because we do not know how many bytes a macroblock takes.
            bool failSafe = SPATIAL == m_pDecoder->WMP.wmiSCP.bfBitstreamFormat && 1 == GetNumTileCols();
            StartDecodingMBRows(failSafe, interleavedAlpha);
        }

        if (!StartedDecodingMBRows())
            return;

#ifdef DISCARD_HEAD
        // We can now safely discard EXIF and other metadata at the beginning of the image, if any
        size_t discarded;
        m_pStream->DiscardHead(m_pStream, m_pDecoder->WMP.wmiDEMisc.uImageOffset, &discarded);
#endif

        if (IsProgressiveDecoding() || GetNumTileRows() > 1 || GetNumTileCols() > 1)
        {
            bool res = IsProgressiveDecoding() ? FillTileRowBandInfo() : FillTileRowInfo();

            if (!res)
            {
                EndDecodingMBRows();
                PostDecoderError(NS_ERROR_FAILURE);
                return;
            }
        }
    }

    DoTheDecoding();
}

void nsJPEGXRDecoder::FinishInternal()
{
    // We shouldn't be called in error cases
    //@NS_ABORT_IF_FALSE(!HasError(), "Can't call FinishInternal on error!");

    // We should never make multiple frames
    //NS_ABORT_IF_FALSE(GetFrameCount() <= 1, "Multiple JPEG-XR frames?");

    // Send notifications if appropriate
    if (!IsSizeDecode() && HasSize())
    {
        if (DecodeAtEnd())
        {
            StartDecodingMBRows(false, HasAlpha());

            if (HasPlanarAlpha())
            {
                // Circumvent the bug in JXELIB's JPEG-XR encoder that writes wrong alpha plane byte count.
                // Adjust the alpha plane byte count if the value is wrong.
                if (m_pDecoder->WMP.wmiDEMisc.uAlphaOffset + m_pDecoder->WMP.wmiI_Alpha.uImageByteCount > GetTotalNumBytesReceived())
                    m_pDecoder->WMP.wmiI_Alpha.uImageByteCount = GetTotalNumBytesReceived() - m_pDecoder->WMP.wmiDEMisc.uAlphaOffset;

                StartDecodingMBRows_Alpha();
                DecodeAllMBRowsWithAlpha();
                EndDecodingMBRows_Alpha();
            }
            else
                DecodeAllMBRows();

            EndDecodingMBRows();
        }
        else
        {
            if (DecoderInitialized() || !FinishedDecodingMainPlane())
            {
                // This can happen if the image plane size tag is wrong, so we have not decoded the remaining MB rows before 
                // WriteInternal() was called the last time (and it looks like it is never called with aByteCount == 0). 
                // We have to finish the decoding here.

                // Workaround for wrong image byte count in the header. This can take place in old images created with a 
                // buggy version of JXRLib
                if (GetTotalNumBytesReceived() < m_pDecoder->WMP.wmiDEMisc.uImageOffset + m_pDecoder->WMP.wmiDEMisc.uImageByteCount)
                {
                    FixWrongImageSizeTag(GetTotalNumBytesReceived());
                    DoTheDecoding();
                }
            }

            EndDecodingMBRows();

            if (HasPlanarAlpha() && m_pDecoder->WMP.wmiDEMisc.uAlphaOffset < GetTotalNumBytesReceived())
            {
                // We can now safely discard main image
                size_t discarded;
                m_pStream->DiscardHead(m_pStream, m_pDecoder->WMP.wmiDEMisc.uAlphaOffset, &discarded);

                // Circumvent the bug in JXELIB's JPEG-XR encoder that writes wrong alpha plane byte count.
                // Adjust the alpha plane byte count if the value is wrong.
                if (m_pDecoder->WMP.wmiDEMisc.uAlphaOffset + m_pDecoder->WMP.wmiI_Alpha.uImageByteCount > GetTotalNumBytesReceived())
                    m_pDecoder->WMP.wmiI_Alpha.uImageByteCount = GetTotalNumBytesReceived() - m_pDecoder->WMP.wmiDEMisc.uAlphaOffset;

                StartDecodingMBRows_Alpha();
                SetCurrentSubbandUpperLimit(m_pDecoder->WMP.ctxSC, 0);
                SetCurrentSubbandUpperLimit(m_pDecoder->WMP.ctxSC_Alpha, 0);

                if (StartedDecodingMBRows_Alpha())
                {
                    DecodeAllMBRows_Alpha();
                    EndDecodingMBRows_Alpha();
                }
            }
        }

        //if (mUseAlphaData)
        //{
        //    PostFrameStop(FrameBlender::kFrameHasAlpha);
        //}
        //else
        //{
        //    PostFrameStop(FrameBlender::kFrameOpaque);
        //}

        PostDecodeDone();
    }
}
