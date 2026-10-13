// Experimental format writer retained separately from the production encoder.
#include "msaa_depth_encoding_fixture.hpp"
namespace {
struct Payload:ByteBuffer {
    template<class T>Payload& v(T value){auto at=size();resize(at+sizeof value);wire_write(data()+at,value);return *this;}
    Payload& q(std::initializer_list<uint64_t> values){for(auto value:values)v(value);return *this;}
    Payload& u(std::initializer_list<uint32_t> values){for(auto value:values)v(value);return *this;}
    Payload& bytes(const ByteBuffer& b){insert(end(),b.begin(),b.end());return *this;}
    template<class T>Payload& raw(const T& value){auto* p=reinterpret_cast<const uint8_t*>(&value);insert(end(),p,p+sizeof value);return *this;}
};
struct Record {uint8_t category;uint16_t type;ByteBuffer payload;};
struct WireFixture {
    std::map<uint64_t,Record> records;uint64_t next=4;Json helper=Json::array(),application=Json::array();
    uint64_t add(uint8_t category,uint16_t type,ByteBuffer payload){const auto id=next++;records.emplace(id,Record{category,type,std::move(payload)});return id;}
    uint64_t blob(const ByteBuffer& data){return add(9,1,Payload{}.v(UINT(data.size())).bytes(data));}
    template<class T>uint64_t resource(uint16_t type,const T& desc,uint64_t data=0,bool has_data=false,uint64_t owner=2){const auto id=next++;auto p=Payload{}.q({id,owner}).raw(desc);if(has_data)p.v(data);records.emplace(id,Record{5,type,p});return id;}
    uint64_t texture(const D3D11_TEXTURE2D_DESC& desc,const ByteBuffer& data,uint64_t owner=2,uint16_t type=0x85){return resource(type,desc,blob(data),true,owner);}
    template<class T>uint64_t view(uint16_t type,uint64_t texture,const T& desc){const auto id=next++;records.emplace(id,Record{5,type,Payload{}.q({id,2,texture}).raw(desc)});return id;}
    uint64_t shader(uint16_t type,const ByteBuffer& code){const auto data=add(9,0x81,Payload{}.v(uint64_t(code.size())).bytes(code).q({0}));const auto id=next++;records.emplace(id,Record{5,type,Payload{}.q({id,2,0,0,0,0,data})});return id;}
    uint64_t state(uint64_t vs,uint64_t ps,uint64_t raster,uint64_t rtv,uint64_t srv,UINT w,UINT h,UINT mask=UINT_MAX,bool partial=false,uint64_t dsv=0,uint64_t ds=0,UINT stencil=0,uint64_t cb=0,uint64_t srv1=0){
        state_block s{};s.mask.fill(UINT_MAX);s.topology=D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;s.vs_shader=vs;s.ps_shader=ps;s.ps_srv[0]=srv;s.ps_srv[1]=srv1;s.ps_cb[0]=cb;s.rasterizer=raster;s.rtv[0]=rtv;s.rt_count=rtv?1:0;s.dsv=dsv;s.depth_state=ds;s.stencil_ref=stencil;s.om_uav_start=64;s.om_uav_counts.fill(UINT_MAX);s.cs_uav_counts.fill(UINT_MAX);s.sample_mask=mask;
        D3D11_VIEWPORT vp{0,0,float(w),float(h),0,1};s.viewports=add(9,0x87,Payload{}.u({1}).raw(vp));D3D11_RECT rect{0,0,partial?LONG(width/2):LONG(w),LONG(h)};s.scissors=add(9,0x86,Payload{}.u({1}).raw(rect));const auto wire=encode_state_block(s);return add(3,3,ByteBuffer(wire.begin(),wire.end()));
    }
    void draw(uint64_t state,bool init){auto id=add(7,0x37,Payload{}.q({state,0,1}).u({3,0}));(init?helper:application).push_back(id);}
    void annotation(bool begin){if(begin){std::u16string name=u"FloraCapture initialization (experimental): per-sample contents";name.push_back(0);ByteBuffer data(name.size()*2);memcpy(data.data(),name.data(),data.size());add(7,0x327b,Payload{}.q({0,UINT64_MAX-2}).u({0,UINT(data.size())}).bytes(data));}else add(7,0x327c,Payload{}.q({0,UINT64_MAX-2}).u({0}));}
    Json write(const std::filesystem::path& path,const Owned& owned,UINT samples,bool init,bool edits){
        records.emplace(1,Record{5,0x127,Payload{}.q({1,2,0})});records.emplace(2,Record{5,0x81,Payload{}.q({2,0}).u({1,0,D3D_FEATURE_LEVEL_11_0})});
        DXGI_SWAP_CHAIN_DESC swap{};swap.BufferDesc.Width=width*samples*2;swap.BufferDesc.Height=height*layers;swap.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;swap.SampleDesc.Count=1;swap.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;swap.BufferCount=1;swap.Windowed=TRUE;records.emplace(3,Record{5,0x38,Payload{}.q({3,2}).raw(swap)});
        D3D11_TEXTURE2D_DESC td{width,height,1,layers,DXGI_FORMAT_R32G8X24_TYPELESS,{samples,0},D3D11_USAGE_DEFAULT,D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE,0,0};
        auto original=texture(td,{});td.BindFlags=D3D11_BIND_DEPTH_STENCIL;auto temporary=texture(td,{});
        const auto vs=shader(0x90,compile(vertex,"vs_5_0")),depth_ps=shader(0x92,compile(restore_depth,"ps_5_0")),stencil_ps=shader(0x92,compile(restore_stencil,"ps_5_0")),edit_ps=shader(0x92,compile(change,"ps_5_0")),show_ps=shader(0x92,compile(display,"ps_5_0"));
        D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;rd.MultisampleEnable=TRUE;auto raster=resource(0x89,rd);rd.ScissorEnable=TRUE;auto scissor=resource(0x89,rd);
        const auto depth_only=resource(0x8b,depth_state(false)),depth_edit=resource(0x8b,depth_state());
        std::array<uint64_t,8> stencil_states{},bit_constants{};
        for(UINT bit=0;bit<8;++bit){auto sd=depth_state();sd.DepthEnable=FALSE;sd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;sd.StencilWriteMask=uint8_t(1u<<bit);stencil_states[bit]=resource(0x8b,sd);D3D11_BUFFER_DESC bd{16,D3D11_USAGE_DEFAULT,D3D11_BIND_CONSTANT_BUFFER,0,0,0};bit_constants[bit]=resource(0x83,bd,blob(Payload{}.u({1u<<bit,0,0,0})),true);}
        std::array<uint64_t,layers> original_views{},temporary_views{};
        for(UINT l=0;l<layers;++l){D3D11_DEPTH_STENCIL_VIEW_DESC vd{};vd.Format=DXGI_FORMAT_D32_FLOAT_S8X24_UINT;vd.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY;vd.Texture2DMSArray={l,1};original_views[l]=view(0x8e,original,vd);temporary_views[l]=view(0x8e,temporary,vd);}
        annotation(true);
        for(auto v:temporary_views)add(7,0x31,Payload{}.q({0,1,v}).u({3}).v(0.0f).v(uint8_t(0)));
        for(const auto& plane:owned.samples.planes){
            D3D11_TEXTURE2D_DESC d{width,height,1,1,DXGI_FORMAT_R32G32_UINT,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,0};auto data=texture(d,plane.bytes);D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=d.Format;sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sd.Texture2D.MipLevels=1;auto srv=view(0x8c,data,sd);
            auto s=state(vs,depth_ps,raster,0,srv,width,height,1u<<plane.sample,false,temporary_views[plane.array_slice],depth_only);if(init)draw(s,true);
            for(UINT bit=0;bit<8;++bit){s=state(vs,stencil_ps,raster,0,srv,width,height,1u<<plane.sample,false,temporary_views[plane.array_slice],stencil_states[bit],1u<<bit,bit_constants[bit]);if(init)draw(s,true);}
        }
        auto copy=add(7,0x3e,Payload{}.q({0,1,original,temporary}));helper.push_back(copy);annotation(false);
        if(edits)for(auto v:original_views)draw(state(vs,edit_ps,scissor,0,0,width,height,0x55555555,true,v,depth_edit,0xa6),false);
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;sd.Texture2DMSArray.ArraySize=layers;auto srv=view(0x8c,original,sd);sd.Format=DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;auto stencil_srv=view(0x8c,original,sd);
        td={width*samples*2,height*layers,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET,0,0};auto output=texture(td,ByteBuffer(owned.final.size()),3);D3D11_RENDER_TARGET_VIEW_DESC vd{};vd.Format=td.Format;vd.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;auto rtv=view(0x8d,output,vd);draw(state(vs,show_ps,raster,rtv,srv,td.Width,td.Height,UINT_MAX,false,0,0,0,0,stencil_srv),false);texture(td,owned.final,3,0x87);add(7,0x3257,Payload{}.q({0,3}).u({0,1,0}));
        Header h{};memcpy(h.data(),"IGPA",4);wire_write<uint32_t>(h.data()+4,HEADER_SIZE);wire_write<uint32_t>(h.data()+8,VERSION);memcpy(h.data()+API_OFFSET,"DX11",4);memcpy(h.data()+0x4c,"DX11_0",6);memcpy(h.data()+0xd4,"FloraDepthExperiment",20);wire_write<uint32_t>(h.data()+0xfc,6);wire_write(h.data()+DIMENSIONS_OFFSET,td.Width);wire_write(h.data()+DIMENSIONS_OFFSET+4,td.Height);
        ByteBuffer bmp(54+owned.final.size());bmp[0]='B';bmp[1]='M';wire_write(bmp.data()+2,UINT(bmp.size()));wire_write<UINT>(bmp.data()+10,54);wire_write<UINT>(bmp.data()+14,40);wire_write(bmp.data()+18,td.Width);wire_write<int32_t>(bmp.data()+22,-int32_t(td.Height));wire_write<uint16_t>(bmp.data()+26,1);wire_write<uint16_t>(bmp.data()+28,32);wire_write(bmp.data()+34,UINT(owned.final.size()));for(size_t i=0;i<owned.final.size();i+=4){bmp[54+i]=owned.final[i+2];bmp[55+i]=owned.final[i+1];bmp[56+i]=owned.final[i];bmp[57+i]=owned.final[i+3];}
        ContainerWriter writer(path,h);for(const auto& [id,r]:records)writer.append({id,r.category,r.type,r.payload.data(),r.payload.size()});writer.finish(bmp);
        return Json{{"file",path.filename().string()},{"samples",samples},{"layers",layers},{"initialization",init},{"application_edits",edits},{"helper_events",helper},{"application_draws",application},{"msaa_resource",original},{"helper_resource",temporary},{"scope","Experimental D32S8 Draw/Copy initialization; production depth MSAA capture still refused"}};
    }
};
void bytes(const std::filesystem::path& path,const ByteBuffer& value){std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(value.data()),value.size());require(bool(file),"Write oracle failed");}
}
int wmain(int argc,wchar_t** argv){try{bool hardware=false;std::filesystem::path out;for(int i=1;i<argc;++i){if(std::wstring(argv[i])==L"--hardware")hardware=true;else out=argv[i];}const bool temporary=out.empty();if(temporary)out=std::filesystem::temp_directory_path()/("FloraMsaaDepthWire-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));require(std::filesystem::create_directories(out),"Output already exists");Json results=Json::array();
    for(UINT samples:{2u,4u}){auto data=native(hardware,samples);require(object_status().at("active")==0 && metadata_status().at("alive")==0,"Source objects alive before encoding");for(UINT mode=0;mode<3;++mode){const auto name=std::to_string(samples)+"x-"+(mode==0?"complete":mode==1?"missing-initialization":"missing-application-draws");WireFixture fixture;results.push_back(fixture.write(out/(name+".gpa_frame"),data,samples,mode!=1,mode!=2));bytes(out/(name+".expected.rgba"),expected(samples,mode!=2,mode!=1));}}
    if(!out.empty()){std::ofstream file(out/"frames.json");file<<results.dump(2);require(bool(file),"Write manifest failed");}if(temporary){for(const auto& entry:std::filesystem::directory_iterator(out)){require(entry.is_regular_file(),"Unexpected fixture output kind");std::filesystem::remove(entry.path());}std::filesystem::remove(out);}std::cout<<Json{{"passed",true},{"hardware",hardware},{"source_gpu_objects_alive",0},{"debug_warnings_or_errors",0},{"files",results.size()}}.dump()<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
