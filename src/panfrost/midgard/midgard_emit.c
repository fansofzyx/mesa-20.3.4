/*
 * Copyright (C) 2018-2019 Alyssa Rosenzweig <alyssa@rosenzweig.io>
 * Copyright (C) 2019-2020 Collabora, Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "compiler.h"
#include "midgard_ops.h"
#include "midgard_quirks.h"

static midgard_int_mod
mir_get_imod(bool shift, nir_alu_type T, bool half, bool scalar)
{
        if (!half) {
                assert(!shift);
                /* Sign-extension, really... */
                return scalar ? 0 : midgard_int_normal;
        }

        if (shift)
                return midgard_int_shift;

        if (nir_alu_type_get_base_type(T) == nir_type_int)
                return midgard_int_sign_extend;
        else
                return midgard_int_zero_extend;
}

unsigned
mir_pack_mod(midgard_instruction *ins, unsigned i, bool scalar)
{
        bool integer = midgard_is_integer_op(ins->op);
        unsigned base_size = max_bitsize_for_alu(ins);
        unsigned sz = nir_alu_type_get_type_size(ins->src_types[i]);
        bool half = (sz == (base_size >> 1));

        return integer ?
                mir_get_imod(ins->src_shift[i], ins->src_types[i], half, scalar) :
                ((ins->src_abs[i] << 0) |
                 ((ins->src_neg[i] << 1)));
}

/* Midgard IR only knows vector ALU types, but we sometimes need to actually
 * use scalar ALU instructions, for functional or performance reasons. To do
 * this, we just demote vector ALU payloads to scalar. */

static int
component_from_mask(unsigned mask)
{
        for (int c = 0; c < 8; ++c) {
                if (mask & (1 << c))
                        return c;
        }

        assert(0);
        return 0;
}

static unsigned
mir_pack_scalar_source(unsigned mod, bool is_full, unsigned component)
{
        midgard_scalar_alu_src s = {
                .mod = mod,
                .full = is_full,
                .component = component << (is_full ? 1 : 0)
        };

        unsigned o;
        memcpy(&o, &s, sizeof(s));

        return o & ((1 << 6) - 1);
}

static midgard_scalar_alu
vector_to_scalar_alu(midgard_vector_alu v, midgard_instruction *ins)
{
        bool is_full = nir_alu_type_get_type_size(ins->dest_type) == 32;

        bool half_0 = nir_alu_type_get_type_size(ins->src_types[0]) == 16;
        bool half_1 = nir_alu_type_get_type_size(ins->src_types[1]) == 16;
        unsigned comp = component_from_mask(ins->mask);

        unsigned packed_src[2] = {
                mir_pack_scalar_source(mir_pack_mod(ins, 0, true), !half_0, ins->swizzle[0][comp]),
                mir_pack_scalar_source(mir_pack_mod(ins, 1, true), !half_1, ins->swizzle[1][comp])
        };

        /* The output component is from the mask */
        midgard_scalar_alu s = {
                .op = v.op,
                .src1 = packed_src[0],
                .src2 = packed_src[1],
                .unknown = 0,
                .outmod = v.outmod,
                .output_full = is_full,
                .output_component = comp
        };

        /* Full components are physically spaced out */
        if (is_full) {
                assert(s.output_component < 4);
                s.output_component <<= 1;
        }

        /* Inline constant is passed along rather than trying to extract it
         * from v */

        if (ins->has_inline_constant) {
                uint16_t imm = 0;
                int lower_11 = ins->inline_constant & ((1 << 12) - 1);
                imm |= (lower_11 >> 9) & 3;
                imm |= (lower_11 >> 6) & 4;
                imm |= (lower_11 >> 2) & 0x38;
                imm |= (lower_11 & 63) << 6;

                s.src2 = imm;
        }

        return s;
}

