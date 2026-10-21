#include "texture_tests.h"
#include "native_texture.h"
#include "png_texture.h"
#include "texture_convert.h"
#include "texture_resize.h"
#include "general_dds_candidate.h"
#include "loader_config.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <zlib.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
#pragma comment(lib, "d3d11.lib")

namespace {
using namespace ds2::modding;
using Microsoft::WRL::ComPtr;
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void Put(std::vector<std::byte>& b,std::size_t p,std::uint32_t v) {
    for(unsigned i=0;i<4;++i) b[p+i]=std::byte((v>>(i*8))&255);
}
std::vector<std::byte> Dds(unsigned w,unsigned h,bool bgra) {
    const unsigned bw=w,bh=h;
    unsigned mips=0; std::size_t total=128;
    for(;;) {
        total+=bgra ? std::size_t(w)*h*4 : std::size_t((w+3)/4)*((h+3)/4)*8;
        ++mips; if(w==1&&h==1) break; w=(std::max)(1u,w/2); h=(std::max)(1u,h/2);
    }
    std::vector<std::byte> b(total,std::byte{0x36});
    std::fill_n(b.begin(),128,std::byte{});
    Put(b,0,0x20534444);Put(b,4,124);Put(b,8,0x21007);Put(b,12,bh);Put(b,16,bw);
    Put(b,28,mips);Put(b,76,32);Put(b,108,0x401008);
    if(bgra) {
        Put(b,80,0x41);Put(b,88,32);Put(b,92,0xFF0000);Put(b,96,0xFF00);Put(b,100,255);Put(b,104,0xFF000000);
    } else { Put(b,80,4);Put(b,84,0x31545844); }
    return b;
}
void Write(const std::filesystem::path& p,std::span<const std::byte> b) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p,std::ios::binary);f.write(reinterpret_cast<const char*>(b.data()),b.size());
    Check(bool(f),"texture fixture write failed");
}
// Independent tiny PNG writer: 3 x 1, transparent red / green / blue.
std::vector<std::byte> Png() {
    std::vector<std::byte> b;
    for(unsigned c:{137,80,78,71,13,10,26,10}) b.push_back(std::byte(c));
    const auto big=[&](unsigned n) { for(int s=24;s>=0;s-=8) b.push_back(std::byte((n>>s)&255)); };
    const auto chunk=[&](const char* type,const std::vector<std::byte>& data) {
        big(static_cast<unsigned>(data.size()));const auto at=b.size();
        for(int i=0;i<4;++i) b.push_back(std::byte(type[i]));
        b.insert(b.end(),data.begin(),data.end());
        big(crc32(0,reinterpret_cast<const Bytef*>(b.data()+at),static_cast<uInt>(4+data.size())));
    };
    std::vector<std::byte> header(13);header[3]=std::byte{3};header[7]=std::byte{1};
    header[8]=std::byte{8};header[9]=std::byte{6};chunk("IHDR",header);
    const std::array<Bytef,13> pixels{0,255,0,0,64,0,255,0,128,0,0,255,255};
    uLongf size=compressBound(static_cast<uLong>(pixels.size()));std::vector<std::byte> compressed(size);
    Check(compress2(reinterpret_cast<Bytef*>(compressed.data()),&size,pixels.data(),static_cast<uLong>(pixels.size()),9)==Z_OK,"PNG fixture compression");
    compressed.resize(size);chunk("IDAT",compressed);chunk("IEND",{});return b;
}
ResourceIdentitySample Sample(const GeneralDdsCandidate& c) {
    ResourceIdentitySample s{};s.sequence=1;s.nesting_depth=1;s.read_ordinal=91;
    s.scope.package_base=c.identity.package_base;s.scope.member_table_offset=c.identity.member_table_offset;
    s.scope.object_fields_valid=true;s.scope.stream_valid=true;s.scope.stream=1;s.scope.caller_rva=0x9FA980;
    s.read.stream=2;s.read.caller_rva=0x9FF0D2;s.read.member_ordinal=c.identity.member_ordinal;return s;
}
struct Call { unsigned w,h,format,mips,flags;const void* data; };
std::vector<Call> calls;
bool fail_first{};
bool Mock(void*,void*,std::uint16_t w,std::uint16_t h,std::uint32_t f,std::uint16_t m,const void* data,std::uint32_t flags) {
    calls.push_back({w,h,f,m,flags,data});SetLastError(713);
    return !(fail_first&&calls.size()==1);
}
bool Gpu(void*,void* raw,std::uint16_t w,std::uint16_t h,std::uint32_t f,std::uint16_t m,const void* pixels,std::uint32_t) {
    auto* device=static_cast<ID3D11Device*>(raw);
    D3D11_TEXTURE2D_DESC desc{};desc.Width=w;desc.Height=h;desc.MipLevels=m;desc.ArraySize=1;
    desc.Format=f==6 ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_BC1_UNORM;
    desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    std::vector<D3D11_SUBRESOURCE_DATA> levels;
    const auto* at=static_cast<const std::byte*>(pixels);unsigned mw=w,mh=h;
    for(unsigned i=0;i<m;++i) {
        unsigned pitch=f==6 ? mw*4 : ((mw+3)/4)*8;
        unsigned size=pitch*(f==6 ? mh : (mh+3)/4);
        levels.push_back({at,pitch,size});at+=size;mw=(std::max)(1u,mw/2);mh=(std::max)(1u,mh/2);
    }
    ComPtr<ID3D11Texture2D> texture;
    if(FAILED(device->CreateTexture2D(&desc,levels.data(),&texture))) return false;
    ComPtr<ID3D11ShaderResourceView> view;
    if(FAILED(device->CreateShaderResourceView(texture.Get(),nullptr,&view))) return false;
    D3D11_TEXTURE2D_DESC actual{};texture->GetDesc(&actual);
    Check(actual.Width==w&&actual.Height==h&&actual.MipLevels==m,"GPU changed replacement dimensions");
    calls.push_back({w,h,f,m,0,pixels});return true;
}
}

