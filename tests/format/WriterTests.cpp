#include "format/Writer.h"
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace flora::format;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void put(Header& h, std::size_t offset, std::uint32_t value) {
    for(unsigned i=0;i<4;++i) h[offset+i]=static_cast<std::uint8_t>(value>>(8*i));
}
int main(int argc,char** argv) {
    try {
        require(argc==2,"Expected output path");
        const std::filesystem::path path=argv[1];
        if (std::filesystem::exists(path)) std::filesystem::remove(path);
        Header h{}; std::memcpy(h.data(),"IGPA",4); put(h,4,HEADER_SIZE); put(h,8,VERSION);
        std::memcpy(h.data()+API_OFFSET,"DX11",4); put(h,0xfc,6);
        const ByteBuffer payload{4,0,0,0,1,2,3,4};
        {
            ContainerWriter writer(path,h); writer.append({7,9,1,payload.data(),payload.size()});
            bool rejected=false;
            try { writer.append({7,9,1,payload.data(),payload.size()}); }
            catch (const std::invalid_argument&) { rejected=true; }
            require(rejected,"Duplicate ID accepted");
            try { writer.finish(); throw std::runtime_error("Poisoned writer committed"); }
            catch (const std::logic_error&) {}
        }
        require(!std::filesystem::exists(path),"Failed transaction published");
        {
            ContainerWriter writer(path,h);writer.append({7,9,1,payload.data(),payload.size()});writer.finish();
        }
        require(std::filesystem::file_size(path)==HEADER_SIZE+payload.size()+ENTRY_SIZE,"Wrong file length");
        bool rejected=false;
        try { ContainerWriter duplicate(path,h); } catch (const std::runtime_error&) { rejected=true; }
        require(rejected,"Existing output overwritten");
        std::cout << "transaction, duplicate, poisoned writer, size, existing output: passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