/* 64-bit swizzles are super easy since there are 2 components of 2 components
 * in an 8-bit field ... lots of duplication to go around!
 *
 * Swizzles of 32-bit vectors accessed from 64-bit instructions are a little
 * funny -- pack them *as if* they were native 64-bit, using rep_* flags to
 * flag upper. For instance, xy would become 64-bit XY but that's just xyzw
 * native. Likewise, zz would become 64-bit XX with rep* so it would be xyxy
 * with rep. Pretty nifty, huh? */

static unsigned
mir_pack_swizzle_64(unsigned *swizzle, unsigned max_component)
{
        unsigned packed = 0;

        for (unsigned i = 0; i < 2; ++i) {
                assert(swizzle[i] <= max_component);

                unsigned a = (swizzle[i] & 1) ?
                        (COMPONENT_W << 2) | COMPONENT_Z :
                        (COMPONENT_Y << 2) | COMPONENT_X;

                packed |= a << (i * 4);
        }

        return packed;
}

static void
mir_pack_mask_alu(midgard_instruction *ins, midgard_vector_alu *alu)
{
        unsigned effective = ins->mask;

        /* If we have a destination override, we need to figure out whether to
         * override to the lower or upper half, shifting the effective mask in
         * the latter, so AAAA.... becomes AAAA */

        unsigned inst_size = max_bitsize_for_alu(ins);
        signed upper_shift = mir_upper_override(ins, inst_size);

        if (upper_shift >= 0) {
                effective >>= upper_shift;
                alu->dest_override = upper_shift ?
                        midgard_dest_override_upper :
                        midgard_dest_override_lower;
        } else {
                alu->dest_override = midgard_dest_override_none;
        }

        if (inst_size == 32)
                alu->mask = expand_writemask(effective, 2);
        else if (inst_size == 64)
                alu->mask = expand_writemask(effective, 1);
        else
                alu->mask = effective;
}

static unsigned
mir_pack_swizzle(unsigned mask, unsigned *swizzle,
                nir_alu_type T, midgard_reg_mode reg_mode,
                bool op_channeled, bool *rep_low, bool *rep_high)
{
        unsigned packed = 0;
        unsigned sz = nir_alu_type_get_type_size(T);

        if (reg_mode == midgard_reg_mode_64) {
                assert(sz == 64 || sz == 32);
                unsigned components = (sz == 32) ? 4 : 2;

                packed = mir_pack_swizzle_64(swizzle, components);

                if (sz == 32) {
                        bool lo = swizzle[0] >= COMPONENT_Z;
                        bool hi = swizzle[1] >= COMPONENT_Z;

                        if (mask & 0x1) {
                                /* We can't mix halves... */
                                if (mask & 2)
                                        assert(lo == hi);

                                *rep_low = lo;
                        } else {
                                *rep_low = hi;
                        }
                } else if (sz < 32) {
                        unreachable("Cannot encode 8/16 swizzle in 64-bit");
                }
        } else {
                /* For 32-bit, swizzle packing is stupid-simple. For 16-bit,
                 * the strategy is to check whether the nibble we're on is
                 * upper or lower. We need all components to be on the same
                 * "side"; that much is enforced by the ISA and should have
                 * been lowered. TODO: 8-bit packing. TODO: vec8 */

                unsigned first = mask ? ffs(mask) - 1 : 0;
                bool upper = swizzle[first] > 3;

                if (upper && mask)
                        assert(sz <= 16);

                bool dest_up = !op_channeled && (first >= 4);

                for (unsigned c = (dest_up ? 4 : 0); c < (dest_up ? 8 : 4); ++c) {
                        unsigned v = swizzle[c];

                        ASSERTED bool t_upper = v > 3;

                        /* Ensure we're doing something sane */

                        if (mask & (1 << c)) {
                                assert(t_upper == upper);
                                assert(v <= 7);
                        }

                        /* Use the non upper part */
                        v &= 0x3;

                        packed |= v << (2 * (c % 4));
                }


                /* Replicate for now.. should really pick a side for
                 * dot products */

                if (reg_mode == midgard_reg_mode_16 && sz == 16) {
                        *rep_low = !upper;
                        *rep_high = upper;
                } else if (reg_mode == midgard_reg_mode_16 && sz == 8) {
                        *rep_low = upper;
                        *rep_high = upper;
                } else if (reg_mode == midgard_reg_mode_32) {
                        *rep_low = upper;
                } else {
                        unreachable("Unhandled reg mode");
                }
        }

        return packed;
}

