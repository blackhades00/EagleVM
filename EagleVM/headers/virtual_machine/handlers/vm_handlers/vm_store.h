#pragma once
#include "virtual_machine/handlers/handler/vm_handler_entry.h"

class vm_store_handler : public vm_handler_entry
{
public:
    vm_store_handler(vm_register_manager* manager, vm_handler_generator* handler_generator)
        : vm_handler_entry(manager, handler_generator)
    {
        handlers = {
            { bit64, 0, HANDLER_BUILDER(construct_single) },
            { bit32, 0, HANDLER_BUILDER(construct_single) },
            { bit16, 0, HANDLER_BUILDER(construct_single) },
        };
    };

private:
    virtual void construct_single(function_container& container, reg_size size);
};