#pragma once
#include "eaglevm-core/virtual_machine/il/commands/models/cmd_modifier.h"
#include "eaglevm-core/virtual_machine/il/commands/models/cmd_stack.h"
#include "eaglevm-core/virtual_machine/il/commands/base_command.h"
#include "eaglevm-core/virtual_machine/il/models/il_reg.h"

namespace eagle::il
{
    class cmd_vm_push : public base_command
    {
    public:
        explicit cmd_vm_push(const reg_vm reg_src, const stack_disp reg_size)
            : base_command(command_type::vm_push), reg(reg_src), size(reg_size)
        {
            type = stack_type::vm_register;
        }

        explicit cmd_vm_push(const uint64_t immediate, const stack_disp stack_disp)
            : base_command(command_type::vm_push), value(immediate), size(stack_disp)
        {
            type = stack_type::immediate;
        }

        void set_modifier(modifier mod)
        {
            modifier = mod;
        }

    private:
        reg_vm reg = reg_vm::none;
        uint64_t value = 0;

        stack_type type;
        stack_disp size;

        std::optional<modifier> modifier = std::nullopt;
    };
}