static void
mir_pack_vector_srcs(midgard_instruction *ins, midgard_vector_alu *alu)
{
        bool channeled = GET_CHANNEL_COUNT(alu_opcode_props[ins->op].props);

        unsigned base_size = max_bitsize_for_alu(ins);

        for (unsigned i = 0; i < 2; ++i) {
                if (ins->has_inline_constant && (i == 1))
                        continue;

                if (ins->src[i] == ~0)
                        continue;

                bool rep_lo = false, rep_hi = false;
                unsigned sz = nir_alu_type_get_type_size(ins->src_types[i]);
                bool half = (sz == (base_size >> 1));

                assert((sz == base_size) || half);

                unsigned swizzle = mir_pack_swizzle(ins->mask, ins->swizzle[i],
                                ins->src_types[i], reg_mode_for_bitsize(base_size),
                                channeled, &rep_lo, &rep_hi);

                midgard_vector_alu_src pack = {
                        .mod = mir_pack_mod(ins, i, false),
                        .rep_low = rep_lo,
                        .rep_high = rep_hi,
                        .half = half,
                        .swizzle = swizzle
                };

                unsigned p = vector_alu_srco_unsigned(pack);
                
                if (i == 0)
                        alu->src1 = p;
                else
                        alu->src2 = p;
        }
}

static void
mir_pack_swizzle_ldst(midgard_instruction *ins)
{
        /* TODO: non-32-bit, non-vec4 */
        for (unsigned c = 0; c < 4; ++c) {
                unsigned v = ins->swizzle[0][c];

                /* Check vec4 */
                assert(v <= 3);

                ins->load_store.swizzle |= v << (2 * c);
        }

        /* TODO: arg_1/2 */
}

static void
mir_pack_swizzle_tex(midgard_instruction *ins)
{
        for (unsigned i = 0; i < 2; ++i) {
                unsigned packed = 0;

                for (unsigned c = 0; c < 4; ++c) {
                        unsigned v = ins->swizzle[i][c];

                        /* Check vec4 */
                        assert(v <= 3);

                        packed |= v << (2 * c);
                }

                if (i == 0)
                        ins->texture.swizzle = packed;
                else
                        ins->texture.in_reg_swizzle = packed;
        }

        /* TODO: bias component */
}

/* Up to 3 { ALU, LDST } bundles can execute in parallel with a texture op.
 * Given a texture op, lookahead to see how many such bundles we can flag for
 * OoO execution */

static bool
mir_can_run_ooo(midgard_block *block, midgard_bundle *bundle,
                unsigned dependency)
{
        /* Don't read out of bounds */
        if (bundle >= (midgard_bundle *) ((char *) block->bundles.data + block->bundles.size))
                return false;

        /* Texture ops can't execute with other texture ops */
        if (!IS_ALU(bundle->tag) && bundle->tag != TAG_LOAD_STORE_4)
                return false;

        /* Ensure there is no read-after-write dependency */

        for (unsigned i = 0; i < bundle->instruction_count; ++i) {
                midgard_instruction *ins = bundle->instructions[i];

                mir_foreach_src(ins, s) {
                        if (ins->src[s] == dependency)
                                return false;
                }
        }

        /* Otherwise, we're okay */
        return true;
}

