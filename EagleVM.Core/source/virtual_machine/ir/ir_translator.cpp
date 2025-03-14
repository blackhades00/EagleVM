#include "eaglevm-core/virtual_machine/ir/ir_translator.h"

#include <intrin.h>
#include <ranges>
#include <unordered_set>
#include <utility>

#include "eaglevm-core/virtual_machine/ir/block.h"
#include "eaglevm-core/virtual_machine/ir/commands/cmd_x86_exec.h"
#include "eaglevm-core/virtual_machine/ir/commands/include.h"
#include "eaglevm-core/virtual_machine/ir/x86/handler_data.h"

#include "eaglevm-core/codec/zydis_helper.h"
#include "eaglevm-core/disassembler/analysis/liveness.h"
#include "eaglevm-core/disassembler/dasm.h"

namespace eagle::ir
{
    ir_translator::ir_translator(const dasm::segment_dasm_ptr& seg_dasm, dasm::analysis::liveness* liveness)
    {
        dasm = std::move(seg_dasm);
        dasm_liveness = liveness;
    }

    std::vector<preopt_block_ptr> ir_translator::translate()
    {
        // we want to initialize the entire map with bb translates
        for (const dasm::basic_block_ptr& block : dasm->get_blocks())
        {
            const auto vm_il = std::make_shared<preopt_block>();
            vm_il->init(block);

            bb_map[block] = vm_il;
        }

        std::vector<preopt_block_ptr> result;
        for (const dasm::basic_block_ptr& block : dasm->get_blocks())
            result.push_back(translate_block_split(block));

        return result;
    }

