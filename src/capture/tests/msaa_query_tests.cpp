#ifdef FLORA_MSAA_DEPTH
#include "msaa_depth_encoding_fixture.hpp"
#else
#include "msaa_native_fixture.hpp"
#endif
#include "encoder.hpp"
#include "query_dependencies.hpp"

int wmain(int argc,wchar_t** argv){try{
    bool hardware=false;std::filesystem::path out;
    for(int i=1;i<argc;++i){if(std::wstring(argv[i])==L"--hardware")hardware=true;else out=argv[i];}
    const bool temporary=out.empty();if(temporary)out=std::filesystem::temp_directory_path()/("FloraMsaaQueries-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
    require(std::filesystem::create_directories(out),"Query output already exists");
    Json manifest=Json::array();unsigned refusals=0;
    for(UINT samples:{2u,4u}){
        FrozenFrame frame;const auto owned=native(hardware,samples,&frame,{},true);
        require(object_status().at("active")==0 && metadata_status().at("alive")==0,"Query fixture retained source objects");
        require(frame.commands.size()==6 && frame.initial_msaa.size()==1,"Internal GPU calls entered application commands");
        const auto dependencies=analyze_event_dependencies(frame);
        require(dependencies.ends==1 && dependencies.observations.size()==2,"EVENT command counts changed");
        for(const auto& q:dependencies.observations)require(q.scope==EventObservationScope::preframe_completed,"Native EVENT did not complete before frame");
        require(frame.commands.back().query->last_end.generation==2,"EVENT generation lost");
        const auto& query=frame.objects.at(frame.commands[0].operand);
        // Metadata probes the highest supported interface, including Query1 on
        // objects originally returned by CreateQuery.
        require(query.kind=="query" || query.kind=="query1","Query identity lost");
        require(frame.commands[1].operand==query.id && frame.commands.back().operand==query.id,"EVENT aliases diverged");
        const auto& planes=frame.initial_msaa.begin()->second.planes;
        require(planes.size()==owned.samples.planes.size(),"Sample planes missing");
        for(size_t i=0;i<planes.size();++i)require(planes[i].bytes==owned.samples.planes[i].bytes,"Query changed initial MSAA data");
        auto reject=[&](FrozenFrame invalid,const char* diagnostic){
            const auto file=out/("reject-"+std::to_string(refusals)+".gpa_frame");bool refused=false;
            try{encode_frame(invalid,file);}catch(const std::exception& e){require(std::string(e.what()).find(diagnostic)!=std::string::npos,e.what());refused=true;}
            require(refused && !std::filesystem::exists(file),"Unverified query published a frame");++refusals;
        };
        auto invalid=frame;invalid.start_completion.reset();reject(std::move(invalid),"EVENT query serialization");
        invalid=frame;invalid.start_completion->value=0;reject(std::move(invalid),"Invalid frame start completion");
        invalid=frame;invalid.commands[0].result=S_FALSE;invalid.commands[0].query->value.reset();reject(std::move(invalid),"contradicts frame start");
        invalid=frame;invalid.commands[0].query.reset();reject(std::move(invalid),"Query observation missing");
        invalid=frame;invalid.commands[0].query->last_end.context=UINT64_MAX;reject(std::move(invalid),"EVENT query serialization");
        invalid=frame;invalid.commands[0].words[1]=2;reject(std::move(invalid),"Invalid GetData flags");
        invalid=frame;invalid.commands.back().query->end_sequence=0;reject(std::move(invalid),"End provenance mismatch");
        invalid=frame;invalid.commands[2].bindings.predicate=frame.commands[0].operand;reject(std::move(invalid),"predicated MSAA");
        for(bool completed:{false,true}){
            invalid=frame;auto observation=invalid.commands.back();observation.sequence=invalid.commands.size();
            observation.kind=CommandKind::query_get_data;observation.api="ID3D11DeviceContext::GetData";
            observation.result=completed?S_OK:S_FALSE;observation.words={sizeof(BOOL),0};observation.query->has_output=true;
            if(completed)observation.query->value=1;invalid.commands.push_back(std::move(observation));
            if(completed){
                // Matched in-frame completion is supported by the ordinary-query
                // replay wait. Missing provenance must still prevent publication.
                const auto analysis=analyze_event_dependencies(invalid);
                require(analysis.observations.back().scope==EventObservationScope::recorded_prefix,
                        "Completed EVENT lost its recorded prefix");
                const auto accepted=out/(std::to_string(samples)+"x-completed-event.gpa_frame");
                encode_frame(invalid,accepted);
                require(std::filesystem::is_regular_file(accepted),"Verified completion was not published");
                require(std::filesystem::remove(accepted),"Cannot remove completion test output");
                invalid.commands.back().query->end_sequence.reset();
                invalid.commands.back().query->last_end={};
                reject(std::move(invalid),"GetData lost its in-frame End association");
            }else reject(std::move(invalid),"EVENT query serialization");
        }
        for(bool edits:{true,false}){
            auto copy=frame;if(!edits)copy.commands.erase(copy.commands.begin()+2,copy.commands.begin()+4);
            for(size_t i=0;i<copy.commands.size();++i){copy.commands[i].sequence=i;if(copy.commands[i].kind==CommandKind::query_end)copy.commands[i].query->end_sequence=i;}
            const auto name=std::to_string(samples)+"x-"+(edits?"complete":"missing-application-draws");
            encode_frame(copy,out/(name+".gpa_frame"));const auto bytes=expected(samples,edits);
            std::ofstream raw(out/(name+".expected.rgba"),std::ios::binary);raw.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());require(bool(raw),"Cannot write query oracle");
#ifdef FLORA_MSAA_DEPTH
            constexpr UINT passes=9;
#else
            constexpr UINT passes=1;
#endif
            manifest.push_back(Json{{"file",name+".gpa_frame"},{"samples",samples},{"application_draws",edits?3:1},{"helper_draws",passes*layers*samples}});
        }
    }
    std::ofstream manifest_file(out/"frames.json");manifest_file<<manifest.dump(2);manifest_file.close();require(bool(manifest_file),"Cannot write query manifest");
    if(temporary){for(const auto& entry:std::filesystem::directory_iterator(out)){require(entry.is_regular_file(),"Unexpected query output");std::filesystem::remove(entry.path());}std::filesystem::remove(out);}
    std::cout<<Json{{"passed",true},{"hardware",hardware},{"encoder_refusals",refusals},{"files",manifest.size()},{"source_gpu_objects_alive",0},{"debug_warnings_or_errors",0}}.dump()<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