static void
mir_pack_tex_ooo(midgard_block *block, midgard_bundle *bundle, midgard_instruction *ins)
{
        unsigned count = 0;

        for (count = 0; count < 3; ++count) {
                if (!mir_can_run_ooo(block, bundle + count + 1, ins->dest))
                        break;
        }

        ins->texture.out_of_order = count;
}

/* Load store masks are 4-bits. Load/store ops pack for that. vec4 is the
 * natural mask width; vec8 is constrained to be in pairs, vec2 is duplicated. TODO: 8-bit?
 */

static void
mir_pack_ldst_mask(midgard_instruction *ins)
{
        unsigned sz = nir_alu_type_get_type_size(ins->dest_type);
        unsigned packed = ins->mask;

        if (sz == 64) {
                packed = ((ins->mask & 0x2) ? (0x8 | 0x4) : 0) |
                         ((ins->mask & 0x1) ? (0x2 | 0x1) : 0);
        } else if (sz == 16) {
                packed = 0;

                for (unsigned i = 0; i < 4; ++i) {
                        /* Make sure we're duplicated */
                        bool u = (ins->mask & (1 << (2*i + 0))) != 0;
                        ASSERTED bool v = (ins->mask & (1 << (2*i + 1))) != 0;
                        assert(u == v);

                        packed |= (u << i);
                }
        } else {
                assert(sz == 32);
        }

        ins->load_store.mask = packed;
}

static void
mir_lower_inverts(midgard_instruction *ins)
{
        bool inv[3] = {
                ins->src_invert[0],
                ins->src_invert[1],
                ins->src_invert[2]
        };

        switch (ins->op) {
        case midgard_alu_op_iand:
                /* a & ~b = iandnot(a, b) */
                /* ~a & ~b = ~(a | b) = inor(a, b) */

                if (inv[0] && inv[1])
                        ins->op = midgard_alu_op_inor;
                else if (inv[1])
                        ins->op = midgard_alu_op_iandnot;

                break;
        case midgard_alu_op_ior:
                /*  a | ~b = iornot(a, b) */
                /* ~a | ~b = ~(a & b) = inand(a, b) */

                if (inv[0] && inv[1])
                        ins->op = midgard_alu_op_inand;
                else if (inv[1])
                        ins->op = midgard_alu_op_iornot;

                break;

        case midgard_alu_op_ixor:
                /* ~a ^ b = a ^ ~b = ~(a ^ b) = inxor(a, b) */
                /* ~a ^ ~b = a ^ b */

                if (inv[0] ^ inv[1])
                        ins->op = midgard_alu_op_inxor;

                break;

        default:
                break;
        }
}

/* Opcodes with ROUNDS are the base (rte/0) type so we can just add */

static void
mir_lower_roundmode(midgard_instruction *ins)
{
        if (alu_opcode_props[ins->op].props & MIDGARD_ROUNDS) {
                assert(ins->roundmode <= 0x3);
                ins->op += ins->roundmode;
        }
}

static midgard_load_store_word
load_store_from_instr(midgard_instruction *ins)
{
        midgard_load_store_word ldst = ins->load_store;
        ldst.op = ins->op;

        if (OP_IS_STORE(ldst.op)) {
                ldst.reg = SSA_REG_FROM_FIXED(ins->src[0]) & 1;
        } else {
                ldst.reg = SSA_REG_FROM_FIXED(ins->dest);
        }

        /* Atomic opcode swizzles have a special meaning:
         *   - The first two bits say which component of the implicit register should be used
         *   - The next two bits say if the implicit register is r26 or r27 */
        if (OP_IS_ATOMIC(ins->op)) {
                ldst.swizzle = 0;
                ldst.swizzle |= ins->swizzle[3][0] & 3;
                ldst.swizzle |= (SSA_REG_FROM_FIXED(ins->src[3]) & 1 ? 1 : 0) << 2;
        }

        if (ins->src[1] != ~0) {
                unsigned src = SSA_REG_FROM_FIXED(ins->src[1]);
                unsigned sz = nir_alu_type_get_type_size(ins->src_types[1]);
                ldst.arg_1 |= midgard_ldst_reg(src, ins->swizzle[1][0], sz);
        }

        if (ins->src[2] != ~0) {
                unsigned src = SSA_REG_FROM_FIXED(ins->src[2]);
                unsigned sz = nir_alu_type_get_type_size(ins->src_types[2]);
                ldst.arg_2 |= midgard_ldst_reg(src, ins->swizzle[2][0], sz);
        }

        return ldst;
}

