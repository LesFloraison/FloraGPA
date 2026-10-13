#include "msaa_native_fixture.hpp"
#include "encoder.hpp"
namespace {
void raw(const std::filesystem::path& path,const ByteBuffer& data){std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(data.data()),data.size());require(bool(file),"Cannot write recording oracle");}
}
int wmain(int argc,wchar_t** argv){try{
    bool hardware=false;std::filesystem::path out;
    for(int i=1;i<argc;++i){if(std::wstring(argv[i])==L"--hardware")hardware=true;else out=argv[i];}
    const bool temporary=out.empty();if(temporary)out=std::filesystem::temp_directory_path()/("FloraMsaaRecording-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
    require(std::filesystem::create_directories(out),"Recording output already exists");Json manifest=Json::array();unsigned refusals=0;
    for(UINT samples:{2u,4u}){
        FrozenFrame frame;auto owned=native(hardware,samples,&frame);
        require(object_status().at("active")==0 && metadata_status().at("alive")==0,"Recording retained native objects");
        require(frame.commands.size()==3 && frame.initial_msaa.size()==1,"Internal sample reads entered application commands");
        const auto& recorded=frame.initial_msaa.begin()->second;
        require(!frame.initial_resources.contains(recorded.resource),"MSAA stored as ordinary resource bytes");
        require(recorded.planes.size()==owned.samples.planes.size(),"Recording lost sample planes");
        for(size_t i=0;i<recorded.planes.size();++i)require(recorded.planes[i].bytes==owned.samples.planes[i].bytes,"Recording replaced pre-frame samples with modified samples");
        for(bool edits:{true,false}){
            auto copy=frame;if(!edits)copy.commands.erase(copy.commands.begin(),copy.commands.begin()+2);
            for(size_t i=0;i<copy.commands.size();++i)copy.commands[i].sequence=i;
            const auto name=std::to_string(samples)+"x-"+(edits?"complete":"missing-application-draws");
            encode_frame(copy,out/(name+".gpa_frame"));raw(out/(name+".expected.rgba"),expected(samples,edits));
            manifest.push_back(Json{{"file",name+".gpa_frame"},{"samples",samples},{"application_draws",edits?3:1},{"helper_draws",layers*samples}});
        }
        for(bool bytes:{false,true}){
            RecordingOptions options;if(bytes)options.byte_limit=1;else options.msaa_plane_limit=1;
            bool refused=false;try{FrozenFrame rejected;native(hardware,samples,&rejected,options);}catch(const std::exception& e){require(std::string(e.what()).find(bytes?"byte limit":"plane limit")!=std::string::npos,e.what());refused=true;}
            require(refused,"Recording budget did not reject samples");++refusals;
            require(object_status().at("active")==0 && metadata_status().at("alive")==0,"Rejected recording retained native objects");
        }
    }
    std::ofstream manifest_file(out/"frames.json");manifest_file<<manifest.dump(2);manifest_file.close();require(bool(manifest_file),"Cannot write recording manifest");
    if(temporary){for(const auto& entry:std::filesystem::directory_iterator(out)){require(entry.is_regular_file(),"Unexpected recording output kind");std::filesystem::remove(entry.path());}std::filesystem::remove(out);}
    std::cout<<Json{{"passed",true},{"hardware",hardware},{"recording_refusals",refusals},{"files",manifest.size()},{"source_gpu_objects_alive",0},{"debug_warnings_or_errors",0},{"scope","Wrapped immediate-context MSAA recording; startup injection and GF2 pending"}}.dump()<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