void TestNativeTextures(const std::filesystem::path& root) {
    const auto png=Png();auto decoded=DecodePngTexture(png,1024);
    Check(bool(decoded),"valid RGBA PNG rejected");
    const auto metadata=ValidateDds(decoded.storage->Bytes());
    Check(metadata&&metadata.metadata->width==3&&metadata.metadata->height==1&&metadata.metadata->mip_count==2,
        "rectangular PNG dimensions/mips lost");
    const auto pixels=decoded.storage->Bytes().subspan(128);
    Check(pixels[0]==std::byte{0}&&pixels[2]==std::byte{255}&&pixels[3]==std::byte{64}&&
        pixels[12]==std::byte{85}&&pixels[13]==std::byte{85}&&pixels[14]==std::byte{85}&&pixels[15]==std::byte{149},
        "PNG alpha, BGRA order or odd-size mip is wrong");
    Check(!DecodePngTexture(png,143),"PNG decode exceeded its byte budget");
    Check(!DecodePngTexture(std::span(png).first(12),1024),"truncated PNG accepted");
    auto broken=png;broken[0]=std::byte{};Check(!DecodePngTexture(broken,1024),"non-PNG accepted");
    const auto mods=root/L"png-mods";const auto file=mods/L"sample"/L"media"/L"weapon.dds.png";
    Write(file,png);
    ModIndexOptions options;options.dds_only=true;options.include_png=true;
    auto index=BuildModIndex(mods,options);
    Check(index&&index.snapshot->Find(L"media/weapon.dds")&&index.snapshot->Assets()[0].source_is_png,"PNG target mapping failed");
    Write(file.parent_path()/L"weapon.dds",Dds(4,4,false));
    index=BuildModIndex(mods,options);
    Check(index&&index.snapshot->Assets().empty(),"DDS/PNG alias collision accepted");
    Check(ParseLoaderConfig("[loader]\ntextures=exact").valid&&
        !ParseLoaderConfig("[loader]\ntextures=exact").config.native_textures,"exact texture mode failed");
    Check(!ParseLoaderConfig("[loader]\ntextures=native\ntextures=exact").valid,"duplicate texture mode accepted");

    const auto source=Dds(512,512,false);
    auto replacement=MakeByteStorage(Dds(2048,2048,false));
    GeneralDdsCandidate c{};c.identity={0x100,0x300,2};c.virtual_path=*NormalizeVirtualPath(L"media/weapon.dds").path;
    c.requires_native_upload=true;c.original_dds=*ValidateDds(source).metadata;c.original_size=static_cast<std::uint32_t>(source.size());
    c.dds=*ValidateDds(replacement.storage->Bytes()).metadata;c.storage=replacement.storage;c.payload_offset=128;
    c.payload_size=static_cast<std::uint32_t>(c.dds.payload_size);
    c.original_full_sha256=ComputeSha256(source).digest;
    c.original_payload_sha256=ComputeSha256(std::span(source).subspan(128)).digest;
    Check(GeneralDdsReplacementBytes({GeneralDdsDryRunDecision::full_dds,&c}).empty(),"native data reached stream copy");
    auto payload=std::vector<std::byte>(source.begin()+128,source.end());const auto untouched=payload;
    auto sample=Sample(c);const auto n=static_cast<std::int32_t>(payload.size());
    NativeTextureRuntime runtime({&c,1});runtime.write_enabled=true;
    const auto arm=[&] { runtime.Forget(payload.data(),payload.size());runtime.Observe(sample,payload.data(),n,n);calls.clear(); };
    arm();SetLastError(777);
    Check(runtime.Upload(Mock,nullptr,nullptr,512,512,15,10,payload.data(),0),"native resize failed");
    Check(calls.size()==1&&calls[0].w==2048&&calls[0].h==2048&&calls[0].mips==12&&
        calls[0].data==c.storage->Data()+128&&payload==untouched&&GetLastError()==713,"native resize changed original buffer/ABI");
    // Consumed identity cannot accidentally authorize another allocation.
    calls.clear();runtime.Upload(Mock,nullptr,nullptr,512,512,15,10,payload.data(),0);
    Check(calls[0].w==512,"stale upload registration reused");
    for(unsigned gate=0;gate<8;++gate) {
        arm();unsigned w=512,m=10,f=15,flags=0;const void* ptr=payload.data();
        if(gate==0) runtime.write_enabled=false;
        if(gate==1) w=256;
        if(gate==2) m=9;
        if(gate==3) f=17;
        if(gate==4) flags=1;
        if(gate==5) ptr=untouched.data();
        if(gate==6) payload.back()^=std::byte{1};
        if(gate==7) runtime.Forget(payload.data()+8,4);
        runtime.Upload(Mock,nullptr,nullptr,static_cast<std::uint16_t>(w),512,f,static_cast<std::uint16_t>(m),ptr,flags);
        Check(calls.size()==1&&calls[0].data==ptr,"native contract rejection did not pass original");
        payload=untouched;runtime.write_enabled=true;
    }
    arm();fail_first=true;
    Check(runtime.Upload(Mock,nullptr,nullptr,512,512,15,10,payload.data(),0),"failed GPU replacement lost original fallback");
    Check(calls.size()==2&&calls[0].w==2048&&calls[1].w==512&&calls[1].data==payload.data(),"fallback ABI mismatch");
    fail_first=false;
    for(unsigned gate=0;gate<5;++gate) {
        runtime.Forget(payload.data(),payload.size());auto bad=sample;
        if(gate==0) ++bad.read.member_ordinal;
        if(gate==1) ++bad.scope.package_base;
        if(gate==2) ++bad.scope.caller_rva;
        if(gate==3) ++bad.read.caller_rva;
        runtime.Observe(bad,payload.data(),n,gate==4?n-1:n);calls.clear();
        runtime.Upload(Mock,nullptr,nullptr,512,512,15,10,payload.data(),0);
        Check(calls[0].data==payload.data(),"unverified source armed native upload");
    }
    // Real D3D11 WARP creation of resized compressed data, plus a new format
    // and rectangular mip chain through the same production decision path.
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,
        D3D11_SDK_VERSION,&device,nullptr,&context)),"D3D11 WARP device unavailable");
    arm();Check(runtime.Upload(Gpu,nullptr,device.Get(),512,512,15,10,payload.data(),0),"2048 BC1 GPU creation failed");
    Check(calls.size()==1&&calls[0].w==2048,"GPU resize fell back");
    c.storage=MakeByteStorage(Dds(1024,256,true)).storage;c.dds=*ValidateDds(c.storage->Bytes()).metadata;
    c.payload_size=static_cast<std::uint32_t>(c.dds.payload_size);
    NativeTextureRuntime rectangle({&c,1});rectangle.write_enabled=true;
    rectangle.Observe(sample,payload.data(),n,n);calls.clear();
    Check(rectangle.Upload(Gpu,nullptr,device.Get(),512,512,15,10,payload.data(),0),"rectangular BGRA GPU creation failed");
    Check(calls.size()==1&&calls[0].w==1024&&calls[0].h==256&&calls[0].format==6,"rectangular GPU upload fell back");
    // Whole DDS reads register the payload after its header, not the header.
    rectangle.Observe(sample,source.data(),static_cast<std::int32_t>(source.size()),static_cast<std::int32_t>(source.size()));calls.clear();
    rectangle.Upload(Mock,nullptr,nullptr,512,512,15,10,source.data()+128,0);
    Check(calls[0].w==1024,"whole DDS did not register payload offset");
    std::cout<<"native_textures: PASS (512->2048, 1024x256, WARP texture+view, fallback, identity, PNG alpha/mips/budget/collisions)\n";
}