    preopt_block_ptr ir_translator::translate_block_split(dasm::basic_block_ptr bb)
    {
        preopt_block_ptr block_info = bb_map[bb];
        if (bb->decoded_insts.empty())
            return block_info;

        const block_ptr& entry = block_info->head;
        block_ptr current_block = nullptr;
        const block_ptr& exit = block_info->tail;

        //
        // entry
        //
        entry->push_back(std::make_shared<cmd_vm_enter>());

        //
        // body
        //

        std::vector<std::pair<dasm::analysis::liveness_info, dasm::analysis::liveness_info>> liveness;
        if (dasm_liveness) liveness = dasm_liveness->analyze_block(bb);

        for (uint32_t i = 0; i < bb->decoded_insts.size(); i++)
        {
            // use il x86 translator to translate the instruction to il
            auto decoded_inst = bb->decoded_insts[i];
            auto& [inst, ops] = decoded_inst;

            constexpr std::array ignored_mnemonics = { codec::m_nop };
            if (std::ranges::find(ignored_mnemonics, static_cast<codec::mnemonic>(inst.mnemonic)) != ignored_mnemonics.end())
                continue;

            handler::base_handler_gen_ptr handler_gen = nullptr;
            std::optional<uint64_t> target_handler = std::nullopt;

            codec::mnemonic mnemonic = static_cast<codec::mnemonic>(inst.mnemonic);

            // prepare the mnemonic incase its a conditional jump so that we can select the correct handler
            // there will be a cleaner way of doing this but the default jcc instruction handler is located under the
            // codec::m_jmp mnemonic which will be the way we select it here
            if (is_jmp_or_jcc(mnemonic))
                mnemonic = codec::m_jmp;

            if (instruction_handlers.contains(mnemonic))
            {
                // first we verify if there is even a valid handler for this inustruction
                // we do this by checking the handler generator for this specific handler
                std::vector<handler_op> il_operands;
                for (int j = 0; j < inst.operand_count_visible; j++)
                {
                    auto op = ops[j];
                    il_operands.emplace_back(static_cast<codec::op_type>(op.type), static_cast<codec::reg_size>(op.size));
                }

                handler_gen = instruction_handlers[mnemonic];
                target_handler = handler_gen->get_handler_id(il_operands);
            }

            bool translate_success = target_handler != std::nullopt;
            if (target_handler)
            {
                // we know that a valid handler exists for this instruction
                // this means that we can bring it into the base x86 lifter

                // now we need to find a lifter
                const uint64_t current_rva = bb->get_index_rva(i);

                auto& create_lifter = instruction_lifters[mnemonic];
                const std::shared_ptr<lifter::base_x86_translator> lifter = create_lifter(shared_from_this(), decoded_inst, current_rva);

                if (dasm_liveness)
                {
                    auto out_liveness = std::get<1>(liveness[i]);
                    auto inst_flags_liveness = dasm::analysis::liveness::compute_inst_flags(decoded_inst);

                    auto relevant_out = inst_flags_liveness & out_liveness;
                    auto relevant_out_flags = relevant_out.get_flags();

                    translate_success = lifter->translate_to_il(current_rva, static_cast<x86_cpu_flag>(relevant_out_flags));
                }
                else
                {
                    // no liveness so we assume all flags are relevant
                    auto inst_flags_liveness = dasm::analysis::liveness::compute_inst_flags(decoded_inst);
                    translate_success = lifter->translate_to_il(current_rva, static_cast<x86_cpu_flag>(inst_flags_liveness.get_flags()));
                }

                if (translate_success)
                {
                    // append basic block
                    block_ptr result_block = lifter->get_block();

                    // TODO: add way to scatter blocks instead of appending them to a single block
                    // this should probably be done post gen though
                    if (current_block == nullptr)
                    {
                        current_block = std::make_shared<block_virt_ir>();

                        cmd_branch_ptr branch = std::make_shared<cmd_branch>(current_block);
                        branch->set_virtual(true);
                        entry->push_back(branch);
                    }

                    if (current_block->get_block_state() == x86_block)
                    {
                        // the current block is an x86 block
                        const block_ptr previous = current_block;
                        block_info->body.push_back(current_block);

                        current_block = std::make_shared<block_virt_ir>();
                        current_block->push_back(std::make_shared<cmd_vm_enter>());

                        previous->push_back(std::make_shared<cmd_branch>(current_block));
                        previous->back()->get<cmd_branch>()->set_virtual(false);
                    }

                    current_block->copy_from(result_block);
                }
            }

            if (!translate_success)
            {
                if (current_block == nullptr)
                {
                    current_block = std::make_shared<block_x86_ir>();
                    entry->push_back(std::make_shared<cmd_vm_exit>(current_block));
                }

                if (current_block->get_block_state() == vm_block)
                {
                    // the current block is a vm block
                    const block_ptr previous = current_block;
                    block_info->body.push_back(current_block);

                    current_block = std::make_shared<block_x86_ir>();
                    previous->push_back(std::make_shared<cmd_vm_exit>(current_block));
                }

                // handler does not exist
                // we need to execute the original instruction
                handle_block_command(decoded_inst, current_block, bb->get_index_rva(i));
            }
        }

        // every jcc/jmp is garuanteed to get processed if its at the end of the block
        // this means we didn't process any kind of jump in the block, but we always want the block to end with a branch
        cmd_branch_ptr branch_cmd;

        switch (bb->get_end_reason())
        {
            case dasm::block_end:
            {
                // add a branch to make sure we account for this case
                // where there's on instruction to virtualize
                ir_exit_result target_exit;

                auto next_block_rva = bb->start_rva + bb->get_block_size();

                dasm::basic_block_ptr target = dasm->get_block(next_block_rva, false);
                if (target == nullptr)
                    target_exit = next_block_rva;
                else
                    target_exit = bb_map[target]->head;

                branch_cmd = std::make_shared<cmd_branch>(target_exit);
                break;
            }
            case dasm::block_jump:
            case dasm::block_conditional_jump:
            {
                // at this point "current_block" should contain a branch to the final block, we want to swap this to the exit block at the very end
                branch_cmd = std::dynamic_pointer_cast<cmd_branch>(current_block->back());
                VM_ASSERT(branch_cmd->get_command_type() == command_type::vm_branch, "final block command must be a branch");

                current_block->pop_back();
                break;
            }
            case dasm::block_ret:
            {
                // in this case we are just going to completely ignore the idea of
                // adding a tail block
                // im not actually quite sure this is the correct solution
                block_info->body.push_back(current_block);
                block_info->tail = nullptr; // delete the tail

                return block_info;
            }
        }

        // we want to jump to the exit block now
        if (current_block->get_block_state() == vm_block)
            current_block->push_back(std::make_shared<cmd_vm_exit>(exit));
        else
            current_block->push_back(std::make_shared<cmd_branch>(exit));

        // add as final block_info body
        block_info->body.push_back(current_block);

        //
        // exit
        //
        branch_cmd->set_virtual(false);
        exit->push_back(branch_cmd);

        return block_info;
    }

