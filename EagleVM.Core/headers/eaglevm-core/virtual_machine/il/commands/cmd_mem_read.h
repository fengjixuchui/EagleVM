#pragma once
#include "eaglevm-core/virtual_machine/il/commands/base_command.h"
#include "eaglevm-core/virtual_machine/il/commands/models/cmd_stack.h"

namespace eagle::il
{
    class cmd_mem_read : public base_command
    {
    public:
        explicit cmd_mem_read(const reg_vm reg_dest, const mem_size size)
            : base_command(command_type::vm_mem_read), dest_reg(reg_dest), size(size)
        {
        }

    private:
        reg_vm dest_reg;
        mem_size size;
    };
}
