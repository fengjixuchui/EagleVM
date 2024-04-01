#pragma once
#include <vector>

#include "commands/base_command.h"

namespace eagle::il
{
    class il_bb
    {
    public:
        void add_command(const base_command_ptr& command);
        bool insert_after(const base_command_ptr& command_ptr);
        bool insert_before(const base_command_ptr& command_ptr);

        bool get_inline() const;

        void set_exit(const cmd_exit_ptr& exit);
        void set_inline(bool inline_exit_block);


    private:
        std::vector<base_command_ptr> commands;
        cmd_exit_ptr exit_command;
        bool inline_next = false;

        std::vector<base_command_ptr>::iterator get_iterator(base_command_ptr command);
    };

    using il_bb_ptr = std::shared_ptr<il_bb>;
}
