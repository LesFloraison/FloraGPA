#include "rdc/Assets.h"
#include "rdc/InputAttribute.h"
#include "rdc/ClearCoverage.h"
#include "rdc/TransferCoverage.h"
#include "application/ReplayMesh.h"
#include <QFile>
#include <QtTest>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <limits>
extern "C" void *__cdecl RENDERDOC_AllocArrayMem(uint64_t size) { return std::malloc(size_t(size)); }
extern "C" void __cdecl RENDERDOC_FreeArrayMem(void *p) { std::free(p); }
using namespace flora;
using Json = nlohmann::json;
namespace {
ResourceId resource(uint64_t value) { return std::bit_cast<ResourceId>(value); }
Json annotationProbe() {
    Json result = Json::array();
    for (uint64_t bits : {uint64_t(0), uint64_t(123), uint64_t(255), uint64_t(0x7ff0000000000000),
                          uint64_t(0xfff0000000000000), uint64_t(0x7ff8000000000000),
                          uint64_t(0x8000000000000000), UINT64_MAX}) {
        SDObject object("probe"_lit, "uint64_t"_lit);
        object.type.basetype = SDBasic::UnsignedInteger;
        object.data.basic.u = bits;
        ResourceDescription r;
        r.annotations = &object;
        result.push_back(rdcResourceDescription(r).at("annotations"));
    }
    return result;
}
} // namespace
class RdcAssetTests : public QObject {
    Q_OBJECT
  private slots:
    void transferCoverage() {
        const auto node=[](SDObject &parent,const char *name) {
            return parent.AddAndOwnChild(new SDObject(rdcstr(name),"struct"_lit));
        };
        const auto integer=[&](SDObject &parent,const char *name,uint64_t value) {
            auto field=node(parent,name); field->type.basetype=SDBasic::UnsignedInteger;
            field->data.basic.u=value; return field;
        };
        constexpr uint64_t identity=9007199254740993ULL;
        TextureDescription target{}; target.resourceId=resource(identity); target.dimension=2;
        target.width=target.height=32; target.mips=2; target.arraysize=2;
        target.format.type=ResourceFormatType::Regular; target.format.compType=CompType::Float;
        auto source=target; source.resourceId=resource(identity+1); source.width=source.height=8;
        source.mips=source.arraysize=1;
        SDObject copy("ID3D12GraphicsCommandList::CopyTextureRegion"_lit,"chunk"_lit);
        auto dst=node(copy,"dst"),src=node(copy,"src");
        integer(*dst,"pResource",identity); integer(*dst,"Type",0); integer(*dst,"SubresourceIndex",3);
        integer(*src,"pResource",identity+1); auto srcType=integer(*src,"Type",0); integer(*src,"SubresourceIndex",0);
        auto dx=integer(copy,"DstX",4); integer(copy,"DstY",4); integer(copy,"DstZ",0);
        auto box=node(copy,"pSrcBox");
        integer(*box,"left",2); integer(*box,"top",1); integer(*box,"front",0);
        integer(*box,"right",6); integer(*box,"bottom",5); integer(*box,"back",1);
        const auto query=[&](const SDObject &chunk,uint32_t x=5,uint32_t y=5,uint32_t mip=1,uint32_t layer=1) {
            return rdcTransferCoverage(chunk,target,{&source,1},Subresource(mip,layer,0),x,y);
        };
        auto result=query(copy); QCOMPARE(result.at("status"),Json("covered"));
        QCOMPARE(result.at("source_resource_id"),Json("9007199254740994"));
        QCOMPARE(result.at("destination_rectangle"),Json::array({4,4,8,8}));
        QCOMPARE(query(copy,4,4).at("status"),Json("covered"));
        QCOMPARE(query(copy,8,4).at("status"),Json("outside"));
        QCOMPARE(query(copy,5,5,0).at("status"),Json("outside"));
        QCOMPARE(query(copy,5,5,1,0).at("status"),Json("outside"));
        dx->data.basic.u=UINT64_MAX; QCOMPARE(query(copy).at("status"),Json("unavailable")); dx->data.basic.u=4;
        box->type.basetype=SDBasic::Null;
        QCOMPARE(query(copy,11,11).at("status"),Json("covered"));
        QCOMPARE(query(copy,12,11).at("status"),Json("outside"));
        source.resourceId=resource(0); QCOMPARE(query(copy).at("status"),Json("unavailable"));
        srcType->data.basic.u=1; auto placed=node(*src,"PlacedFootprint"),fp=node(*placed,"Footprint");
        auto format=integer(*fp,"Format",10); integer(*fp,"Width",8); integer(*fp,"Height",8); integer(*fp,"Depth",1);
        QCOMPARE(query(copy).at("status"),Json("covered")); // Footprint needs no texture object metadata.
        format->data.basic.u=71; QCOMPARE(query(copy).at("status"),Json("unavailable"));
        SDObject whole("ID3D12GraphicsCommandList::CopyResource"_lit,"chunk"_lit);
        integer(whole,"pDstResource",identity); integer(whole,"pSrcResource",identity+1);
        QCOMPARE(query(whole).at("status"),Json("covered"));
        target.resourceId=resource(identity+1); QCOMPARE(query(whole).at("status"),Json("outside"));
        target.resourceId=resource(identity);
        SDObject resolve("ID3D12GraphicsCommandList1::ResolveSubresourceRegion"_lit,"chunk"_lit);
        integer(resolve,"pDstResource",identity); integer(resolve,"pSrcResource",identity+1);
        integer(resolve,"DstSubresource",3); integer(resolve,"SrcSubresource",0);
        integer(resolve,"DstX",4); integer(resolve,"DstY",4);
        auto rect=node(resolve,"pSrcRect");
        for (const auto &[name,value] : std::map<std::string,int64_t>{{"left",0},{"top",0},{"right",4},{"bottom",4}}) {
            auto field=node(*rect,name.c_str()); field->type.basetype=SDBasic::SignedInteger; field->data.basic.i=value;
        }
        source.resourceId=resource(identity+1); source.msSamp=4;
        QCOMPARE(query(resolve).at("status"),Json("covered"));
        QCOMPARE(query(resolve,8,5).at("status"),Json("outside"));
        resolve.name="ID3D12GraphicsCommandList::ResolveSubresource"_lit;
        QCOMPARE(query(resolve).at("status"),Json("unavailable")); // Full Resolve requires equal extents.
        source.width=source.height=16;
        QCOMPARE(query(resolve,15,15).at("status"),Json("covered"));
        QCOMPARE(query(resolve,5,5,1,0).at("status"),Json("outside"));
    }
    void clearCoverage() {
        SDObject chunk("ID3D12GraphicsCommandList::ClearRenderTargetView"_lit, "chunk"_lit);
        const auto node = [](SDObject &parent, const char *name) {
            return parent.AddAndOwnChild(new SDObject(rdcstr(name), "struct"_lit));
        };
        const auto integer = [&](SDObject &parent, const char *name, uint64_t value) {
            auto field = node(parent,name); field->type.basetype=SDBasic::UnsignedInteger;
            field->data.basic.u=value; return field;
        };
        auto target = node(chunk,"RenderTargetView");
        constexpr uint64_t identity=9007199254740993ULL;
        integer(*target,"Resource",identity)->type.basetype=SDBasic::Resource;
        auto view=node(*target,"Descriptor");
        auto dimension=integer(*view,"ViewDimension",5); // D3D12_RTV_DIMENSION_TEXTURE2DARRAY
        auto range=node(*view,"Texture2DArray");
        integer(*range,"MipSlice",1); integer(*range,"FirstArraySlice",1); integer(*range,"ArraySize",2);
        auto count=integer(chunk,"NumRects",0);
        TextureDescription texture{}; texture.resourceId=resource(identity); texture.dimension=2; texture.mips=3; texture.arraysize=4;
        const auto query=[&](uint32_t mip,uint32_t layer,uint32_t x=4,uint32_t y=4) {
            return rdcClearCoverage(chunk,texture,Subresource(mip,layer,0),x,y);
        };
        QCOMPARE(query(1,2).at("status"),Json("covered"));
        QCOMPARE(query(1,2).at("resource_id"),Json("9007199254740993"));
        QCOMPARE(query(0,2).at("status"),Json("outside"));
        QCOMPARE(query(1,0).at("status"),Json("outside"));
        auto rects=node(chunk,"pRects"); rects->type.basetype=SDBasic::Array;
        auto rect=node(*rects,"[]");
        for (const auto &[name,value] : std::map<std::string,int64_t>{{"left",-1},{"top",0},{"right",5},{"bottom",5}}) {
            auto c=node(*rect,name.c_str()); c->type.basetype=SDBasic::SignedInteger; c->data.basic.i=value;
        }
        count->data.basic.u=1;
        QCOMPARE(query(1,2).at("status"),Json("covered"));
        QCOMPARE(query(1,2,5,4).at("status"),Json("outside"));
        count->data.basic.u=2;
        QCOMPARE(query(1,2).at("status"),Json("unavailable"));
        count->data.basic.u=0; dimension->data.basic.u=0;
        QCOMPARE(query(1,2).at("status"),Json("unavailable"));
        chunk.name="ID3D12GraphicsCommandList::ClearDepthStencilView"_lit;
        target->name="DepthStencilView"_lit; dimension->data.basic.u=4; // D3D12_DSV_DIMENSION_TEXTURE2DARRAY
        integer(chunk,"ClearFlags",1);
        QCOMPARE(query(1,2).at("status"),Json("covered"));
        QCOMPARE(query(1,0).at("status"),Json("outside"));
        texture.resourceId=resource(identity+1);
        QCOMPARE(query(1,2).at("status"),Json("unavailable"));
    }
    void inputAttributePrecision() {
        ResourceFormat f{}; f.type = ResourceFormatType::Regular; f.compByteWidth = 4;
        f.compCount = 3; f.compType = CompType::UInt;
        const uint32_t source[]{UINT32_MAX, 0x80000000, 16777217};
        const auto bytes = std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(source), sizeof source);
        auto value = inputAttributeValue(bytes, f);
        QCOMPARE(value, (std::array<uint32_t, 4>{UINT32_MAX, 0x80000000, 16777217, 1}));
        Json m{{"input", {{"first_vertex", "0"}}}, {"perspective_divide", false},
            {"format", {{"compType", uint32_t(inputAttributeFormat(f).outputType)}, {"compCount", 4}, {"compByteWidth", 4}}},
            {"vertexByteStride", 16}, {"indexResourceId", "ResourceId::0"}, {"indexByteStride", 0},
            {"baseVertex", 0}, {"numIndices", 1}, {"topology", 1}};
        const auto mesh = ReplayMesh::decode(m, {reinterpret_cast<const uint8_t *>(value.data()), 16}, {});
        QCOMPARE(mesh.positions.at(0), (std::array<double, 4>{4294967295., 2147483648., 16777217., 1.}));
        QVERIFY(mesh.csv().find("4294967295.0,2147483648.0,16777217.0,1.0") != std::string::npos);
        QVERIFY(mesh.obj().find("Exact 32-bit values") != std::string::npos);
        f.compType = CompType::SInt;
        QCOMPARE(inputAttributeFormat(f).outputType, CompType::SInt);
        QCOMPARE(inputAttributeValue(bytes, f), value);
        f.compByteWidth = 1; f.compCount = 2;
        const uint8_t small[]{0x80, 0xff};
        QCOMPARE(inputAttributeValue(small, f), (std::array<uint32_t, 4>{0xffffff80, UINT32_MAX, 0, 1}));
        f.compType = CompType::SNorm;
        value = inputAttributeValue(small, f);
        QCOMPARE(std::bit_cast<float>(value[0]), -1.f);
        QCOMPARE(std::bit_cast<float>(value[1]), -1.f / 127.f);
        QCOMPARE(std::bit_cast<float>(value[3]), 1.f);
        f.compType = CompType::Float; f.compByteWidth = 4;
        const uint32_t special[]{0x80000000, 0x7fc12345};
        value = inputAttributeValue({reinterpret_cast<const uint8_t *>(special), sizeof special}, f);
        QCOMPARE(value[0], special[0]); QCOMPARE(value[1], special[1]);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, inputAttributeValue({}, f));
    }
    void packedInputAttributes() {
        ResourceFormat f{}; f.type = ResourceFormatType::R10G10B10A2; f.compCount = 4; f.compType = CompType::UInt;
        const uint32_t packed = 1 | (511U << 10) | (1023U << 20) | (2U << 30);
        const auto bytes = std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(&packed), 4);
        QCOMPARE(inputAttributeFormat(f).bytes, 4U);
        QCOMPARE(inputAttributeValue(bytes, f), (std::array<uint32_t, 4>{1, 511, 1023, 2}));
        f.compType = CompType::UNorm;
        const auto normalized = inputAttributeValue(bytes, f);
        QCOMPARE(std::bit_cast<float>(normalized[0]), 1.f / 1023.f);
        QCOMPARE(std::bit_cast<float>(normalized[1]), 511.f / 1023.f);
        QCOMPARE(std::bit_cast<float>(normalized[2]), 1.f);
        QCOMPARE(std::bit_cast<float>(normalized[3]), 2.f / 3.f);
        f.compType = CompType::SInt;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, inputAttributeFormat(f));
        f.type = ResourceFormatType::R11G11B10; f.compCount = 3; f.compType = CompType::Float;
        uint32_t mini = 1 | ((15U << 6) << 11) | ((31U << 5) << 22);
        auto value = inputAttributeValue({reinterpret_cast<const uint8_t *>(&mini), 4}, f);
        QCOMPARE(std::bit_cast<float>(value[0]), std::ldexp(1.f, -20));
        QCOMPARE(std::bit_cast<float>(value[1]), 1.f);
        QVERIFY(std::isinf(std::bit_cast<float>(value[2])));
        QCOMPARE(std::bit_cast<float>(value[3]), 1.f);
        mini = (31U << 6) | 1;
        value = inputAttributeValue({reinterpret_cast<const uint8_t *>(&mini), 4}, f);
        QVERIFY(std::isnan(std::bit_cast<float>(value[0])));
        QCOMPARE(appendInputAttributeOffset(1, f), 4U);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, appendInputAttributeOffset(UINT32_MAX, f));
        f.type = ResourceFormatType::Regular; f.compType = CompType::UInt; f.compCount = 1; f.compByteWidth = 2;
        QCOMPARE(appendInputAttributeOffset(1, f), 2U);
        f.compByteWidth = 1;
        QCOMPARE(appendInputAttributeOffset(1, f), 1U);
        f.type = ResourceFormatType::BC1;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, inputAttributeFormat(f));
    }
    void textureReadbackSelection() {
        TextureDescription t{};
        t.dimension = 3;
        t.width = 16;
        t.height = 8;
        t.depth = 4;
        t.mips = 3;
        t.arraysize = 1;
        t.msSamp = 1;
        t.byteSize = 9007199254740993ULL;
        t.format.type = ResourceFormatType::Regular;
        t.format.compType = CompType::UNormSRGB;
        t.format.compCount = 4;
        t.format.compByteWidth = 1;
        t.format.SetBGRAOrder(true);
        const auto volume = rdcTextureReadbackDescription(t, Subresource(1, 0, 0));
        QCOMPARE(volume.at("selected_extent"), Json({{"width", 8}, {"height", 4}, {"depth", 2}}));
        QCOMPARE(volume.at("scope"), Json("whole_3d_mip"));
        QCOMPARE(volume.at("texture").at("byteSize"), Json("9007199254740993"));
        QVERIFY(volume.at("texture").at("format").at("bgra_order").get<bool>());
        QVERIFY(volume.at("texture").at("format").at("srgb").get<bool>());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, rdcTextureReadbackDescription(t, Subresource(1, 1, 0)));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, rdcTextureReadbackDescription(t, Subresource(3, 0, 0)));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, rdcTextureReadbackDescription(t, Subresource(0, 0, 1)));
        t.dimension = 2;
        t.arraysize = 6;
        t.msSamp = 4;
        const auto array = rdcTextureReadbackDescription(t, Subresource(0, 5, 3));
        QCOMPARE(array.at("scope"), Json("array_layer_mip_sample"));
        QCOMPARE(array.at("selected_extent").at("depth"), Json(1));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, rdcTextureReadbackDescription(t, Subresource(0, 6, 0)));
    }
    void resourceGraphAndWideValues() {
        ResourceDescription r;
        r.resourceId = resource(UINT64_MAX);
        r.name = "GPA resource 25 (texture)";
        r.autogeneratedName = false;
        r.parentResources = {resource(2), resource(3)};
        r.derivedResources = {resource(4)};
        r.initialisationChunks = {5, UINT32_MAX};
        const auto j = rdcResourceDescription(r);
        QCOMPARE(j.at("resourceId"), Json("ResourceId::18446744073709551615"));
        QCOMPARE(j.at("parentResources"), Json({"ResourceId::2", "ResourceId::3"}));
        QCOMPARE(j.at("initialisationChunks"), Json({5, UINT32_MAX}));
        QVERIFY(j.at("annotations").is_null());
        QCOMPARE(j.size(), size_t(8));
        BufferDescription b;
        b.resourceId = resource(99);
        b.length = UINT64_MAX;
        b.gpuAddress = uint64_t(9007199254740993);
        const auto buffer = rdcBufferDescription(b);
        QCOMPARE(buffer.at("length").dump(), std::string("18446744073709551615"));
        QCOMPARE(buffer.at("gpuAddress").dump(), std::string("9007199254740993"));
        QCOMPARE(buffer.size(), size_t(4));
    }
    void textureMetadata() {
        TextureDescription t{};
        t.resourceId = resource(10);
        t.dimension = 3;
        t.type = TextureType::Texture3D;
        t.width = 16;
        t.height = 8;
        t.depth = 4;
        t.mips = 3;
        t.arraysize = 1;
        t.msSamp = 1;
        t.byteSize = UINT64_MAX;
        t.format.type = ResourceFormatType::Regular;
        t.format.compType = CompType::UNormSRGB;
        t.format.compCount = 4;
        t.format.compByteWidth = 1;
        t.format.SetBGRAOrder(true);
        const auto result = rdcTextureDescription(t);
        QCOMPARE(result.size(), size_t(14));
        QCOMPARE(result.at("depth"), Json(4));
        QCOMPARE(result.at("byteSize").get<uint64_t>(), UINT64_MAX);
        QCOMPARE(result.at("format").size(), size_t(4));
        QVERIFY(!result.at("format").contains("flags"));
        QCOMPARE(result.at("format").at("compType"), Json(uint32_t(CompType::UNormSRGB)));
    }
    void annotationUnion() {
        auto rows = annotationProbe();
        QCOMPARE(rows.at(1).at("data").at("basic").at("c"), Json("{"));
        QCOMPARE(rows.at(1).at("data").at("basic").at("id"), Json("ResourceId::123"));
        QCOMPARE(rows.at(3).at("data").at("basic").at("d"), Json("inf"));
        QCOMPARE(rows.at(4).at("data").at("basic").at("d"), Json("-inf"));
        QCOMPARE(rows.at(5).at("data").at("basic").at("d"), Json("nan"));
        QVERIFY(std::signbit(rows.at(6).at("data").at("basic").at("d").get<double>()));
        QCOMPARE(rows.at(7).at("data").at("basic").at("i"), Json(-1));
        QVERIFY(rdcInventoryText(rows).find("\\udcff") != std::string::npos);
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().size() == 3 && app.arguments()[1] == "--probe") {
        QFile f(app.arguments()[2]);
        const auto data = rdcInventoryText(annotationProbe());
        return f.open(QIODevice::WriteOnly) &&
                       f.write(data.data(), qint64(data.size())) == qint64(data.size())
                   ? 0
                   : 1;
    }
    RdcAssetTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "RdcAssetTests.moc"