static midgard_texture_word
texture_word_from_instr(midgard_instruction *ins)
{
        midgard_texture_word tex = ins->texture;
        tex.op = ins->op;

        unsigned src1 = ins->src[1] == ~0 ? REGISTER_UNUSED : SSA_REG_FROM_FIXED(ins->src[1]);
        tex.in_reg_select = src1 & 1;

        unsigned dest = ins->dest == ~0 ? REGISTER_UNUSED : SSA_REG_FROM_FIXED(ins->dest);
        tex.out_reg_select = dest & 1;

        if (ins->src[2] != ~0) {
                midgard_tex_register_select sel = {
                        .select = SSA_REG_FROM_FIXED(ins->src[2]) & 1,
                        .full = 1,
                        .component = ins->swizzle[2][0]
                };
                uint8_t packed;
                memcpy(&packed, &sel, sizeof(packed));
                tex.bias = packed;
        }

        if (ins->src[3] != ~0) {
                unsigned x = ins->swizzle[3][0];
                unsigned y = x + 1;
                unsigned z = x + 2;

                /* Check range, TODO: half-registers */
                assert(z < 4);

                unsigned offset_reg = SSA_REG_FROM_FIXED(ins->src[3]);
                tex.offset =
                        (1)                   | /* full */
                        (offset_reg & 1) << 1 | /* select */
                        (0 << 2)              | /* upper */
                        (x << 3)              | /* swizzle */
                        (y << 5)              | /* swizzle */
                        (z << 7);               /* swizzle */
        }

        return tex;
}

static midgard_vector_alu
vector_alu_from_instr(midgard_instruction *ins)
{
        midgard_vector_alu alu = {
                .op = ins->op,
                .outmod = ins->outmod,
                .reg_mode = reg_mode_for_bitsize(max_bitsize_for_alu(ins))
        };

        if (ins->has_inline_constant) {
                /* Encode inline 16-bit constant. See disassembler for
                 * where the algorithm is from */

                int lower_11 = ins->inline_constant & ((1 << 12) - 1);
                uint16_t imm = ((lower_11 >> 8) & 0x7) |
                               ((lower_11 & 0xFF) << 3);

                alu.src2 = imm << 2;
        }

        return alu;
}

static midgard_branch_extended
midgard_create_branch_extended( midgard_condition cond,
                                midgard_jmp_writeout_op op,
                                unsigned dest_tag,
                                signed quadword_offset)
{
        /* The condition code is actually a LUT describing a function to
         * combine multiple condition codes. However, we only support a single
         * condition code at the moment, so we just duplicate over a bunch of
         * times. */

        uint16_t duplicated_cond =
                (cond << 14) |
                (cond << 12) |
                (cond << 10) |
                (cond << 8) |
                (cond << 6) |
                (cond << 4) |
                (cond << 2) |
                (cond << 0);

        midgard_branch_extended branch = {
                .op = op,
                .dest_tag = dest_tag,
                .offset = quadword_offset,
                .cond = duplicated_cond
        };

        return branch;
}

