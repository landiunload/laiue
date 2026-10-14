#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>

#include "render/ui_image_wic.h"

#include <stddef.h>

bool UiImageLoadRgba(const wchar_t* path, uint8_t** outPixels,
    uint32_t* outWidth, uint32_t* outHeight)
{
    if (path == NULL || outPixels == NULL || outWidth == NULL
        || outHeight == NULL)
    {
        return false;
    }
    *outPixels = NULL;
    *outWidth = 0;
    *outHeight = 0;

    HRESULT initializeResult = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool uninitialize = initializeResult == S_OK
        || initializeResult == S_FALSE;
    if (FAILED(initializeResult)
        && initializeResult != RPC_E_CHANGED_MODE)
    {
        return false;
    }

    IWICImagingFactory* factory = NULL;
    IWICBitmapDecoder* decoder = NULL;
    IWICBitmapFrameDecode* frame = NULL;
    IWICFormatConverter* converter = NULL;
    uint8_t* pixels = NULL;
    bool succeeded = false;

    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL,
            CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory,
            (void**)&factory)))
    {
        goto cleanup;
    }
    if (FAILED(IWICImagingFactory_CreateDecoderFromFilename(factory,
            path, NULL, GENERIC_READ, WICDecodeMetadataCacheOnLoad,
            &decoder))
        || FAILED(IWICBitmapDecoder_GetFrame(decoder, 0, &frame)))
    {
        goto cleanup;
    }

    UINT width = 0;
    UINT height = 0;
    if (FAILED(IWICBitmapFrameDecode_GetSize(frame, &width, &height))
        || width == 0 || height == 0 || width > 16384U || height > 16384U
        || (uint64_t)width * (uint64_t)height > SIZE_MAX / 4U)
    {
        goto cleanup;
    }
    if (FAILED(IWICImagingFactory_CreateFormatConverter(factory, &converter))
        || FAILED(IWICFormatConverter_Initialize(converter,
            (IWICBitmapSource*)frame, &GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone, NULL, 0.0,
            WICBitmapPaletteTypeCustom)))
    {
        goto cleanup;
    }

    uint32_t stride = (uint32_t)width * 4U;
    size_t byteCount = (size_t)stride * (size_t)height;
    pixels = HeapAlloc(GetProcessHeap(), 0, byteCount);
    if (pixels == NULL
        || FAILED(IWICFormatConverter_CopyPixels(converter, NULL, stride,
            (UINT)byteCount, pixels)))
    {
        goto cleanup;
    }

    *outPixels = pixels;
    *outWidth = width;
    *outHeight = height;
    pixels = NULL;
    succeeded = true;

cleanup:
    if (pixels != NULL) HeapFree(GetProcessHeap(), 0, pixels);
    if (converter != NULL) IWICFormatConverter_Release(converter);
    if (frame != NULL) IWICBitmapFrameDecode_Release(frame);
    if (decoder != NULL) IWICBitmapDecoder_Release(decoder);
    if (factory != NULL) IWICImagingFactory_Release(factory);
    if (uninitialize) CoUninitialize();
    return succeeded;
}