int CheckTextureFiles(const std::filesystem::path& source) {
    using namespace ds2::modding;
    const auto check=[](const std::filesystem::path& file) {
        // No byte budget, as the loader (RESTRICCIONES.md).
        constexpr auto unlimited=(std::numeric_limits<std::uint64_t>::max)();
        auto loaded=LoadByteStorage(file,unlimited);Check(bool(loaded),"texture input unreadable");
        if(file.extension()==L".png") loaded=DecodePngTexture(loaded.storage->Bytes(),unlimited);
        Check(bool(loaded),"texture decode failed");const auto d=ValidateDds(loaded.storage->Bytes());Check(bool(d),"texture DDS validation failed");
        std::wcout<<file.filename().wstring()<<L": "<<d.metadata->width<<L"x"<<d.metadata->height
            <<L" mips="<<d.metadata->mip_count<<L" format="<<DdsFormatName(d.metadata->format)
            <<L" bytes="<<loaded.storage->Size()<<L"\n";
    };
    if(std::filesystem::is_directory(source)) {
        for(const auto& f:std::filesystem::directory_iterator(source))
            if(f.path().extension()==L".png"||f.path().extension()==L".dds") check(f.path());
    } else check(source);
    return 0;
}
void TestTextureConversion() {
    // Solid BGRA 8x4 -> BC1 16x16 with its full mip chain: every block holds
    // the 565-quantized color (24,25,6) and the original header is verbatim.
    auto source=Dds(8,4,true);const auto source_meta=*ValidateDds(source).metadata;
    for(std::size_t at=128;at<source.size();at+=4) {
        source[at]=std::byte{50};source[at+1]=std::byte{100};source[at+2]=std::byte{200};source[at+3]=std::byte{255};
    }
    auto target=Dds(16,16,false);target[127]=std::byte{0x5A}; // reserved header byte must survive
    const auto target_meta=*ValidateDds(target).metadata;
    const auto bc1=ConvertDdsToContract(source,source_meta,std::span(target).first(128),target_meta);
    Check(bool(bc1)&&bc1.bytes.size()==target.size()&&std::equal(target.begin(),target.begin()+128,bc1.bytes.begin()),
        "BGRA to BC1 conversion lost the original contract");
    const auto converted=ValidateDds(bc1.bytes);
    Check(bool(converted)&&converted.metadata->width==16&&converted.metadata->mip_count==target_meta.mip_count,
        "converted BC1 is not a valid DDS of the original contract");
    for(std::size_t at=128;at<bc1.bytes.size();at+=8)
        Check(bc1.bytes[at]==std::byte{0x26}&&bc1.bytes[at+1]==std::byte{0xC3}&&bc1.bytes[at+2]==std::byte{0x26}&&
              bc1.bytes[at+3]==std::byte{0xC3}&&bc1.bytes[at+4]==std::byte{}&&bc1.bytes[at+7]==std::byte{},
              "solid color did not encode to its exact BC1 endpoints");
    // BC1 red block -> BGRA 8x8: decoding and enlarging keep the color.
    auto red=Dds(4,4,false);const auto red_meta=*ValidateDds(red).metadata;
    std::fill(red.begin()+128,red.end(),std::byte{});
    for(std::size_t at=128;at<red.size();at+=8) { red[at]=std::byte{0x00};red[at+1]=std::byte{0xF8}; }
    auto bgra=Dds(8,8,true);const auto bgra_meta=*ValidateDds(bgra).metadata;
    const auto decoded=ConvertDdsToContract(red,red_meta,std::span(bgra).first(128),bgra_meta);
    Check(bool(decoded)&&decoded.bytes.size()==bgra.size(),"BC1 to BGRA conversion failed");
    for(std::size_t at=128;at<decoded.bytes.size();at+=4)
        Check(decoded.bytes[at]==std::byte{0}&&decoded.bytes[at+1]==std::byte{0}&&
              decoded.bytes[at+2]==std::byte{255}&&decoded.bytes[at+3]==std::byte{255},"BC1 red did not decode to red");
    // Limits: header size and unsupported formats.
    Check(!ConvertDdsToContract(source,source_meta,std::span(target).first(64),target_meta),"short header accepted");
    auto bc4=target_meta;bc4.format=DdsFormat::bc4;
    Check(!CanConvertDdsFormat(DdsFormat::bc4)&&!ConvertDdsToContract(source,source_meta,std::span(target).first(128),bc4),
        "BC4 conversion accepted");
    std::cout<<"texture_conversion: PASS (BGRA->BC1 exact endpoints and mips, BC1->BGRA decode, verbatim header, limits)\n";
}

