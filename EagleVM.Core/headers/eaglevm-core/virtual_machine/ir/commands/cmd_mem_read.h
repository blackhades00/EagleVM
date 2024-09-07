#pragma once
#include "eaglevm-core/virtual_machine/ir/commands/base_command.h"
#include "eaglevm-core/virtual_machine/ir/commands/models/cmd_stack.h"

namespace eagle::ir
{
    class cmd_mem_read : public base_command
    {
    public:
        explicit cmd_mem_read(ir_size size);

        ir_size get_read_size() const;
        bool is_similar(const std::shared_ptr<base_command>& other) override;

    private:
        ir_size size;
    };
}
