#include "eaglevm-core/virtual_machine/machines/eagle/inst_regs.h"

#include <algorithm>
#include <array>

#include "eaglevm-core/codec/zydis_helper.h"
#include "eaglevm-core/util/random.h"

namespace eagle::virt::eg
{
    uint8_t inst_regs::num_v_regs = 6;
    uint8_t inst_regs::num_gpr_regs = 16;

    uint8_t inst_regs::index_vip = 0;
    uint8_t inst_regs::index_vsp = 1;
    uint8_t inst_regs::index_vregs = 2;
    uint8_t inst_regs::index_vcs = 3;
    uint8_t inst_regs::index_vcsret = 4;
    uint8_t inst_regs::index_vbase = 5;

    inst_regs::inst_regs(const settings_ptr& settings_info)
    {
        vm_order = get_gpr64_regs();
        num_v_temp = 2;
        settings = settings_info;
    }

    void inst_regs::init_reg_order()
    {
        // setup vm regs
        std::ranges::shuffle(vm_order, util::ran_device::get().gen);

        // setup allowed registers for mapping
        const auto blocked_values = num_v_regs + num_v_temp;
        for (int i = blocked_values; i < vm_order.size(); i++)
            dest_register_map[static_cast<codec::reg>(i)] = { };

        // setup allowed xmm registers for mapping
        for (int i = codec::xmm0; i <= codec::xmm15; i++)
            dest_register_map[static_cast<codec::reg>(i)] = { };
    }

    void inst_regs::create_mappings()
    {
        const std::array<codec::reg, 16> avail_regs = get_gpr64_regs();
        for (auto avail_reg : avail_regs)
        {
            std::vector<uint16_t> points;
            points.push_back(0); // starting point
            points.push_back(64); // ending point (inclusive)

            // generate random points between 0 and 62 (inclusive)
            constexpr auto numRanges = 5;
            for (uint16_t i = 0; i < numRanges - 1; ++i)
            {
                uint16_t point;
                do
                {
                    point = util::ran_device::get().gen_8() % 64; // generates random number between 0 and 63
                }
                while (std::ranges::find(points, point) != points.end());
                points.push_back(point);
            }

            // sort the points
            std::ranges::sort(points);

            // form the inclusive ranges
            std::vector<reg_range> register_ranges;
            for (size_t i = 0; i < points.size() - 1; ++i)
                register_ranges.emplace_back( points[i], points[i + 1] - 1 );

            for (auto& [first, last] : register_ranges)
            {
                const uint16_t length = last - first;

                auto find_avail_range = [](
                    const std::vector<reg_range>& occupied_ranges,
                    const uint16_t range_length,
                    const uint16_t max_bit
                ) -> std::optional<reg_range>
                {
                    std::vector<uint16_t> potential_starts;
                    for (uint16_t start = 0; start <= max_bit - range_length; ++start)
                        potential_starts.push_back(start);

                    // Shuffle the potential start points to randomize the search
                    std::ranges::shuffle(potential_starts, util::ran_device::get().gen);

                    for (const uint16_t start : potential_starts)
                    {
                        bool available = true;
                        for (const auto& [fst, snd] : occupied_ranges)
                        {
                            if (!(start + range_length <= fst || start >= snd))
                            {
                                available = false;
                                break;
                            }
                        }

                        if (available)
                        {
                            return reg_range{ start, start + range_length };
                        }
                    }

                    return std::nullopt;
                };

                // get random destination register
                std::uniform_int_distribution<uint64_t> distr(0, dest_register_map.size() - 1);
                auto it = dest_register_map.begin();
                const uint64_t random_dest_idx = util::ran_device::get().gen_dist(distr);
                std::advance(it, random_dest_idx);

                auto& [dest_reg, occ_ranges] = *it;

                // give 10 attempts to map the range
                std::optional<reg_range> found_range = std::nullopt;
                for (auto j = 0; j < 10 && found_range == std::nullopt; j++)
                    found_range = find_avail_range(occ_ranges, length, get_reg_size(dest_reg));

                assert(found_range.has_value(), "unable to find valid range to map registers");
                occ_ranges.push_back(found_range.value());

                // insert into source registers
                std::vector<reg_mapped_range>& source_register = source_register_map[avail_reg];
                source_register.push_back({ { first, last }, found_range.value(), dest_reg });
            }
        }
    }

    std::vector<reg_mapped_range> inst_regs::get_register_mapped_ranges(const codec::reg reg)
    {
        return source_register_map[reg];
    }

    std::vector<reg_range> inst_regs::get_occupied_ranges(const codec::reg reg)
    {
        return dest_register_map[reg];
    }

    std::vector<reg_range> inst_regs::get_unoccupied_ranges(const codec::reg reg)
    {
        const uint16_t bit_count = get_reg_size(reg);
        std::vector<reg_range> occupied_ranges = dest_register_map[reg];
        std::vector<reg_range> unoccupied_ranges;

        // Sort the occupied ranges by their starting point
        std::ranges::sort(occupied_ranges);

        uint16_t current_pos = 0;

        for (const auto& [fst, snd] : occupied_ranges)
        {
            if (fst > current_pos)
            {
                // there is a gap before this range
                unoccupied_ranges.emplace_back(current_pos, fst - 1);
            }

            // move the current position past this range
            current_pos = snd + 1;
        }

        // check if there is an unoccupied range after the last occupied range
        if (current_pos < bit_count)
            unoccupied_ranges.emplace_back(current_pos, bit_count - 1);

        return unoccupied_ranges;
    }

    std::array<codec::reg, 16> inst_regs::get_gpr64_regs()
    {
        std::array<codec::reg, 16> avail_regs{ };
        for (int i = codec::rax; i <= codec::r15; i++)
            avail_regs[i - codec::rax] = static_cast<codec::reg>(i);

        return avail_regs;
    }
}
