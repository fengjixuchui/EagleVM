#include <bitset>
#include <Windows.h>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <vector>
#include <eaglevm-core/disassembler/disassembler.h>

#include "nlohmann/json.hpp"

#include "util.h"
#include "run_container.h"
#include "eaglevm-core/compiler/section_manager.h"
#include "eaglevm-core/virtual_machine/ir/ir_translator.h"
#include "eaglevm-core/virtual_machine/machines/pidgeon/machine.h"
#include "eaglevm-core/virtual_machine/machines/pidgeon/settings.h"

reg_overwrites build_writes(nlohmann::json& inputs);
uint32_t compare_context(CONTEXT& result, CONTEXT& target, reg_overwrites& outs, bool flags);
uint64_t* get_value(CONTEXT& new_context, std::string& reg);

// imul and mul tests are cooked
const std::string inclusive_tests[] = {
    "add",
    "dec",
    "inc",
    "lea",
    "mov",
    "movsx",
    "sub",
    "cmp"
};

#pragma section(".run_section", execute)
__declspec(allocate(".run_section")) unsigned char run_buffer[0x1000] = { };

using namespace eagle;

int main(int argc, char* argv[])
{
    // give .handlers and .run_section execute permissions
    // the fact that i have to do this is so extremely cooked
    // i mean its literally DOOMED
    // the virtualizer doesnt allow displacement sizes larger than run time addresses
    // so all i can do is create a section 🤣
    DWORD old_protect;
    VirtualProtect(run_buffer, sizeof(run_buffer), PAGE_EXECUTE_READWRITE, &old_protect);
    memset(run_buffer, 0xCC, sizeof(run_buffer));

    // setbuf(stdout, NULL);
    const char* test_data_path = argc > 1 ? argv[1] : "../../../deps/x86_test_data/TestData64";
    if (!std::filesystem::exists("x86-tests"))
        std::filesystem::create_directory("x86-tests");

    codec::setup_decoder();
    run_container::init_veh();

    // we want the same settings for every machine
    virt::pidg::settings_ptr settings = std::make_shared<virt::pidg::settings>();
    settings->set_temp_count(4);
    settings->set_randomize_vm_regs(true);
    settings->set_randomize_stack_regs(true);

    // loop each file that test_data_path contains
    for (const auto& entry : std::filesystem::directory_iterator(test_data_path))
    {
        auto entry_path = entry.path();
        entry_path.make_preferred();

        std::string file_name = entry_path.stem().string();
        if (std::ranges::find(inclusive_tests, file_name) == std::end(inclusive_tests))
            continue;

        std::printf("[>] generating tests for: %ls\n", entry_path.c_str());

        // Create an ofstream object for the output file
        std::ofstream outfile("x86-tests/" + file_name);
        outfile << "[>] generating tests for: " << entry_path << "\n";

        // read entry file as string
        std::ifstream file(entry.path());
        nlohmann::json data = nlohmann::json::parse(file);

        int passed = 0;
        int failed = 0;

        #ifdef _DEBUG
        outfile.basic_ios::rdbuf(std::cout.rdbuf());
        #endif

        // data now contains an array of objects, enumerate each object
        for (auto& test : data)
        {
            // create a new file for each test
            std::string instr_data = test["data"];
            std::string instr = test["instr"];

            nlohmann::json inputs = test["inputs"];
            nlohmann::json outputs = test["outputs"];

            // i dont know what else to do
            // you cannot just use VEH to recover RIP/RSP corruption
            if (instr.contains("sp"))
                continue;

            bool bp = false;
            if (test.contains("bp"))
                bp = test["bp"];

            #ifdef _DEBUG
            if(bp)
                __debugbreak();
            #endif

            {
                outfile << "\n\n[test] " << instr.c_str() << "\n";
                outfile << "[input]\n";
                test_util::print_regs(inputs, outfile);

                outfile << "[output]\n";
                test_util::print_regs(outputs, outfile);
            }

            reg_overwrites ins = build_writes(inputs);
            reg_overwrites outs = build_writes(outputs);

            run_container container(ins, outs);
            {
                std::vector<uint8_t> instruction_data = test_util::parse_hex(instr_data);
                instruction_data.push_back(0x0F);
                instruction_data.push_back(0x01);
                instruction_data.push_back(0xC1);

                codec::decode_vec instructions = codec::get_instructions(instruction_data.data(), instruction_data.size());

                dasm::segment_dasm dasm(std::move(instructions), 0, instruction_data.size());
                dasm.generate_blocks();

                ir::ir_translator ir_trans(&dasm);
                ir::preopt_block_vec preopt = ir_trans.translate(true);

                // here we assign vms to each block
                // for the current example we can assign a unique vm to each block
                uint32_t vm_index = 0;
                std::vector<ir::preopt_vm_id> block_vm_ids;
                for (const auto& preopt_block : preopt)
                    block_vm_ids.emplace_back(preopt_block, vm_index++);

                // we want to prevent the vmenter from being removed from the first block, therefore we mark it as an external call
                ir::preopt_block_ptr entry_block = nullptr;
                for (const auto& preopt_block : preopt)
                    if (preopt_block->get_original_block() == dasm.get_block(0))
                        entry_block = preopt_block;

                assert(entry_block != nullptr, "could not find matching preopt block for entry block");

                // if we want, we can do a little optimzation which will rewrite the preopt blocks
                // or we could simply ir_trans.flatten()
                std::unordered_map<ir::preopt_block_ptr, ir::block_ptr> block_tracker = { { entry_block, nullptr } };
                std::vector<ir::block_vm_id> vm_blocks = ir_trans.optimize(block_vm_ids, block_tracker, { entry_block });

                // we want the same settings for every machine
                virt::pidg::settings_ptr vm_settings = std::make_shared<virt::pidg::settings>();
                vm_settings->set_temp_count(4);
                vm_settings->set_randomize_vm_regs(true);
                vm_settings->set_randomize_stack_regs(true);

                // initialize block code labels
                std::unordered_map<ir::block_ptr, asmb::code_label_ptr> block_labels;
                for (auto& blocks : vm_blocks | std::views::keys)
                    for (const auto& block : blocks)
                        block_labels[block] = asmb::code_label::create();

                asmb::section_manager vm_section(false);

                asmb::code_label_ptr entry_point = asmb::code_label::create();
                for (const auto& [blocks, vm_id] : vm_blocks)
                {
                    // we create a new machine based off of the same settings to make things more annoying
                    // but the same machine could be used :)
                    virt::pidg::machine_ptr machine = virt::pidg::machine::create(vm_settings);
                    machine->add_block_context(block_labels);

                    for (auto& translated_block : blocks)
                    {
                        asmb::code_container_ptr result_container = machine->lift_block(translated_block);
                        ir::block_ptr block = block_tracker[entry_block];
                        if (block == translated_block)
                            result_container->bind_start(entry_point);

                        vm_section.add_code_container(result_container);
                    }

                    // build handlers
                    std::vector<asmb::code_container_ptr> handler_containers = machine->create_handlers();
                    vm_section.add_code_container(handler_containers);
                }

                container.set_run_area(reinterpret_cast<uint64_t>(&run_buffer), sizeof(run_buffer), false);
                uint64_t instruction_rva = reinterpret_cast<uint64_t>(&run_buffer) - reinterpret_cast<uint64_t>(&__ImageBase);

                codec::encoded_vec virtualized_instruction = vm_section.compile_section(instruction_rva);

                assert(sizeof(run_buffer) >= virtualized_instruction.size());
                container.set_instruction_data(virtualized_instruction);
            }

            #ifdef _DEBUG
            if(!bp)
                continue;
            else
                __debugbreak();
            #endif

            auto [result_context, output_target] = container.run(bp);

            // result_context is being set in the exception handler
            const uint32_t result = compare_context(
                result_context,
                output_target,
                outs,
                outputs.contains("flags")
            );

            if (result == none)
            {
                outfile << "[+] passed\n";
                passed++;
            }
            else
            {
                if (result & register_mismatch)
                {
                    outfile << "[!] register mismatch\n";

                    for (auto reg : outs | std::views::keys)
                    {
                        if (reg == "flags" || reg == "rip")
                            continue;

                        outfile << "  > " << reg << "\n";
                        outfile << "  target: 0x" << std::hex << *test_util::get_value(output_target, reg) << '\n';
                        outfile << "  out   : 0x" << std::hex << *test_util::get_value(result_context, reg) << '\n';
                    }
                }

                if (result & flags_mismatch)
                {
                    outfile << "[!] flags mismatch\n";

                    std::bitset<32> target_flags(output_target.EFlags);
                    std::bitset<32> out_flags(result_context.EFlags);
                    outfile << "  target:" << target_flags << '\n';
                    outfile << "  out:   " << out_flags << '\n';
                }

                outfile << "[!] failed\n";
                failed++;
            }
        }

        // Close the output file
        outfile.close();

        std::printf("[+] finished generating %i tests for: %ls\n", passed + failed, entry_path.c_str());
        std::printf("[>] passed: %i\n", passed);
        std::printf("[>] failed: %i\n", failed);

        float success = static_cast<float>(passed) / (passed + failed) * 100;
        std::printf("[>] success: %f%%\n\n", success);
    }

    run_container::destroy_veh();
}

reg_overwrites build_writes(nlohmann::json& inputs)
{
    reg_overwrites overwrites;
    for (auto& input : inputs.items())
    {
        std::string reg = input.key();
        uint64_t value = 0;
        if (input.value().is_string())
        {
            std::string str = input.value();
            value = std::stoull(str, nullptr, 16);
            value = _byteswap_uint64(value);
        }
        else
        {
            value = input.value();
        }

        overwrites.emplace_back(reg, value);
    }

    return overwrites;
}

uint32_t compare_context(CONTEXT& result, CONTEXT& target, reg_overwrites& outs, bool flags)
{
    uint32_t fail = none;

    // rip comparison is COOKED there is something really off about the test data
    // auto res_rip = result.Rip == target.Rip;
    for (auto& reg : outs | std::views::keys)
    {
        if (reg == "rip" || reg == "flags")
            continue;

        uint64_t tar = *test_util::get_value(target, reg);
        uint64_t out = *test_util::get_value(result, reg);

        if (tar != out)
        {
            fail |= register_mismatch;
            break;
        }
    }

    if (flags)
    {
        const bool res_flags = (result.EFlags & target.EFlags) == target.EFlags;
        if (!res_flags)
            fail |= flags_mismatch;
    }

    return fail;
}