static void
emit_branch(midgard_instruction *ins,
            compiler_context *ctx,
            midgard_block *block,
            midgard_bundle *bundle,
            struct util_dynarray *emission)
{
        /* Parse some basic branch info */
        bool is_compact = ins->unit == ALU_ENAB_BR_COMPACT;
        bool is_conditional = ins->branch.conditional;
        bool is_inverted = ins->branch.invert_conditional;
        bool is_discard = ins->branch.target_type == TARGET_DISCARD;
        bool is_tilebuf_wait = ins->branch.target_type == TARGET_TILEBUF_WAIT;
        bool is_special = is_discard || is_tilebuf_wait;
        bool is_writeout = ins->writeout;

        /* Determine the block we're jumping to */
        int target_number = ins->branch.target_block;

        /* Report the destination tag */
        int dest_tag = is_discard ? 0 :
                is_tilebuf_wait ? bundle->tag :
                midgard_get_first_tag_from_block(ctx, target_number);

        /* Count up the number of quadwords we're
         * jumping over = number of quadwords until
         * (br_block_idx, target_number) */

        int quadword_offset = 0;

        if (is_discard) {
                /* Fixed encoding, not actually an offset */
                quadword_offset = 0x2;
        } else if (is_tilebuf_wait) {
                quadword_offset = -1;
        } else if (target_number > block->base.name) {
                /* Jump forward */

                for (int idx = block->base.name+1; idx < target_number; ++idx) {
                        midgard_block *blk = mir_get_block(ctx, idx);
                        assert(blk);

                        quadword_offset += blk->quadword_count;
                }
        } else {
                /* Jump backwards */

                for (int idx = block->base.name; idx >= target_number; --idx) {
                        midgard_block *blk = mir_get_block(ctx, idx);
                        assert(blk);

                        quadword_offset -= blk->quadword_count;
                }
        }

        /* Unconditional extended branches (far jumps)
         * have issues, so we always use a conditional
         * branch, setting the condition to always for
         * unconditional. For compact unconditional
         * branches, cond isn't used so it doesn't
         * matter what we pick. */

        midgard_condition cond =
                !is_conditional ? midgard_condition_always :
                is_inverted ? midgard_condition_false :
                midgard_condition_true;

        midgard_jmp_writeout_op op =
                is_discard ? midgard_jmp_writeout_op_discard :
                is_tilebuf_wait ? midgard_jmp_writeout_op_tilebuffer_pending :
                is_writeout ? midgard_jmp_writeout_op_writeout :
                (is_compact && !is_conditional) ?
                midgard_jmp_writeout_op_branch_uncond :
                midgard_jmp_writeout_op_branch_cond;

        if (is_compact) {
                unsigned size = sizeof(midgard_branch_cond);

                if (is_conditional || is_special) {
                        midgard_branch_cond branch = {
                                .op = op,
                                .dest_tag = dest_tag,
                                .offset = quadword_offset,
                                .cond = cond
                        };
                        memcpy(util_dynarray_grow_bytes(emission, size, 1), &branch, size);
                } else {
                        assert(op == midgard_jmp_writeout_op_branch_uncond);
                        midgard_branch_uncond branch = {
                                .op = op,
                                .dest_tag = dest_tag,
                                .offset = quadword_offset,
                                .unknown = 1
                        };
                        assert(branch.offset == quadword_offset);
                        memcpy(util_dynarray_grow_bytes(emission, size, 1), &branch, size);
                }
        } else { /* `ins->compact_branch`,  misnomer */
                unsigned size = sizeof(midgard_branch_extended);

                midgard_branch_extended branch =
                        midgard_create_branch_extended(
                                        cond, op,
                                        dest_tag,
                                        quadword_offset);

                memcpy(util_dynarray_grow_bytes(emission, size, 1), &branch, size);
        }
}

