#ifdef FLORA_MSAA_DEPTH
#include "msaa_depth_encoding_fixture.hpp"
constexpr UINT display_tiles=2;
constexpr bool depth_profile=true;
#else
#include "msaa_native_fixture.hpp"
constexpr bool depth_profile=false;
#endif
#include "encoder.hpp"

int wmain(int argc,wchar_t** argv){try{
    bool hardware=false;std::filesystem::path out;
    for(int i=1;i<argc;++i){if(std::wstring(argv[i])==L"--hardware")hardware=true;else out=argv[i];}
    const bool temporary=out.empty();
    if(temporary)out=std::filesystem::temp_directory_path()/("FloraDeferredMsaa-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
    require(std::filesystem::create_directories(out),"Deferred MSAA output already exists");
    Json manifest=Json::array();unsigned refusals=0;
    for(UINT samples:{2u,4u}){
        FrozenFrame frame;auto owned=native(hardware,samples,&frame,{},false,true);
        require(object_status().at("active")==0 && metadata_status().at("alive")==0,"Deferred MSAA retained native owners");
#ifdef FLORA_MSAA_DEPTH
        const std::array<uint64_t,4> executions{1,1,2,2};
#else
        const std::array<uint64_t,8> executions{1,1,2,2,2,2,3,3};
#endif
        require(frame.commands.size()==executions.size()+1 && frame.initial_msaa.size()==1,"Deferred MSAA submission count changed");
        const auto& initial=frame.initial_msaa.begin()->second;
        require(initial.planes.size()==owned.samples.planes.size(),"Deferred initial samples missing");
        for(size_t i=0;i<initial.planes.size();++i)require(initial.planes[i].bytes==owned.samples.planes[i].bytes,"Repeated list replaced frame-start MSAA samples");
        ObjectId editList=0;
        for(size_t i=0;i<executions.size();++i){
            const auto& c=frame.commands[i];require(c.deferred && c.deferred->execution_ordinal==executions[i],"Deferred MSAA execution order lost");
            require(c.deferred->restore_context_state==(executions[i]!=2),"Deferred restore flag lost");
            if(i==0)editList=c.deferred->list;
            if(depth_profile || executions[i]!=2)require(c.deferred->list==editList,"Repeated list identity changed");
        }
        for(bool edits:{true,false}){
            auto copy=frame;
            if(!edits)std::erase_if(copy.commands,[](const auto& c){return c.deferred && (depth_profile || c.deferred->execution_ordinal==3);});
            for(size_t i=0;i<copy.commands.size();++i)copy.commands[i].sequence=i;
            const auto name=std::to_string(samples)+"x-"+(edits?"complete":"missing-last-execution");
            encode_frame(copy,out/(name+".gpa_frame"));
            const auto bytes=expected(samples,edits);std::ofstream file(out/(name+".expected.rgba"),std::ios::binary);
            file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());file.close();require(bool(file),"Cannot write MSAA oracle");
            manifest.push_back(Json{{"file",name+".gpa_frame"},{"samples",samples},
#ifdef FLORA_MSAA_DEPTH
                {"application_draws",edits?5:1},{"helper_draws",layers*samples*9}
#else
                {"application_draws",edits?7:5},{"helper_draws",layers*samples}
#endif
            });
        }
        auto invalid=frame;auto& c=invalid.commands[0];c.kind=CommandKind::copy_resource;c.operand=initial.resource;c.source=frame.output;
        const auto bad=out/(std::to_string(samples)+"x-invalid-copy.gpa_frame");bool rejected=false;
        try{encode_frame(invalid,bad);}catch(const std::exception& e){require(std::string(e.what()).find("MSAA mutation")!=std::string::npos,e.what());rejected=true;}
        require(rejected && !std::filesystem::exists(bad),"Deferred MSAA mutation published");++refusals;
        rejected=false;try{FrozenFrame badFrame;native(hardware,samples,&badFrame,{},false,true,true);}
        catch(const std::exception& e){require(std::string(e.what()).find("Deferred MSAA mutation")!=std::string::npos,e.what());rejected=true;}
        require(rejected,"Native deferred MSAA copy was accepted");++refusals;
        for(bool bytes:{false,true}){
            RecordingOptions options;if(bytes)options.byte_limit=1;else options.msaa_plane_limit=1;
            rejected=false;try{FrozenFrame badFrame;native(hardware,samples,&badFrame,options,false,true);}
            catch(const std::exception& e){require(std::string(e.what()).find(bytes?"byte limit":"plane limit")!=std::string::npos,e.what());rejected=true;}
            require(rejected,"Deferred MSAA budget was ignored");++refusals;
        }
    }
    require(object_status().at("active")==0 && metadata_status().at("alive")==0 && object_status().at("interface_gaps")==0,"Deferred MSAA lifetime or interface gap");
    std::ofstream file(out/"frames.json");file<<manifest.dump(2);file.close();require(bool(file),"Cannot write deferred manifest");
    if(temporary){for(const auto& e:std::filesystem::directory_iterator(out)){require(e.is_regular_file(),"Unexpected test output");std::filesystem::remove(e.path());}std::filesystem::remove(out);}
    std::cout<<Json{{"passed",true},{"hardware",hardware},{"component_bytes",display_tiles},{"files",manifest.size()},{"refusals",refusals},{"debug_warnings_or_errors",0},{"source_gpu_objects_alive",0}}.dump()<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
