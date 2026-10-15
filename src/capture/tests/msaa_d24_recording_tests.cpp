#include "msaa_depth_encoding_fixture.hpp"
#include "encoder.hpp"
int wmain(int argc,wchar_t** argv){try{
    require(argc>=2,"Output directory required");const std::filesystem::path out=argv[1];
    require(std::filesystem::create_directories(out),"Output already exists");
    Json manifest=Json::array();
    for(UINT samples:{2u,4u}){
        FrozenFrame frame;const auto owned=native(true,samples,&frame);
        require(frame.initial_msaa.size()==1 && frame.initial_msaa.begin()->second.encoding==MsaaSampleEncoding::depth24_stencil8,"D24 sample encoding lost");
        require(object_status().at("active")==0 && metadata_status().at("alive")==0,"D24 recording retained native objects");
        for(bool edits:{true,false}){
            auto copy=frame;if(!edits)copy.commands.erase(copy.commands.begin(),copy.commands.begin()+2);
            for(size_t i=0;i<copy.commands.size();++i)copy.commands[i].sequence=i;
            const auto name=std::to_string(samples)+(edits?"x-complete":"x-negative");
            encode_frame(copy,out/(name+".gpa_frame"));const auto pixels=edits?owned.final:expected(samples,false);
            std::ofstream file(out/(name+".expected.rgba"),std::ios::binary);file.write(reinterpret_cast<const char*>(pixels.data()),pixels.size());require(bool(file),"Cannot save D24 oracle");
            manifest.push_back({{"file",name+".gpa_frame"},{"samples",samples},{"application_draws",edits?3:1},{"helper_draws",9*layers*samples}});
        }
    }
    std::ofstream file(out/"frames.json");file<<manifest.dump(2);require(bool(file),"Cannot save D24 manifest");
    std::cout<<"D24S8 sample preservation and negative fixtures passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
