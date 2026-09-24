#include "png_texture.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstring>
#include <limits>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace ds2::modding {
namespace {
using Microsoft::WRL::ComPtr;
ByteLoadResult Invalid(const wchar_t* detail) {
    ByteLoadResult r; r.error = ByteLoadError::read_failed;
    r.win32_error = ERROR_INVALID_DATA; r.detail = detail; return r;
}
void Put(std::vector<std::byte>& b, std::size_t at, std::uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) b[at+i] = std::byte((n >> (i*8)) & 255);
}
}
ByteLoadResult DecodePngTexture(std::span<const std::byte> png, std::uint64_t max_bytes) {
    constexpr unsigned char magic[]{137,80,78,71,13,10,26,10};
    if (png.size() < 33 || png.size() > MAXDWORD ||
        std::memcmp(png.data(), magic, sizeof(magic)) != 0)
        return Invalid(L"invalid PNG signature or length");
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) return Invalid(L"COM initialization failed");
    struct ComScope { bool owned; ~ComScope() { if (owned) CoUninitialize(); } } scope{SUCCEEDED(init)};
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory))) || FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(
            reinterpret_cast<BYTE*>(const_cast<std::byte*>(png.data())), static_cast<DWORD>(png.size()))) ||
        FAILED(factory->CreateDecoder(GUID_ContainerFormatPng, nullptr, &decoder)) ||
        FAILED(decoder->Initialize(stream.Get(), WICDecodeMetadataCacheOnDemand)) ||
        FAILED(decoder->GetFrame(0, &frame))) return Invalid(L"PNG decoding failed");
    UINT w{}, h{}, frames{};
    if (FAILED(decoder->GetFrameCount(&frames)) || frames != 1 ||
        FAILED(frame->GetSize(&w, &h)) || !w || !h || w > 16384 || h > 16384)
        return Invalid(L"PNG dimensions or frame count unsupported");
    std::uint64_t size = 128;
    UINT mips = 0;
    for (UINT mw=w, mh=h;; mw=(std::max)(1u,mw/2), mh=(std::max)(1u,mh/2)) {
        size += std::uint64_t(mw)*mh*4; ++mips;
        if (mw == 1 && mh == 1) break;
    }
    if (size > max_bytes || size > MAXDWORD) {
        auto r = Invalid(L"decoded PNG mip chain exceeds asset byte budget");
        r.error = ByteLoadError::file_too_large; return r;
    }
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        return Invalid(L"PNG BGRA conversion failed");
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    Put(bytes,0,0x20534444); Put(bytes,4,124); Put(bytes,8,0x2100F);
    Put(bytes,12,h); Put(bytes,16,w); Put(bytes,20,w*4); Put(bytes,28,mips);
    Put(bytes,76,32); Put(bytes,80,0x41); Put(bytes,88,32);
    Put(bytes,92,0xFF0000); Put(bytes,96,0xFF00); Put(bytes,100,0xFF); Put(bytes,104,0xFF000000);
    Put(bytes,108,mips > 1 ? 0x401008 : 0x1000);
    if (FAILED(converter->CopyPixels(nullptr,w*4,w*h*4,
            reinterpret_cast<BYTE*>(bytes.data()+128)))) return Invalid(L"PNG pixel decode failed");
    std::size_t previous = 128, next = 128 + std::size_t(w)*h*4;
    for (UINT level=1; level<mips; ++level) {
        const UINT nw=(std::max)(1u,w/2), nh=(std::max)(1u,h/2);
        for (UINT y=0; y<nh; ++y) for (UINT x=0; x<nw; ++x) {
            // Area boxes include the last row/column of odd dimensions.
            const UINT x0=x*w/nw, x1=(x+1)*w/nw, y0=y*h/nh, y1=(y+1)*h/nh;
            for (UINT c=0;c<4;++c) {
                UINT sum=0, count=0;
                for (UINT sy=y0;sy<y1;++sy) for (UINT sx=x0;sx<x1;++sx) {
                    sum += std::to_integer<UINT>(bytes[previous+(std::size_t(sy)*w+sx)*4+c]); ++count;
                }
                bytes[next+(std::size_t(y)*nw+x)*4+c] = std::byte((sum+count/2)/count);
            }
        }
        previous=next; next+=std::size_t(nw)*nh*4; w=nw; h=nh;
    }
    return MakeByteStorage(std::move(bytes));
}
}