static void
emit_alu_bundle(compiler_context *ctx,
                midgard_block *block,
                midgard_bundle *bundle,
                struct util_dynarray *emission,
                unsigned lookahead)
{
        /* Emit the control word */
        util_dynarray_append(emission, uint32_t, bundle->control | lookahead);

        /* Next up, emit register words */
        for (unsigned i = 0; i < bundle->instruction_count; ++i) {
                midgard_instruction *ins = bundle->instructions[i];

                /* Check if this instruction has registers */
                if (ins->compact_branch) continue;

                unsigned src2_reg = REGISTER_UNUSED;
                if (ins->has_inline_constant)
                        src2_reg = ins->inline_constant >> 11;
                else if (ins->src[1] != ~0)
                        src2_reg = SSA_REG_FROM_FIXED(ins->src[1]);

                /* Otherwise, just emit the registers */
                uint16_t reg_word = 0;
                midgard_reg_info registers = {
                        .src1_reg = (ins->src[0] == ~0 ?
                                        REGISTER_UNUSED :
                                        SSA_REG_FROM_FIXED(ins->src[0])),
                        .src2_reg = src2_reg,
                        .src2_imm = ins->has_inline_constant,
                        .out_reg = (ins->dest == ~0 ?
                                        REGISTER_UNUSED :
                                        SSA_REG_FROM_FIXED(ins->dest)),
                };
                memcpy(&reg_word, &registers, sizeof(uint16_t));
                util_dynarray_append(emission, uint16_t, reg_word);
        }

        /* Now, we emit the body itself */
        for (unsigned i = 0; i < bundle->instruction_count; ++i) {
                midgard_instruction *ins = bundle->instructions[i];

                if (!ins->compact_branch) {
                        mir_lower_inverts(ins);
                        mir_lower_roundmode(ins);
                }

                if (midgard_is_branch_unit(ins->unit)) {
                        emit_branch(ins, ctx, block, bundle, emission);
                } else if (ins->unit & UNITS_ANY_VECTOR) {
                        midgard_vector_alu source = vector_alu_from_instr(ins);
                        mir_pack_mask_alu(ins, &source);
                        mir_pack_vector_srcs(ins, &source);
                        unsigned size = sizeof(source);
                        memcpy(util_dynarray_grow_bytes(emission, size, 1), &source, size);
                } else {
                        midgard_scalar_alu source = vector_to_scalar_alu(vector_alu_from_instr(ins), ins);
                        unsigned size = sizeof(source);
                        memcpy(util_dynarray_grow_bytes(emission, size, 1), &source, size);
                }
        }

        /* Emit padding (all zero) */
        memset(util_dynarray_grow_bytes(emission, bundle->padding, 1), 0, bundle->padding);

        /* Tack on constants */

        if (bundle->has_embedded_constants)
                util_dynarray_append(emission, midgard_constants, bundle->constants);
}

/* Shift applied to the immediate used as an offset. Probably this is papering
 * over some other semantic distinction else well, but it unifies things in the
 * compiler so I don't mind. */

static unsigned
mir_ldst_imm_shift(midgard_load_store_op op)
{
        if (OP_IS_UBO_READ(op))
                return 3;
        else
                return 1;
}

static enum mali_sampler_type
midgard_sampler_type(nir_alu_type t) {
        switch (nir_alu_type_get_base_type(t))
        {
        case nir_type_float:
                return MALI_SAMPLER_FLOAT;
        case nir_type_int:
                return MALI_SAMPLER_SIGNED;
        case nir_type_uint:
                return MALI_SAMPLER_UNSIGNED;
        default:
                unreachable("Unknown sampler type");
        }
}

/* After everything is scheduled, emit whole bundles at a time */

