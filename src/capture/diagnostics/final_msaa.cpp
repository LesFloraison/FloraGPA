#include "final_msaa.hpp"
#include "msaa_readback.hpp"
#include <fstream>
#include <limits>
#include <stdexcept>

namespace flora::capture::diagnostics {
namespace {
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
void write(const std::filesystem::path& path,const void* data,size_t size){
    require(size<=size_t((std::numeric_limits<std::streamsize>::max)()),"Final-MSAA file too large");
    std::ofstream stream(path,std::ios::binary);
    stream.write(static_cast<const char*>(data),static_cast<std::streamsize>(size));
    stream.close();require(bool(stream),"Cannot write final-MSAA diagnostic");
}
}
void export_final_msaa(ID3D11DeviceContext* context,const RecordedFrame& frame,
                       const std::filesystem::path& directory,const SnapshotOptions& options){
    frame.require_complete();
    require(context!=nullptr,"Final-MSAA diagnosis requires a context");
    require(std::filesystem::create_directory(directory),"Final-MSAA diagnostic directory already exists");
    Json report={{"schema",1},{"readbacks_complete",false},{"acceptance_established",false},
                 {"scope","Frame-end retained MSAA color resources; separate from GPA publication"},
                 {"resources",Json::array()}};
    auto save=[&]{const auto text=report.dump(2)+"\n";write(directory/"manifest.json",text.data(),text.size());};
    save();
    try{
        size_t used=0;
        for(const auto& [id,initial]:frame.initial_msaa){
            if(initial.encoding!=MsaaSampleEncoding::color_bits)continue;
            if(options.cancelled && options.cancelled())throw std::runtime_error("Final-MSAA diagnosis cancelled");
            size_t bytes=0;for(const auto& plane:initial.planes){
                require(used<=options.byte_limit && bytes<=options.byte_limit-used &&
                    plane.bytes.size()<=options.byte_limit-used-bytes,"Final-MSAA diagnostic byte limit exceeded");
                bytes+=plane.bytes.size();
            }
            const auto found=frame.objects.find(id);
            require(found!=frame.objects.end() && found->second.native.Get()!=nullptr,"Final-MSAA native lease missing");
            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            require(SUCCEEDED(found->second.native.As(&texture)),"Final-MSAA object is not Texture2D");
            auto remaining=options;remaining.byte_limit=options.byte_limit-used;
            const auto samples=snapshot_msaa_samples(context,texture.Get(),remaining);
            require(samples.resource==id && samples.planes.size()==initial.planes.size(),"Final-MSAA identity changed");
            Json row={{"runtime_resource",id},{"width",samples.shape.width},{"height",samples.shape.height},
                      {"format",samples.shape.format},{"array_size",samples.shape.array_size},
                      {"samples",samples.shape.samples},{"quality",samples.quality},{"planes",Json::array()}};
            size_t actual=0;
            for(size_t index=0;index<samples.planes.size();++index){
                const auto& plane=samples.planes[index];const auto& before=initial.planes[index];
                require(plane.array_slice==before.array_slice && plane.sample==before.sample &&
                    plane.row_bytes==before.row_bytes && plane.bytes.size()==before.bytes.size() &&
                    actual<=bytes && plane.bytes.size()<=bytes-actual,"Final-MSAA layout changed");actual+=plane.bytes.size();
                const auto name=std::to_string(id)+"-layer-"+std::to_string(plane.array_slice)+"-sample-"+std::to_string(plane.sample)+".bin";
                write(directory/name,plane.bytes.data(),plane.bytes.size());
                row["planes"].push_back({{"array_slice",plane.array_slice},{"sample",plane.sample},
                                         {"row_bytes",plane.row_bytes},{"bytes",plane.bytes.size()},{"file",name}});
            }
            require(actual==bytes,"Final-MSAA byte count changed");used+=actual;
            report["resources"].push_back(std::move(row));save();
        }
        require(!report["resources"].empty(),"No retained MSAA color resource for diagnosis");
        report["bytes"]=used;report["readbacks_complete"]=true;save();
    }catch(const std::exception& error){report["error"]=error.what();save();throw;}
}
}