    std::vector<flat_block_vmid> ir_translator::flatten(std::unordered_map<preopt_block_ptr, uint32_t>& block_vm_ids,
        std::unordered_map<preopt_block_ptr, block_ptr>& block_tracker)
    {
        // for now we just flatten
        std::vector<flat_block_vmid> block_groups;
        for (const auto& [block, vm_id] : block_vm_ids)
        {
            std::vector<block_ptr> block_group;
            auto entry = block->head;
            auto body = block->body;
            auto exit = block->tail;

            if (block_tracker.contains(block))
            {
                // this is a block we want to track
                if (entry == nullptr)
                {
                    // entry point was optimized out, we want to track body
                    const block_ptr first_body = body.front();
                    block_tracker[block] = first_body;
                }
                else
                {
                    // entry point exists so we will track entry
                    block_tracker[block] = entry;
                }
            }

            block_group.push_back(entry);
            block_group.append_range(body);
            block_group.push_back(exit);
            std::erase(block_group, nullptr);

            block_groups.emplace_back(block_group, vm_id);
        }

        return block_groups;
    }

    std::vector<flat_block_vmid> ir_translator::optimize(std::unordered_map<preopt_block_ptr, uint32_t>& block_vm_ids,
        std::unordered_map<preopt_block_ptr, block_ptr>& block_tracker,
        const std::vector<preopt_block_ptr>& extern_call_blocks)
    {
        // removes head preopt block and moves it to the first body if the first body block is not a vm_block
        // every reference to the head is then rewritten to be the first body
        optimize_heads(block_vm_ids, block_tracker, extern_call_blocks);
        optimize_body_to_tail(block_vm_ids, block_tracker, extern_call_blocks);
        optimize_same_vm(block_vm_ids, block_tracker, extern_call_blocks);

        return flatten(block_vm_ids, block_tracker);
    }

    void ir_translator::optimize_heads(std::unordered_map<preopt_block_ptr, uint32_t>& block_vm_ids,
        std::unordered_map<preopt_block_ptr, block_ptr>& block_tracker,
        const std::vector<preopt_block_ptr>& extern_call_blocks)
    {
        // check each preopt block to see if vmenter head is even necessary
        for (const auto& preopt : block_vm_ids | std::views::keys)
        {
            const auto head = preopt->head;
            const auto first_body = preopt->body.front();

            if (first_body->get_block_state() != vm_block)
            {
                // this means the head is useless

                // replace all references to head with the first body
                for (const auto& seek_preopt : block_vm_ids | std::views::keys)
                {
                    const auto tail = seek_preopt->tail;
                    if (!tail)
                        continue;

                    const auto tail_branch = tail->exit_as_branch();
                    VM_ASSERT(tail_branch, "tail branch cannot be null");

                    tail_branch->rewrite_branch(head, first_body);
                }

                preopt->head = nullptr;
            }
        }
    }

    void ir_translator::optimize_body_to_tail(std::unordered_map<preopt_block_ptr, uint32_t>& block_vm_ids,
        std::unordered_map<preopt_block_ptr, block_ptr>& block_tracker, const std::vector<preopt_block_ptr>& extern_call_blocks)
    {
        // check each preopt block to see if vmenter head is even necessary
        for (const auto& preopt : block_vm_ids | std::views::keys)
        {
            const auto tail = preopt->tail;
            if (!tail)
                continue;

            // check for body size of two, i will figure a better way of oding this better
            // but wwhat this means is that the last instruction generated a branch which got copied to the tail
            // and that hte last body only contains a vm enter and vmexit ( as by design a VM preopt body MINIMALY SHOULD)
            // so we will remove it because this is not supposed to ahppen
            const auto last_body = preopt->body.back();
            if (last_body->get_block_state() == vm_block && last_body->size() == 2)
            {
                const auto first = last_body->as_virt()->at(0);
                const auto second = last_body->as_virt()->at(1);

                if (first->get_command_type() != command_type::vm_enter ||
                    second->get_command_type() != command_type::vm_exit)
                    continue;

                // this means the last body is useless
                VM_ASSERT(preopt->body.size() > 1,
                    "there should never be a case where a generated preopt body should have only 1 body that is broken");

                const auto before_last = preopt->body.at(preopt->body.size() - 2);
                before_last->as_x86()->exit_as_branch()->rewrite_branch(last_body, tail);

                preopt->body.pop_back();
            }
        }
    }

