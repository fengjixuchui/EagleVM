#pragma once
#include "basic_block.h"
#include "eaglevm-core/codec/zydis_helper.h"
#include "eaglevm-core/util/util.h"

namespace eagle::dasm
{
    class segment_dasm
    {
    public:
        explicit segment_dasm(const codec::decode_vec&& segment, uint64_t binary_rva, uint64_t binary_end);

        basic_block* generate_blocks();

        std::pair<uint64_t, block_jump_location> get_jump(const basic_block* block, bool last = false);
        block_jump_location get_jump_location(uint64_t rva) const;

        basic_block* get_block(uint64_t rva) const;

        std::vector<basic_block*> blocks;
        basic_block* root_block;

    private:
        uint64_t rva_begin;
        uint64_t rva_end;

        codec::decode_vec function;
    };
}