void
emit_binary_bundle(compiler_context *ctx,
                   midgard_block *block,
                   midgard_bundle *bundle,
                   struct util_dynarray *emission,
                   int next_tag)
{
        int lookahead = next_tag << 4;

        switch (bundle->tag) {
        case TAG_ALU_4:
        case TAG_ALU_8:
        case TAG_ALU_12:
        case TAG_ALU_16:
        case TAG_ALU_4 + 4:
        case TAG_ALU_8 + 4:
        case TAG_ALU_12 + 4:
        case TAG_ALU_16 + 4:
                emit_alu_bundle(ctx, block, bundle, emission, lookahead);
                break;

        case TAG_LOAD_STORE_4: {
                /* One or two composing instructions */

                uint64_t current64, next64 = LDST_NOP;

                /* Copy masks */

                for (unsigned i = 0; i < bundle->instruction_count; ++i) {
                        mir_pack_ldst_mask(bundle->instructions[i]);

                        /* Atomic ops don't use this swizzle the same way as other ops */
                        if (!OP_IS_ATOMIC(bundle->instructions[i]->op))
                                mir_pack_swizzle_ldst(bundle->instructions[i]);

                        /* Apply a constant offset */
                        unsigned offset = bundle->instructions[i]->constants.u32[0];

                        if (offset) {
                                unsigned shift = mir_ldst_imm_shift(bundle->instructions[i]->op);
                                unsigned upper_shift = 10 - shift;

                                bundle->instructions[i]->load_store.varying_parameters |= (offset & ((1 << upper_shift) - 1)) << shift;
                                bundle->instructions[i]->load_store.address |= (offset >> upper_shift);
                        }
                }

                midgard_load_store_word ldst0 =
                        load_store_from_instr(bundle->instructions[0]);
                memcpy(&current64, &ldst0, sizeof(current64));

                if (bundle->instruction_count == 2) {
                        midgard_load_store_word ldst1 =
                                load_store_from_instr(bundle->instructions[1]);
                        memcpy(&next64, &ldst1, sizeof(next64));
                }

                midgard_load_store instruction = {
                        .type = bundle->tag,
                        .next_type = next_tag,
                        .word1 = current64,
                        .word2 = next64
                };

                util_dynarray_append(emission, midgard_load_store, instruction);

                break;
        }

        case TAG_TEXTURE_4:
        case TAG_TEXTURE_4_VTX:
        case TAG_TEXTURE_4_BARRIER: {
                /* Texture instructions are easy, since there is no pipelining
                 * nor VLIW to worry about. We may need to set .cont/.last
                 * flags. */

                midgard_instruction *ins = bundle->instructions[0];

                ins->texture.type = bundle->tag;
                ins->texture.next_type = next_tag;

                /* Nothing else to pack for barriers */
                if (ins->op == TEXTURE_OP_BARRIER) {
                        ins->texture.cont = ins->texture.last = 1;
                        ins->texture.op = ins->op;
                        util_dynarray_append(emission, midgard_texture_word, ins->texture);
                        return;
                }

                signed override = mir_upper_override(ins, 32);

                ins->texture.mask = override > 0 ?
                        ins->mask >> override :
                        ins->mask;

                mir_pack_swizzle_tex(ins);

                if (!(ctx->quirks & MIDGARD_NO_OOO))
                        mir_pack_tex_ooo(block, bundle, ins);

                unsigned osz = nir_alu_type_get_type_size(ins->dest_type);
                unsigned isz = nir_alu_type_get_type_size(ins->src_types[1]);

                assert(osz == 32 || osz == 16);
                assert(isz == 32 || isz == 16);

                ins->texture.out_full = (osz == 32);
                ins->texture.out_upper = override > 0;
                ins->texture.in_reg_full = (isz == 32);
                ins->texture.sampler_type = midgard_sampler_type(ins->dest_type);
                ins->texture.outmod = ins->outmod;

                if (mir_op_computes_derivatives(ctx->stage, ins->op)) {
                        ins->texture.cont = !ins->helper_terminate;
                        ins->texture.last = ins->helper_terminate || ins->helper_execute;
                } else {
                        ins->texture.cont = ins->texture.last = 1;
                }

                midgard_texture_word texture = texture_word_from_instr(ins);
                util_dynarray_append(emission, midgard_texture_word, texture);
                break;
        }

        default:
                unreachable("Unknown midgard instruction type\n");
        }
}