void TestTextureResize() {
    using B=std::byte;
    // ds1a01_shoulder_diff.dds in media.upak: 1024x1024, BC1, 11 mips.
    const auto bc1=*ValidateDds(Dds(1024,1024,false)).metadata;
    const auto extra=EncodeTextureEngineExtra(bc1);
    Check(extra&&*extra==TextureEngineExtra{B{15},B{11},B{1},B{0},B{4},B{0},B{4}},"BC1 extra differs from the package");
    const auto format=[&](DdsFormat f) { auto m=bc1;m.format=f;return EncodeTextureEngineExtra(m); };
    Check(format(DdsFormat::bgra8)->front()==B{6}&&format(DdsFormat::bc2)->front()==B{16}&&
        format(DdsFormat::bc3)->front()==B{17},"engine formats differ from 0x751260");
    auto dx10=bc1;dx10.has_dx10_header=true;dx10.header_size=148;
    // Sides are u16 fields of the extra: 65535 is the only side limit.
    auto wide=bc1;wide.width=65536;auto widest=bc1;widest.width=65535;auto mips=bc1;mips.mip_count=256;
    Check(!format(DdsFormat::rgba8)&&!format(DdsFormat::bgr8)&&!format(DdsFormat::bc4)&&
        !EncodeTextureEngineExtra(dx10)&&!EncodeTextureEngineExtra(wide)&&!EncodeTextureEngineExtra(mips),
        "extra encoded for something the engine cannot take");
    Check(EncodeTextureEngineExtra(widest)&&(*EncodeTextureEngineExtra(widest))[3]==B{0xFF}&&
        (*EncodeTextureEngineExtra(widest))[4]==B{0xFF},"a u16 side was refused");
    // The user's shoulder replacement: 512x1024 BC1 with its full chain.
    const auto shoulder_bytes=Dds(512,1024,false);
    const auto shoulder=*ValidateDds(shoulder_bytes).metadata;
    const auto levels=TextureEngineLevels(shoulder);
    std::uint64_t sum=0;for(const auto l:levels) sum+=l;
    Check(levels.size()==11&&levels[0]==262144&&levels[1]==65536&&levels.back()==8&&sum==shoulder.payload_size,
        "engine level sizes differ from the DDS layout");
    // Mip trimming of the D3D9 handler (0x75A63C).
    Check(TextureEngineMipSkip(11,1024,1024,0,2)==0&&TextureEngineMipSkip(11,1024,1024,0,1)==1&&
        TextureEngineMipSkip(11,1024,1024,0,0)==2&&TextureEngineMipSkip(7,64,64,0,1)==0&&
        TextureEngineMipSkip(8,128,128,0,1)==1&&TextureEngineMipSkip(12,2048,2048,9,2)==3&&
        TextureEngineMipSkip(12,2048,2048,12,0)==2&&TextureEngineMipSkip(11,1024,4,9,2)==0&&
        TextureEngineMipSkip(11,1024,1024,0,3)==0&&TextureEngineMipSkip(1,512,1024,0,0)==0,
        "mip trimming differs from the handler");
    // The plan: the package entry must repeat the original contract.
    PackageIdentityEntry entry;entry.identity={0xABD000,0x19800,1251};entry.type_id=6;
    entry.original_size=static_cast<std::uint32_t>(128+bc1.payload_size);
    entry.metadata_size=64;entry.extra_offset=20;entry.extra.assign(extra->begin(),extra->end());
    entry.metadata_sha256[0]=0x5A;
    const auto storage=MakeByteStorage(shoulder_bytes).storage;
    std::wstring error;
    const auto plan=PlanTextureResize(entry,bc1,storage,shoulder,error);
    Check(plan&&error.empty()&&plan->extra==TextureEngineExtra{B{15},B{11},B{1},B{0},B{2},B{0},B{4}}&&
        plan->extra_offset==20&&plan->metadata_size==64&&plan->metadata_sha256==entry.metadata_sha256&&
        plan->replacement_runs.size()==11&&plan->replacement_runs.front()==262144&&
        plan->replacement_runs.back()==shoulder.payload_size&&plan->original_runs.back()==bc1.payload_size&&
        plan->storage==storage&&plan->warnings.empty(),"shoulder resize plan rejected");
    const auto rejected=[&](PackageIdentityEntry e,const DdsMetadata& replacement,std::shared_ptr<const ByteStorage> s) {
        std::wstring why;return !PlanTextureResize(e,bc1,std::move(s),replacement,why)&&!why.empty();
    };
    auto other=entry;other.extra[1]^=B{1};
    auto tiny=entry;tiny.metadata_size=6;
    auto outside=entry;outside.extra_offset=58;
    auto size=entry;++size.original_size;
    auto rgba=shoulder;rgba.format=DdsFormat::rgba8;
    auto longer=shoulder_bytes;longer.push_back(B{});
    Check(rejected(other,shoulder,storage)&&rejected(tiny,shoulder,storage)&&rejected(outside,shoulder,storage)&&
        rejected(size,shoulder,storage)&&rejected(entry,rgba,storage)&&
        rejected(entry,shoulder,MakeByteStorage(longer).storage)&&
        rejected(entry,bc1,MakeByteStorage(Dds(1024,1024,false)).storage),
        "invalid texture resize plan accepted");
    // D3D9's documented rules were never seen failing in game: warnings only.
    const auto odd_bytes=Dds(510,1022,false);const auto odd_side=*ValidateDds(odd_bytes).metadata;
    const auto odd=PlanTextureResize(entry,bc1,MakeByteStorage(odd_bytes).storage,odd_side,error);
    Check(odd&&odd->warnings.size()==1&&odd->warnings[0].find(L"multiples of 4")!=std::wstring::npos,
        "BC sides that are not multiples of 4 were refused");
    auto huge=shoulder;huge.width=16388;huge.height=4;huge.mip_count=1;huge.payload_size=4097*8;
    std::vector<std::byte> huge_bytes(128+huge.payload_size);
    const auto big=PlanTextureResize(entry,bc1,MakeByteStorage(huge_bytes).storage,huge,error);
    Check(big&&big->warnings.size()==1&&big->warnings[0].find(L"16384")!=std::wstring::npos,
        "a side over 16384 was refused");
    const auto mode=ParseLoaderConfig("[loader]\ntextures=resize");
    Check(mode.valid&&mode.config.native_textures&&mode.config.resize_textures&&
        ParseLoaderConfig("[loader]").config.resize_textures&&
        !ParseLoaderConfig("[loader]\ntextures=native").config.resize_textures&&
        !ParseLoaderConfig("[loader]\ntextures=exact").config.resize_textures&&
        !ParseLoaderConfig("[loader]\ntextures=resize\ntextures=native").valid,"resize texture mode failed");
    Check(ParseLoaderConfig("[loader]").config.resize_textures,"textures=resize is not the default");
    // A DX10 header behind the legacy header of its format, payload untouched.
    auto dx10_bytes=Dds(8,8,false);
    Put(dx10_bytes,84,0x30315844);
    dx10_bytes.insert(dx10_bytes.begin()+128,20,B{});
    Put(dx10_bytes,128,72);Put(dx10_bytes,132,3);Put(dx10_bytes,140,1);
    const auto dx10_meta=ValidateDds(dx10_bytes);
    const auto legacy=dx10_meta?LegacyDdsBytes(dx10_bytes,*dx10_meta.metadata):std::nullopt;
    const auto legacy_meta=legacy?ValidateDds(*legacy):DdsValidationResult{};
    Check(legacy_meta&&!legacy_meta.metadata->has_dx10_header&&legacy_meta.metadata->format==DdsFormat::bc1&&
        legacy->size()+20==dx10_bytes.size()&&std::equal(legacy->begin()+128,legacy->end(),dx10_bytes.begin()+148),
        "DX10 header was not rewritten as a legacy one");
    // RGBA8 and BGR8 become BGRA8 with their own size and mips.
    auto rgba_bytes=Dds(2,2,true);
    Put(rgba_bytes,92,0xFF);Put(rgba_bytes,100,0xFF0000);
    for(std::size_t at=128;at<rgba_bytes.size();at+=4) {
        rgba_bytes[at]=B{10};rgba_bytes[at+1]=B{20};rgba_bytes[at+2]=B{30};rgba_bytes[at+3]=B{40};
    }
    const auto rgba_meta=*ValidateDds(rgba_bytes).metadata;
    const auto expanded=ExpandDdsToBgra8(rgba_bytes,rgba_meta);
    const auto expanded_meta=expanded?ValidateDds(*expanded):DdsValidationResult{};
    Check(rgba_meta.format==DdsFormat::rgba8&&expanded_meta&&expanded_meta.metadata->format==DdsFormat::bgra8&&
        expanded_meta.metadata->mip_count==rgba_meta.mip_count&&expanded->size()==rgba_bytes.size()&&
        (*expanded)[128]==B{30}&&(*expanded)[129]==B{20}&&(*expanded)[130]==B{10}&&(*expanded)[131]==B{40},
        "RGBA8 was not expanded to BGRA8");
    std::vector<std::byte> bgr_bytes(rgba_bytes.begin(),rgba_bytes.begin()+128);
    Put(bgr_bytes,80,0x40);Put(bgr_bytes,88,24);Put(bgr_bytes,92,0xFF0000);Put(bgr_bytes,100,0xFF);Put(bgr_bytes,104,0);
    for(int i=0;i<5;++i) for(const unsigned c:{1u,2u,3u}) bgr_bytes.push_back(B(c));
    const auto bgr_meta=ValidateDds(bgr_bytes);
    const auto bgr=bgr_meta?ExpandDdsToBgra8(bgr_bytes,*bgr_meta.metadata):std::nullopt;
    Check(bgr_meta&&bgr_meta.metadata->format==DdsFormat::bgr8&&bgr&&bgr->size()==128+5*4&&ValidateDds(*bgr)&&
        (*bgr)[128]==B{1}&&(*bgr)[129]==B{2}&&(*bgr)[130]==B{3}&&(*bgr)[131]==B{0xFF},
        "BGR8 was not expanded to BGRA8");
    Check(!ExpandDdsToBgra8(shoulder_bytes,shoulder),"BC1 was expanded");
    std::cout<<"texture_resize: PASS (engine extra, level sizes, mip trimming, plan, 7 rejections, 2 warnings, "
               "resize default, DX10 header, RGBA8/BGR8 to BGRA8, INI mode)\n";
}

