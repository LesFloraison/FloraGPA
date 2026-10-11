#include "format/Wire.h"
#include <iostream>
using namespace flora::format;
int main(){
    try{
        // High IDs, flags and misaligned input must survive wire decoding.
        const entry original{0xfedcba9876543210ull,0x123456789ull,0xabcdef12,255,9,0x127};
        const auto bytes=encode_entry(original);
        std::array<std::uint8_t,ENTRY_SIZE+1> misaligned{};
        std::memcpy(misaligned.data()+1,bytes.data(),bytes.size());
        const auto decoded=decode_entry(misaligned.data()+1,ENTRY_SIZE);
        if(encode_entry(decoded)!=bytes || decoded.id!=original.id || decoded.type!=0x127)
            throw std::runtime_error("Entry wire mismatch");
        bool refused=false;
        try{decode_state_block(bytes.data(),bytes.size());}catch(const std::invalid_argument&){refused=true;}
        if(!refused)throw std::runtime_error("Truncated state accepted");
        // Exercise every byte of every field, including signed zero and NaNs.
        std::array<std::uint8_t,STATE_SIZE> raw{};
        for(std::size_t i=0;i<raw.size();++i)raw[i]=static_cast<std::uint8_t>(i*31+7);
        wire_write<std::uint32_t>(raw.data()+STATE_BLEND_FACTOR_OFFSET,0x7fc01234);
        wire_write<std::uint32_t>(raw.data()+STATE_BLEND_FACTOR_OFFSET+4,0x80000000);
        if(encode_state_block(decode_state_block(raw.data(),raw.size()))!=raw)
            throw std::runtime_error("State wire mismatch");
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
