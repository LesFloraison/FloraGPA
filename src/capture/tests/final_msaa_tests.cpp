#include "msaa_native_fixture.hpp"
#include "final_msaa.hpp"

namespace {
ByteBuffer read(const std::filesystem::path& path){
    std::ifstream file(path,std::ios::binary);require(bool(file),"Cannot read diagnostic output");
    return ByteBuffer(std::istreambuf_iterator<char>(file),{});
}
Json manifest(const std::filesystem::path& path){std::ifstream file(path/"manifest.json");return Json::parse(file);}
}
int wmain(int argc,wchar_t** argv){try{
    const bool hardware=argc==2 && std::wstring(argv[1])==L"--hardware";
    const auto root=std::filesystem::temp_directory_path()/("FloraFinalMsaa-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
    require(std::filesystem::create_directory(root),"Diagnostic test output exists");
    unsigned planes=0,refusals=0;
    for(UINT samples:{2u,4u}){
        FrozenFrame frozen;
        native(hardware,samples,&frozen,{},false,false,false,[&](ID3D11DeviceContext* context,const RecordedFrame& frame){
            const auto out=root/std::to_string(samples);diagnostics::export_final_msaa(context,frame,out);
            const auto report=manifest(out);require(report.at("readbacks_complete") && !report.at("acceptance_established"),"Wrong diagnostic completion status");
            require(report.at("resources").size()==1,"Wrong retained resource count");
            const auto& row=report.at("resources")[0];const auto id=row.at("runtime_resource").get<ObjectId>();
            require(row.at("width")==width && row.at("height")==height && row.at("samples")==samples,"Wrong resource shape");
            size_t total=0;
            for(const auto& plane:row.at("planes")){
                const auto layer=plane.at("array_slice").get<UINT>(),sample=plane.at("sample").get<UINT>();
                ByteBuffer oracle;for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x)for(auto word:sample_words(x,y,layer,sample,true)){oracle.push_back(uint8_t(word));oracle.push_back(uint8_t(word>>8));}
                const auto bytes=read(out/plane.at("file").get<std::string>());require(bytes==oracle,"Final samples differ from independent CPU oracle");
                require(plane.at("row_bytes")==width*8 && plane.at("bytes")==bytes.size(),"Wrong plane layout");
                if(!(sample%2))require(bytes!=frame.initial_msaa.at(id).planes.at(layer*samples+sample).bytes,"Export used initial rather than final samples");
                total+=bytes.size();++planes;
            }
            require(report.at("bytes")==total,"Wrong aggregate size");
            auto reject=[&](const RecordedFrame& candidate,SnapshotOptions options,const char* name,const char* reason){
                const auto destination=root/(std::to_string(samples)+"-"+name);bool refused=false;
                try{diagnostics::export_final_msaa(context,candidate,destination,options);}catch(const std::exception& e){require(std::string(e.what()).find(reason)!=std::string::npos,e.what());refused=true;}
                require(refused && !manifest(destination).at("readbacks_complete").get<bool>(),"Failure marked complete");++refusals;
            };
            SnapshotOptions limited;limited.byte_limit=total-1;reject(frame,limited,"budget","byte limit");
            SnapshotOptions cancelled;cancelled.cancelled=[]{return true;};reject(frame,cancelled,"cancel","cancelled");
            auto missing=frame;missing.objects.at(id).native.Reset();reject(missing,{},"lease","lease missing");
            auto empty=frame;empty.initial_msaa.clear();reject(empty,{},"empty","No retained");
            const auto original=read(out/"manifest.json");bool refused=false;
            try{diagnostics::export_final_msaa(context,frame,out);}catch(const std::exception& e){require(std::string(e.what()).find("already exists")!=std::string::npos,e.what());refused=true;}
            require(refused && read(out/"manifest.json")==original,"Existing evidence changed");++refusals;
        });
        require(frozen.commands.size()==3 && frozen.initial_msaa.size()==1,"Diagnostics entered captured event stream");
        require(object_status().at("active")==0 && metadata_status().at("alive")==0,"Diagnostics retained native objects");
    }
    // Only remove files in the unique directory this test created.
    for(const auto& directory:std::filesystem::directory_iterator(root)){
        require(directory.is_directory(),"Unexpected diagnostic test output");
        for(const auto& file:std::filesystem::directory_iterator(directory.path())){require(file.is_regular_file(),"Unexpected diagnostic nested directory");std::filesystem::remove(file.path());}
        std::filesystem::remove(directory.path());
    }
    std::filesystem::remove(root);
    std::cout<<Json{{"passed",true},{"hardware",hardware},{"planes",planes},{"refusals",refusals},{"debug_warnings_or_errors",0},{"acceptance_established",false}}.dump()<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