    void ir_translator::optimize_same_vm(std::unordered_map<preopt_block_ptr, uint32_t>& block_vm_ids,
        std::unordered_map<preopt_block_ptr, block_ptr>& block_tracker,
        const std::vector<preopt_block_ptr>& extern_call_blocks)
    {
        // // remove vm enter block if every reference to vm enter block uses the same vm
        // for (const auto& [preopt_block, vm_id] : block_vm_ids)
        // {
        //     const block_ptr search_enter = preopt_block->head;
        //     if (search_enter == nullptr)
        //     {
        //         // this means we removed the head
        //         // meaning the first body block doesnt use vm enter
        //         continue;
        //     }
        //
        //     const block_ptr redirect_body = preopt_block->body.front();
        //
        //     bool vm_enter_unremovable = false;
        //     std::vector<cmd_branch_ptr> search_enter_refs;
        //
        //     // check if there are any external calls to any of the body blocks
        //     // if there are, we cannot remove vm enter
        //     for (auto& external : extern_call_blocks)
        //     {
        //         if (preopt_block == external)
        //         {
        //             vm_enter_unremovable = true;
        //             goto UNREMOVABLE;
        //         }
        //     }
        //
        //     // go through each preopt block
        //     for (const auto& [search_preopt_block, search_vm_id] : block_vm_ids)
        //     {
        //         // go through each block
        //         // the only blocks that can reference our search block are exits
        //         const auto& search_block = search_preopt_block->tail;
        //
        //         const cmd_branch_ptr branch = search_block->exit_as_branch();
        //         if (branch->branch_visits(search_enter))
        //         {
        //             if (search_vm_id != vm_id)
        //                 vm_enter_unremovable = true;
        //             else
        //                 search_enter_refs.emplace_back(branch);
        //         }
        //
        //         // the only case where this happens is if a block referencing the current search block
        //         // is using a different vm id
        //         if (vm_enter_unremovable)
        //             goto UNREMOVABLE;
        //     }
        //
        // UNREMOVABLE:
        //     if (!vm_enter_unremovable)
        //     {
        //         // we found that we can replace each branch to the block with a branch to the body
        //         for (const auto& branch_ref : search_enter_refs)
        //             branch_ref->rewrite_branch(search_enter, redirect_body);
        //
        //         preopt_block->head = nullptr;
        //
        //         // TODO: make sure we are using a VIRTUAL branch for these branches
        //         // this means a vmenter has to be added before any of these branches are called
        //         // also check if
        //     }
        // }
        //
        // // remove vm exit block if every branching block uses the same vm
        // for (const auto& [preopt_block, vm_id] : block_vm_ids)
        // {
        //     // check entery of every block
        //     const block_ptr preopt_exit = preopt_block->tail;
        //     const cmd_branch_ptr branch = preopt_exit->get_branch();
        //
        //     // now we want to check if the current vm group has all the exits of this preopt
        //     auto is_same_vm = [&](const ir_exit_result& exit_result)
        //     {
        //         if (std::holds_alternative<block_ptr>(exit_result))
        //         {
        //             const block_ptr exit_block = std::get<block_ptr>(exit_result);
        //             for (const auto& [search_preopt_block, search_vm_id] : block_vm_ids)
        //             {
        //                 const bool contains = search_preopt_block->head == exit_block || search_preopt_block->tail == exit_block ||
        //                     std::ranges::find(search_preopt_block->body, exit_block) != search_preopt_block->body.end();
        //
        //                 if (contains)
        //                     return search_vm_id == vm_id;
        //             }
        //         }
        //
        //         // if one of the exits is an rva then we have to exit no matter what : (
        //         // otherwise the search would fall through to this fail
        //         return false;
        //     };
        //
        //     if (is_same_vm(branch->get_condition_default()) && is_same_vm(branch->get_condition_special()))
        //     {
        //         // this means all exits are of the same vm
        //         const size_t command_count = preopt_exit->size();
        //         VM_ASSERT(command_count <= 2 && command_count > 0, "preoptimized exit should not have more than 2 obfuscation");
        //
        //         // this means we should have a vm exit command
        //         if (command_count == 2)
        //             preopt_exit->erase(preopt_exit->begin());
        //     }
        // }
    }

