#pragma once

#include "eaglevm-core/virtual_machine/ir/commands/cmd_logic.h"
#include "eaglevm-core/virtual_machine/ir/commands/cmd_branch.h"
#include "eaglevm-core/virtual_machine/ir/commands/cmd_compare.h"
#include "eaglevm-core/virtual_machine/ir/commands/cmd_context_load.h"
#include "eaglevm-core/virtual_machine/ir/commands/cmd_context_store.h"
#include "eaglevm-core/virtual_machine/ir/commands/cmd_handler_call.h"
#include "eaglevm-core/virtual_machine/ir/commands/cmd_jmp.h"
#include "eaglevm-core/virtual_machine/ir/commands/cmd_mem_read.h"
#include "eaglevm-core/virtual_machine/ir/commands/cmd_mem_write.h"
#include "eaglevm-core/virtual_machine/ir/commands/cmd_pop.h"
#include "eaglevm-core/virtual_machine/ir/commands/cmd_push.h"
#include "eaglevm-core/virtual_machine/ir/commands/cmd_context_rflags_load.h"
#include "eaglevm-core/virtual_machine/ir/commands/cmd_context_rflags_store.h"
#include "eaglevm-core/virtual_machine/ir/commands/cmd_sx.h"
#include "eaglevm-core/virtual_machine/ir/commands/cmd_resize.h"
#include "eaglevm-core/virtual_machine/ir/commands/cmd_vm_enter.h"
#include "eaglevm-core/virtual_machine/ir/commands/cmd_vm_exit.h"
#include "eaglevm-core/virtual_machine/ir/commands/cmd_x86_exec.h"
#include "eaglevm-core/virtual_machine/ir/commands/cmd_flags_load.h"
#include "eaglevm-core/virtual_machine/ir/commands/cmd_cf.h"
#include "eaglevm-core/virtual_machine/ir/commands/cmd_carry.h"
