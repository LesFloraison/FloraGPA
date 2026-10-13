#include "msaa_native_fixture.hpp"
#include "encoder.hpp"
namespace {
FrozenFrame production_frame(const Owned& owned,bool edits=true){
    FrozenFrame frame;ObjectId next=1;
    auto object=[&](const char* kind,const auto& desc,ObjectId resource=0){const auto id=next++;FrozenObject o;o.id=id;o.device=id==1?0:1;o.resource=resource;o.kind=kind;o.descriptor.resize(sizeof desc);memcpy(o.descriptor.data(),&desc,sizeof desc);frame.objects.emplace(id,std::move(o));return id;};
    object("device",DeviceRecordDesc{D3D_FEATURE_LEVEL_11_0,0});object("context",ContextRecordDesc{D3D11_DEVICE_CONTEXT_IMMEDIATE,0});
    const auto samples=owned.samples.shape.samples;
    D3D11_TEXTURE2D_DESC d{width,height,1,layers,DXGI_FORMAT_R8G8B8A8_TYPELESS,{samples,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE,0,0};auto source=object("texture2d",d);auto data=owned.samples;data.resource=source;frame.initial_msaa.emplace(source,std::move(data));
    d={width*samples,height*layers,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET,0,0};frame.output=object("texture2d",d);ResourceContents output;output.shape={D3D11_RESOURCE_DIMENSION_TEXTURE2D,d.Format,d.Width,d.Height,1,1,1,1,0};output.subresources=resource_layout(output.shape);output.subresources[0].bytes.resize(owned.final.size());frame.initial_resources.emplace(frame.output,output);output.subresources[0].bytes=owned.final;frame.final_output=std::move(output);
    auto shader=[&](const char* kind,const char* text,const char* profile){auto id=object(kind,UINT(0));auto& s=frame.objects.at(id);s.creation_observed=true;s.creation_bytes=compile(text,profile);return id;};
    const auto vs=shader("vertex_shader",vertex,"vs_5_0"),edit=shader("pixel_shader",change,"ps_5_0"),show=shader("pixel_shader",display(samples).c_str(),"ps_5_0");
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;rd.MultisampleEnable=TRUE;auto raster=object("rasterizer",rd);rd.ScissorEnable=TRUE;auto scissor=object("rasterizer",rd);
    PipelineBindings bindings;bindings.device=1;bindings.context=2;bindings.feature_level=D3D_FEATURE_LEVEL_11_0;bindings.topology=D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;bindings.shaders[0].shader=vs;bindings.shaders[4].shader=edit;bindings.rasterizer=scissor;bindings.sample_mask=0x55555555;bindings.viewports={{0,0,float(width),float(height),0,1}};bindings.scissors={{0,0,width/2,height}};
    auto draw=[&]{RecordedCommand c;c.kind=CommandKind::draw;c.api="ID3D11DeviceContext::Draw";c.sequence=frame.commands.size();c.completed=true;c.words={3,0};c.bindings=bindings;frame.commands.push_back(std::move(c));};
    for(UINT layer=0;layer<layers;++layer){D3D11_RENDER_TARGET_VIEW_DESC vd{};vd.Format=DXGI_FORMAT_R8G8B8A8_UINT;vd.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;vd.Texture2DMSArray={layer,1};bindings.render_targets[0]=object("rtv",vd,source);if(edits)draw();}
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;sd.Texture2DMSArray.ArraySize=layers;bindings.shaders[4].resources[0]=object("srv",sd,source);bindings.shaders[4].shader=show;
    D3D11_RENDER_TARGET_VIEW_DESC vd{};vd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;vd.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;bindings.render_targets[0]=object("rtv",vd,frame.output);bindings.rasterizer=raster;bindings.sample_mask=UINT_MAX;bindings.scissors.clear();bindings.viewports={{0,0,float(width*samples),float(height*layers),0,1}};draw();return frame;
}
unsigned malformed(const FrozenFrame& good,const std::filesystem::path& out){
    unsigned cases=0;
    auto rejects=[&](FrozenFrame frame,const char* why,EncodeOptions options={}){const auto path=out/("reject-"+std::to_string(cases)+".gpa_frame");bool refused=false;try{encode_frame(frame,path,options);}catch(const std::exception& e){require(std::string(e.what()).find(why)!=std::string::npos,e.what());refused=true;}require(refused,"Malformed MSAA input was accepted");require(!std::filesystem::exists(path),"Malformed MSAA frame published");++cases;};
    auto f=good;f.initial_msaa.at(3).planes.pop_back();rejects(f,"plane count");
    f=good;f.initial_msaa.at(3).planes[1]=f.initial_msaa.at(3).planes[0];rejects(f,"plane identity");
    f=good;f.initial_msaa.at(3).planes[0].row_bytes++;rejects(f,"plane identity");
    f=good;f.initial_msaa.at(3).planes[0].bytes.pop_back();rejects(f,"plane identity");
    f=good;f.initial_msaa.at(3).resource=99;rejects(f,"key/resource");
    f=good;f.initial_msaa.at(3).quality=1;rejects(f,"sample count/quality");
    f=good;f.initial_msaa.at(3).shape.samples=8;rejects(f,"sample count/quality");
    f=good;f.initial_msaa.at(3).encoding=MsaaSampleEncoding::depth16;rejects(f,"format/encoding");
    f=good;f.initial_msaa.at(3).shape.width++;rejects(f,"descriptor/shape");
    f=good;f.objects.erase(3);rejects(f,"orphan MSAA");
    f=good;f.objects.at(3).device=999;rejects(f,"another device");
    f=good;f.objects.at(3).descriptor.pop_back();rejects(f,"descriptor length");
    f=good;f.objects.at(3).kind="buffer";rejects(f,"object kind");
    const auto view_id=good.commands.front().bindings.render_targets[0];
    f=good;D3D11_RENDER_TARGET_VIEW_DESC view{};memcpy(&view,f.objects.at(view_id).descriptor.data(),sizeof view);view.Texture2DMSArray.ArraySize=UINT_MAX;memcpy(f.objects.at(view_id).descriptor.data(),&view,sizeof view);rejects(f,"array range");
    f=good;memcpy(&view,f.objects.at(view_id).descriptor.data(),sizeof view);view.Format=DXGI_FORMAT_R32_UINT;memcpy(f.objects.at(view_id).descriptor.data(),&view,sizeof view);rejects(f,"view format");
    f=good;memcpy(&view,f.objects.at(view_id).descriptor.data(),sizeof view);view.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;memcpy(f.objects.at(view_id).descriptor.data(),&view,sizeof view);rejects(f,"dimension mismatch");
    f=good;f.initial_resources.emplace(3,f.final_output);rejects(f,"both ordinary");
    f=good;f.commands[0].kind=CommandKind::copy_resource;f.commands[0].operand=3;rejects(f,"mutation command");
    f=good;f.commands[0].bindings.predicate=99;rejects(f,"predicated MSAA");
    f=good;D3D11_TEXTURE2D_DESC desc{};memcpy(&desc,f.objects.at(3).descriptor.data(),sizeof desc);desc.BindFlags|=D3D11_BIND_DEPTH_STENCIL;memcpy(f.objects.at(3).descriptor.data(),&desc,sizeof desc);rejects(f,"usage/bind flags");
    f=good;memcpy(&desc,f.objects.at(3).descriptor.data(),sizeof desc);desc.MiscFlags=D3D11_RESOURCE_MISC_SHARED;f.initial_msaa.at(3).shape.misc_flags=desc.MiscFlags;memcpy(f.objects.at(3).descriptor.data(),&desc,sizeof desc);rejects(f,"special resource");
    EncodeOptions limits;limits.msaa_plane_limit=1;rejects(good,"plane limit",limits);limits={};limits.msaa_byte_limit=1;rejects(good,"byte limit",limits);
    f=good;auto second=f.initial_msaa.at(3);const auto id=f.objects.rbegin()->first+1;second.resource=id;auto definition=f.objects.at(3);definition.id=id;f.objects.emplace(id,definition);f.initial_msaa.emplace(id,second);limits={};limits.msaa_byte_limit=second.planes.size()*second.planes[0].bytes.size()*2-1;rejects(f,"byte limit",limits);
    limits={};unsigned polls=0;limits.cancelled=[&]{return ++polls==45;};rejects(good,"cancelled",limits);require(polls==45,"Cancellation missed encoder work");return cases;
}
void raw(const std::filesystem::path& path,const ByteBuffer& data){std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(data.data()),data.size());require(bool(file),"Cannot write expected bytes");}
}
int wmain(int argc,wchar_t** argv){try{bool hardware=false;std::filesystem::path out;for(int i=1;i<argc;++i){if(std::wstring(argv[i])==L"--hardware")hardware=true;else out=argv[i];}const bool temporary=out.empty();if(temporary)out=std::filesystem::temp_directory_path()/("FloraMsaaEncoder-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));require(std::filesystem::create_directories(out),"Fixture output directory already exists");unsigned negatives=0;Json manifest=Json::array();
    for(UINT samples:{2u,4u}){auto owned=native(hardware,samples);require(object_status().at("active")==0 && metadata_status().at("alive")==0,"Source COM objects alive before encode");auto frame=production_frame(owned);negatives+=malformed(frame,out);for(bool edits:{true,false}){if(!edits)frame=production_frame(owned,false);const auto name=std::to_string(samples)+"x-"+(edits?"complete":"missing-application-draws");encode_frame(frame,out/(name+".gpa_frame"));raw(out/(name+".expected.rgba"),expected(samples,edits));manifest.push_back(Json{{"file",name+".gpa_frame"},{"samples",samples},{"application_draws",edits?3:1},{"helper_draws",layers*samples}});}}
    std::ofstream file(out/"frames.json");file<<manifest.dump(2);file.close();require(bool(file),"Cannot write manifest");if(temporary){for(const auto& entry:std::filesystem::directory_iterator(out)){require(entry.is_regular_file(),"Unexpected fixture output kind");std::filesystem::remove(entry.path());}std::filesystem::remove(out);}
    std::cout<<Json{{"passed",true},{"hardware",hardware},{"encoder_refusals",negatives},{"files",manifest.size()},{"source_gpu_objects_alive",0},{"debug_warnings_or_errors",0},{"scope","Production MSAA encoder from owned native samples; runtime recording integration pending"}}.dump()<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