    dasm::basic_block_ptr ir_translator::map_basic_block(const preopt_block_ptr& preopt_target)
    {
        for (auto& [bb, preopt] : bb_map)
            if (preopt_target == preopt)
                return bb;

        return nullptr;
    }

    preopt_block_ptr ir_translator::map_preopt_block(const dasm::basic_block_ptr& basic_block) { return bb_map[basic_block]; }

    std::pair<exit_condition, bool> ir_translator::get_exit_condition(const codec::mnemonic mnemonic)
    {
        switch (mnemonic)
        {
            // Overflow flag (OF)
            case codec::m_jo:
                return { exit_condition::jo, false };
            case codec::m_jno:
                return { exit_condition::jo, true };

            // Sign flag (SF)
            case codec::m_js:
                return { exit_condition::js, false };
            case codec::m_jns:
                return { exit_condition::js, true };

            // Zero flag (ZF)
            case codec::m_jz:
                return { exit_condition::je, false };
            case codec::m_jnz:
                return { exit_condition::je, true };

            // Carry flag (CF)
            case codec::m_jb:
                return { exit_condition::jb, false };
            case codec::m_jnb:
                return { exit_condition::jb, true };

            // Carry or Zero flag (CF or ZF)
            case codec::m_jbe:
                return { exit_condition::jbe, false };
            case codec::m_jnbe:
                return { exit_condition::jbe, true };

            // Sign flag not equal to Overflow flag (SF != OF)
            case codec::m_jl:
                return { exit_condition::jl, false };
            case codec::m_jnl:
                return { exit_condition::jl, true };

            // Zero flag or Sign flag not equal to Overflow flag (ZF or SF != OF)
            case codec::m_jle:
                return { exit_condition::jle, false };
            case codec::m_jnle:
                return { exit_condition::jle, true };

            // Parity flag (PF)
            case codec::m_jp:
                return { exit_condition::jp, false };
            case codec::m_jnp:
                return { exit_condition::jp, true };

            // CX/ECX/RCX register is zero
            case codec::m_jcxz:
                return { exit_condition::jcxz, false };
            case codec::m_jecxz:
                return { exit_condition::jecxz, false };
            case codec::m_jrcxz:
                return { exit_condition::jrcxz, false };

            // Unconditional jump
            case codec::m_jmp:
                return { exit_condition::jmp, false };

            // Conditions not in the enum, might need to be added
            case codec::m_jknzd:
                return { exit_condition::none, false };
            case codec::m_jkzd:
                return { exit_condition::none, false };

            default:
            {
                VM_ASSERT("invalid conditional jump reached");
                return { exit_condition::none, false };
            }
        }
    }