int CheckTextureCatalog(const std::filesystem::path& game,const std::filesystem::path& mods,const std::filesystem::path& output,bool resize) {
    using namespace ds2::modding;
    ModIndexOptions o;o.dds_only=true;o.include_png=true; // no byte budgets, as the loader
    const auto index=BuildModIndex(mods,o);Check(index&&index.issues.empty(),"texture mod index rejected files");
    std::vector<CanonicalVirtualPath> paths;for(const auto& a:index.snapshot->Assets()) paths.push_back(a.virtual_path);
    Check(!paths.empty(),"texture catalog is empty");
    PackageIdentityCatalogOptions identity;identity.allow_member_blocks=true; // as the loader does
    const auto catalog=BuildMediaPackageIdentityCatalog(game,paths,identity);Check(catalog&&catalog.issues.empty(),"texture identities missing");
    const auto contracts=BuildMediaPackageDdsContractCatalog(game,catalog.snapshot.get());Check(contracts&&contracts.issues.empty(),"texture original contracts unavailable");
    GeneralDdsCandidateOptions options;options.allow_native_upload=true;options.resize_textures=resize;
    const auto candidates=BuildGeneralDdsCandidateSnapshot(catalog.snapshot.get(),contracts.snapshot.get(),index.snapshot.get(),options);
    Check(candidates&&candidates.issues.empty()&&candidates.snapshot->Entries().size()==paths.size(),"native texture candidates rejected");
    for(const auto& c:candidates.snapshot->Entries()) std::wcout<<c.virtual_path.key<<L" "<<c.original_dds.width<<L"x"<<c.original_dds.height
        <<L" -> "<<c.source_dds.width<<L"x"<<c.source_dds.height<<L" route="
        <<(c.requires_native_upload ? L"native_upload" : c.converted ? L"converted_stream_copy" : L"stream_copy")
        <<(c.conversion_error.empty() ? std::wstring{} : L" conversion_error="+c.conversion_error)
        <<(c.resize ? L" resize="+std::to_wstring(c.resize->dds.width)+L"x"+std::to_wstring(c.resize->dds.height)+
            L"/"+std::wstring(DdsFormatName(c.resize->dds.format))+L"/mips="+std::to_wstring(c.resize->dds.mip_count)+
            L" member="+std::to_wstring(c.identity.member_ordinal)+L" package_base="+std::to_wstring(c.identity.package_base)+
            L" metadata_offset="+std::to_wstring(catalog.snapshot->Find(c.identity)->metadata_offset)+
            L" metadata_bytes="+std::to_wstring(c.resize->metadata_size)+L" extra_offset="+std::to_wstring(c.resize->extra_offset)+
            L" delivered="+std::to_wstring(c.resize->storage->Size())
            : c.resize_error.empty() ? std::wstring{} : L" resize_error="+c.resize_error)<<L"\n";
    // Every texture of another contract must get its plan.
    if(resize) for(const auto& c:candidates.snapshot->Entries())
        Check(c.resize||(!c.converted&&!c.requires_native_upload),"texture resize plan rejected");
    if(!output.empty()) for(const auto& c:candidates.snapshot->Entries())
        Write(output/std::filesystem::path(c.virtual_path.key).filename(),c.storage->Bytes());
    return 0;
}