    void ir_translator::handle_block_command(codec::dec::inst_info decoded_inst, const block_ptr& current_block, const uint64_t current_rva)
    {
        using namespace codec;
        using namespace codec::encoder;

        inst_req request(static_cast<mnemonic>(decoded_inst.instruction.mnemonic));
        const enc::req encode_request = decode_to_encode(decoded_inst);

        if (contains_rip_relative_operand(decoded_inst))
        {
            auto [target_address, op_i] = calc_relative_rva(decoded_inst, current_rva);
            for (int i = 0; i < decoded_inst.instruction.operand_count_visible; i++)
            {
                const auto op = encode_request.operands[i];
                switch (op.type)
                {
                    case ZYDIS_OPERAND_TYPE_REGISTER:
                    {
                        request.operands.emplace_back(reg_op(static_cast<reg>(op.reg.value)));
                        break;
                    }
                    case ZYDIS_OPERAND_TYPE_MEMORY:
                    {
                        auto& mem = op.mem;
                        if (i == op_i)
                            request.operands.emplace_back(mem_op(static_cast<reg>(mem.base), target_address,
                                mem.size, true));
                        else
                            request.operands.emplace_back(mem_op(static_cast<reg>(mem.base), static_cast<reg>(mem.index), mem.scale, mem.displacement,
                                mem.size));

                        if (decoded_inst.operands[i].mem.segment == gs)
                            request.prefixes |= ZYDIS_ATTRIB_HAS_SEGMENT_GS;

                        break;
                    }
                    case ZYDIS_OPERAND_TYPE_IMMEDIATE:
                    {
                        if (i == op_i)
                            request.operands.emplace_back(imm_op(target_address, i == op_i));
                        else
                            request.operands.emplace_back(imm_op(op.imm.s));
                        break;
                    }
                }
            }
        }
        else
        {
            for (int i = 0; i < decoded_inst.instruction.operand_count_visible; i++)
            {
                const auto op = encode_request.operands[i];
                //if (decoded_inst.operands[i].type == ZYDIS_OPERAND_TYPE_MEMORY)
                //{
                //    if (decoded_inst.operands[i].mem.segment != ZYDIS_REGISTER_NONE)
                //        __debugbreak();
                //}

                switch (op.type)
                {
                    case ZYDIS_OPERAND_TYPE_REGISTER:
                    {
                        request.operands.emplace_back(reg_op(static_cast<reg>(op.reg.value)));
                        break;
                    }
                    case ZYDIS_OPERAND_TYPE_MEMORY:
                    {
                        auto& mem = op.mem;
                        request.operands.emplace_back(mem_op(static_cast<reg>(mem.base), static_cast<reg>(mem.index), mem.scale, mem.displacement,
                            mem.size));

                        if (decoded_inst.operands[i].mem.segment == gs)
                            request.prefixes |= ZYDIS_ATTRIB_HAS_SEGMENT_GS;

                        break;
                    }
                    case ZYDIS_OPERAND_TYPE_IMMEDIATE:
                    {
                        request.operands.emplace_back(imm_op(op.imm.s));
                        break;
                    }
                }
            }
        }

        current_block->push_back(std::make_shared<cmd_x86_exec>(request));
    }

    branch_info ir_translator::get_branch_info(const uint32_t inst_rva)
    {
        branch_info info = { };

        const auto bb = dasm->get_block(inst_rva, true);
        auto branches = bb->branches;

        switch (bb->get_end_reason())
        {
            case dasm::block_jump:
            case dasm::block_end:
            {
                // for both of these conditions, there will only be one branch
                VM_ASSERT(branches.size() == 1, "for the following condition, only one branch must be present");

                const auto block = dasm->get_block(branches[0].target_rva, false);
                if (block == nullptr)
                    info.fallthrough_branch = branches[0].target_rva;
                else
                    info.fallthrough_branch = bb_map[block]->head;

                info.exit_condition = exit_condition::jmp;
                info.inverted_condition = false;
                break;
            }
            case dasm::block_conditional_jump:
            {
                VM_ASSERT(branches.size() == 2, "for the following condition, only two branches must be present");

                // case 1 - condition succeed
                {
                    const auto block = dasm->get_block(branches[0].target_rva, false);
                    if (block == nullptr)
                        info.conditional_branch = branches[0].target_rva;
                    else
                        info.conditional_branch = bb_map[block]->head;
                }

                // case 2 - fall through
                {
                    const auto block = dasm->get_block(branches[1].target_rva, false);
                    if (block == nullptr)
                        info.fallthrough_branch = branches[1].target_rva;
                    else
                        info.fallthrough_branch = bb_map[block]->head;
                }

                const auto& [instruction, _] = bb->decoded_insts.back();
                auto [condition, invert] = get_exit_condition(static_cast<codec::mnemonic>(instruction.mnemonic));

                info.exit_condition = condition;
                info.inverted_condition = invert;
                break;
            }
        }

        return info;
    }

    void preopt_block::init(const dasm::basic_block_ptr& block)
    {
        head = std::make_shared<block_virt_ir>(block->start_rva);
        tail = std::make_shared<block_x86_ir>();
        original_block = block;
    }
}
