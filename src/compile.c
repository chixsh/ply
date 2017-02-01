/*
 * Copyright 2015-2016 Tobias Waldekranz <tobias@waldekranz.com>
 *
 * This file is part of ply.
 *
 * ply is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation, under the terms of version 2 of the
 * License.
 *
 * ply is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
 * License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with ply.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <string.h>

#include <ply/ast.h>
#include <ply/bpf-syscall.h>
#include <ply/compile.h>
#include <ply/ply.h>
#include <ply/pvdr.h>
#include <ply/symtable.h>

const dyn_t dyn_reg[] = {
	[BPF_REG_0] =  { .type = TYPE_INT, .size = 8, .loc = LOC_REG, .reg = BPF_REG_0  },
	[BPF_REG_1] =  { .type = TYPE_INT, .size = 8, .loc = LOC_REG, .reg = BPF_REG_1  },
	[BPF_REG_2] =  { .type = TYPE_INT, .size = 8, .loc = LOC_REG, .reg = BPF_REG_2  },
	[BPF_REG_3] =  { .type = TYPE_INT, .size = 8, .loc = LOC_REG, .reg = BPF_REG_3  },
	[BPF_REG_4] =  { .type = TYPE_INT, .size = 8, .loc = LOC_REG, .reg = BPF_REG_4  },
	[BPF_REG_5] =  { .type = TYPE_INT, .size = 8, .loc = LOC_REG, .reg = BPF_REG_5  },
	[BPF_REG_6] =  { .type = TYPE_INT, .size = 8, .loc = LOC_REG, .reg = BPF_REG_6  },
	[BPF_REG_7] =  { .type = TYPE_INT, .size = 8, .loc = LOC_REG, .reg = BPF_REG_7  },
	[BPF_REG_8] =  { .type = TYPE_INT, .size = 8, .loc = LOC_REG, .reg = BPF_REG_8  },
	[BPF_REG_9] =  { .type = TYPE_INT, .size = 8, .loc = LOC_REG, .reg = BPF_REG_9  },
	[BPF_REG_10] = { .type = TYPE_INT, .size = 8, .loc = LOC_REG, .reg = BPF_REG_10 },
};

static const char *bpf_func_name(enum bpf_func_id id)
{
	switch (id) {
	case BPF_FUNC_get_current_comm:
		return "get_current_comm";
	case BPF_FUNC_get_current_pid_tgid:
		return "get_current_pid_tgid";
	case BPF_FUNC_get_current_uid_gid:
		return "get_current_uid_gid";
#ifdef LINUX_HAS_STACKMAP
	case BPF_FUNC_get_stackid:
		return "get_stackid";
#endif
	case BPF_FUNC_ktime_get_ns:
		return "ktime_get_ns";
	case BPF_FUNC_map_delete_elem:
		return "map_delete_elem";
	case BPF_FUNC_map_lookup_elem:
		return "map_lookup_elem";
	case BPF_FUNC_map_update_elem:
		return "map_update_elem";
	case BPF_FUNC_perf_event_output:
		return "perf_event_output";
	case BPF_FUNC_probe_read:
		return "probe_read";
	case BPF_FUNC_trace_printk:
		return "trace_printk";
	default:
		return NULL;
	}
}

void dump_reg(uint8_t reg, int16_t off)
{
	if (off < 0)
		fprintf(stderr, "[r%u - 0x%x]", reg, -off);
	else if (off > 0)
		fprintf(stderr, "[r%u + 0x%x]", reg, off);
	else
		fprintf(stderr, "r%u", reg);
}

void dump_size(uint8_t size)
{
	switch (BPF_SIZE(size)) {
	case BPF_B:
		fputs("b\t", stderr);
		break;
	case BPF_H:
		fputs("h\t", stderr);
		break;
	case BPF_W:
		fputs("w\t", stderr);
		break;
	case BPF_DW:
		fputs("dw\t", stderr);
		break;
	}
}		

void dump_insn(struct bpf_insn insn, size_t ip)
{
	const char *name;
	enum {
		OFF_NONE,
		OFF_DST,
		OFF_SRC,
		OFF_EXP,
	} off = OFF_NONE;
	

	fprintf(stderr, "%3zu:\t", ip);

	switch (BPF_CLASS(insn.code)) {
	case BPF_LD:
	case BPF_LDX:
		off = OFF_SRC;
		fputs("ld", stderr);
		dump_size(insn.code);
		break;

	case BPF_ST:
	case BPF_STX:
		off = OFF_DST;
		fputs("st", stderr);
		dump_size(insn.code);
		break;

	case BPF_ALU64:
		switch (BPF_OP(insn.code)) {
		case BPF_MOV: fputs("mov\t", stderr); break;
		case BPF_ADD: fputs("add\t", stderr); break;
		case BPF_SUB: fputs("sub\t", stderr); break;
		case BPF_MUL: fputs("mul\t", stderr); break;
		case BPF_DIV: fputs("div\t", stderr); break;
		case BPF_OR : fputs("or\t",  stderr); break;
		case BPF_AND: fputs("and\t", stderr); break;
		case BPF_LSH: fputs("lsh\t", stderr); break;
		case BPF_RSH: fputs("rsh\t", stderr); break;
		case BPF_NEG: fputs("neg\t", stderr); break;
		case BPF_MOD: fputs("mod\t", stderr); break;
		case BPF_XOR: fputs("xor\t", stderr); break;
		}
		break;

	case BPF_JMP:
		off = OFF_EXP;

		switch (BPF_OP(insn.code)) {
		case BPF_EXIT:
			fputs("exit\n", stderr);
			return;
		case BPF_CALL:
			fputs("call\t", stderr);

			name = bpf_func_name(insn.imm);
			if (name)
				fprintf(stderr, "%s\n", name);
			else
				fprintf(stderr, "%d\n", insn.imm);
			return;
		case BPF_JA:
			fprintf(stderr, "ja\t%+d\n", insn.off);
			return;

		case BPF_JEQ:  fputs("jeq\t", stderr); break;
		case BPF_JNE:  fputs("jne\t", stderr); break;
		case BPF_JGT:  fputs("jgt\t", stderr); break;
		case BPF_JGE:  fputs("jge\t", stderr); break;
		case BPF_JSGE: fputs("jsge\t", stderr); break;
		case BPF_JSGT: fputs("jsgt\t", stderr); break;
		default:
			goto unknown;
		}
		break;

	default:
		goto unknown;
	}

	dump_reg(insn.dst_reg, off == OFF_DST ? insn.off : 0);		
	fputs(", ", stderr);

	if (BPF_CLASS(insn.code) == BPF_LDX || BPF_CLASS(insn.code) == BPF_STX)
		goto reg_src;

	switch (BPF_SRC(insn.code)) {
	case BPF_K:
		fprintf(stderr, "#%s0x%x", insn.imm < 0 ? "-" : "",
			insn.imm < 0 ? -insn.imm : insn.imm);
		break;
	case BPF_X:
	reg_src:
		dump_reg(insn.src_reg, off == OFF_SRC ? insn.off : 0);		
		break;
	}

	if (off == OFF_EXP) {
		fputs(", ", stderr);
		fprintf(stderr, "%+d", insn.off);
	}

	fputc('\n', stderr);
	return;

unknown:
	fprintf(stderr, "data\t0x%16.16" PRIx64 "\n", *((uint64_t *)&insn));
}

void emit(prog_t *prog, struct bpf_insn insn)
{
	if (G.dump)
		dump_insn(insn, prog->ip - prog->insns);

	*(prog->ip)++ = insn;
}

int emit_stack_zero(prog_t *prog, const node_t *n)
{
	size_t i;

	emit(prog, MOV_IMM(BPF_REG_0, 0));
	for (i = 0; i < n->dyn->size; i += sizeof(int64_t))
		emit(prog, STXDW(BPF_REG_10, n->dyn->addr + i, BPF_REG_0));

	return 0;
}

static int emit_xfer_literal(prog_t *prog, const dyn_t *to,
			     const void *_from, size_t size)
{
	const uint64_t *u64 = _from;
	const int32_t *s32 = _from;
	ssize_t at;

	switch (to->loc) {
	case LOC_NOWHERE:
	case LOC_VIRTUAL:
		_e("destination unknown");
		return -EINVAL;

	case LOC_REG:
		if (*u64 > 0x3fffffffffffffff) {
			emit(prog, MOV_IMM(to->reg, *u64 >> 33));
			emit(prog, ALU_IMM(ALU_OP_LSH, to->reg, 31));
			emit(prog, ALU_IMM(ALU_OP_OR, to->reg, (*u64 >> 2) & 0x7fffffff));
			emit(prog, ALU_IMM(ALU_OP_LSH, to->reg, 2));
			emit(prog, ALU_IMM(ALU_OP_OR, to->reg, *u64 & 0x3));
		} else if (*u64 > 0x7fffffff) {
			emit(prog, MOV_IMM(to->reg, *u64 >> 31));
			emit(prog, ALU_IMM(ALU_OP_LSH, to->reg, 31));
			emit(prog, ALU_IMM(ALU_OP_OR, to->reg, *u64 & 0x7fffffff));
		} else {
			emit(prog, MOV_IMM(to->reg, *u64));
		}
		return 0;

	case LOC_STACK:
		for (at = to->addr; size;
		     at += sizeof(*s32), size -= sizeof(*s32), s32++)
			emit(prog, STW_IMM(BPF_REG_10, at, *s32));
		return 0;
	}

	return -EINVAL;
}

static int emit_xfer_reg(prog_t *prog, const dyn_t *to, int from)
{
	switch (to->loc) {
	case LOC_NOWHERE:
	case LOC_VIRTUAL:
		_e("destination unknown");
		return -EINVAL;

	case LOC_REG:
		if (to->reg == from)
			return 0;

		emit(prog, MOV(to->reg, from));
		return 0;

	case LOC_STACK:
		emit(prog, STXDW(BPF_REG_10, to->addr, from));
		return 0;
	}

	return -EINVAL;
}

static int emit_xfer_stack(prog_t *prog, const dyn_t *to, ssize_t from)
{
	switch (to->loc) {
	case LOC_NOWHERE:
	case LOC_VIRTUAL:
		_e("destination unknown");
		return -EINVAL;

	case LOC_REG:
		emit(prog, LDXDW(to->reg, from, BPF_REG_10));
		return 0;

	case LOC_STACK:
		_e("stack<->stack transfer not implemented");
		return -ENOSYS;
	}

	return -EINVAL;
}

int emit_xfer_dyns(prog_t *prog, const dyn_t *to, const dyn_t *from)
{
	switch (from->loc) {
	case LOC_NOWHERE:
	case LOC_VIRTUAL:
		_e("source unknown");
		return -EINVAL;

	case LOC_REG:
		return emit_xfer_reg(prog, to, from->reg);

	case LOC_STACK:
		return emit_xfer_stack(prog, to, from->addr);
	}

	return -EINVAL;

}

int emit_xfer_dyn(prog_t *prog, const dyn_t *to, const node_t *from)
{
	switch (from->type) {
	case TYPE_INT:
		return emit_xfer_literal(prog, to, (void *)&from->integer,
					 sizeof(from->integer));
	case TYPE_STR:
		return emit_xfer_literal(prog, to, (void *)from->string,
					 from->dyn->size);
	default:
		break;
	}

	return emit_xfer_dyns(prog, to, from->dyn);	
}

int emit_xfer(prog_t *prog, const node_t *to, const node_t *from)
{
	return emit_xfer_dyn(prog, to->dyn, from);
}

#define LOG2_CMP(_bit)						\
	emit(prog, JMP_IMM(JMP_JSGE, src, (1 << (_bit)), 1));	\
	emit(prog, JMP_IMM(JMP_JA, 0, 0, 2));			\
	emit(prog, ALU_IMM(ALU_OP_ADD, dst, _bit));		\
	emit(prog, ALU_IMM(ALU_OP_RSH, src, _bit))

int emit_log2_raw(prog_t *prog, int dst, int src)
{
	int cmp = BPF_REG_5;

	emit(prog, MOV_IMM(dst, 0));

	/* negative? */
	emit(prog, JMP_IMM(JMP_JSGE, src, 0, 2));
	emit(prog, ALU_IMM(ALU_OP_SUB, dst, 1));
	emit(prog, JMP_IMM(JMP_JA, 0, 0, 8 + 5 * 4));

	/* zero? */
	emit(prog, JMP_IMM(JMP_JEQ, src, 0, 7 + 5 * 4));

	emit(prog, ALU_IMM(ALU_OP_ADD, dst, 1));

	emit(prog, MOV_IMM(cmp, 1));
	emit(prog, ALU_IMM(ALU_OP_LSH, cmp, 32));

	emit(prog, JMP(JMP_JSGE, src, cmp, 1));
	emit(prog, JMP_IMM(JMP_JA, 0, 0, 2));
	emit(prog, ALU_IMM(ALU_OP_ADD, dst, 32));
	emit(prog, ALU_IMM(ALU_OP_RSH, src, 32));

	LOG2_CMP(16);
	LOG2_CMP( 8);
	LOG2_CMP( 4);
	LOG2_CMP( 2);
	LOG2_CMP( 1);
	return 0;
}

int emit_read_raw(prog_t *prog, ssize_t to, int from, size_t size)
{
	emit(prog, MOV(BPF_REG_1, BPF_REG_10));
	emit(prog, ALU_IMM(ALU_OP_ADD, BPF_REG_1, to));
	emit(prog, MOV_IMM(BPF_REG_2, size));
	emit(prog, MOV(BPF_REG_3, from));
	emit(prog, CALL(BPF_FUNC_probe_read));
	return 0;
}

int emit_map_update_raw(prog_t *prog, int fd, ssize_t key, ssize_t val)
{
	emit_ld_mapfd(prog, BPF_REG_1, fd);
	emit(prog, MOV(BPF_REG_2, BPF_REG_10));
	emit(prog, ALU_IMM(ALU_OP_ADD, BPF_REG_2, key));
	emit(prog, MOV(BPF_REG_3, BPF_REG_10));
	emit(prog, ALU_IMM(ALU_OP_ADD, BPF_REG_3, val));
	emit(prog, MOV_IMM(BPF_REG_4, 0));
	emit(prog, CALL(BPF_FUNC_map_update_elem));
	return 0;
}

int emit_map_delete_raw(prog_t *prog, int fd, ssize_t key)
{
	emit_ld_mapfd(prog, BPF_REG_1, fd);
	emit(prog, MOV(BPF_REG_2, BPF_REG_10));
	emit(prog, ALU_IMM(ALU_OP_ADD, BPF_REG_2, key));
	emit(prog, CALL(BPF_FUNC_map_delete_elem));
	return 0;
}

int emit_map_lookup_raw(prog_t *prog, int fd, ssize_t addr)
{
	emit_ld_mapfd(prog, BPF_REG_1, fd);
	emit(prog, MOV(BPF_REG_2, BPF_REG_10));
	emit(prog, ALU_IMM(ALU_OP_ADD, BPF_REG_2, addr));
	emit(prog, CALL(BPF_FUNC_map_lookup_elem));
	return 0;
}

int emit_rec_load(prog_t *prog, node_t *n)
{
	node_t *c;
	dyn_t to = { .loc = LOC_STACK };

	to.addr = n->dyn->addr;
	node_foreach(c, n->rec.vargs) {
		if (c->type == TYPE_VAR) {
			to.size = c->dyn->size;
			emit_xfer_dyn(prog, &to, c);
		}

		to.addr += c->dyn->size;
	}

	return 0;
}

int emit_map_load(prog_t *prog, node_t *n)
{
	/* when overriding the current value, there is no need to load
	 * any previous value */
	if (n->parent->type == TYPE_ASSIGN &&
	    n->parent->assign.lval == n)
		return 0;

	emit_stack_zero(prog, n);

	emit_map_lookup_raw(prog, n->dyn->map.fd, n->map.rec->dyn->addr);

	/* if we get a null pointer, skip copy */
	emit(prog, JMP_IMM(JMP_JEQ, BPF_REG_0, 0, 5));

	/* if key existed, copy it to the value area */
	emit_read_raw(prog, n->dyn->addr, BPF_REG_0, n->dyn->size);

	if (n->dyn->loc == LOC_REG)
		emit_xfer_stack(prog, n->dyn, n->dyn->addr);

	return 0;
}

/* int emit_var_load(prog_t *prog, node_t *n) */
/* { */
/* 	node_t *script = node_get_script(n); */
/* 	sym_t *s; */

/* 	s = symtable_get(script->dyn->script.st, n); */
/* 	assert(s); */

/* 	/\* this is not the first reference, value is loaded already *\/ */
/* 	if (n != s->var.first) */
/* 		return 0; */
	
/* 	/\* when overriding the current value, there is no need to load */
/* 	 * any previous value *\/ */
/* 	if (n->parent->type == TYPE_ASSIGN && */
/* 	    n->parent->assign.lval == n) */
/* 		return 0; */

/* 	switch (n->dyn->loc) { */
/* 	case LOC_STACK: */
/* 		emit_stack_zero(prog, n); */
/* 		break; */
/* 	case LOC_REG: */
/* 		emit(prog, MOV_IMM(n->dyn->reg, 0)); */
/* 		break; */
/* 	default: */
/* 		assert(0); */
/* 		break; */
/* 	} */

/* 	return 0; */
/* } */

int emit_not(prog_t *prog, node_t *not)
{
	node_t *expr = not->not;
	int src = expr->dyn->loc == LOC_REG ? expr->dyn->reg : BPF_REG_0;
	int err;
	
	err = emit_xfer_dyns(prog, &dyn_reg[src], expr->dyn);
	if (err)
		return err;

	emit(prog, JMP_IMM(JMP_JNE, src, 0, 2));
	emit(prog, MOV_IMM(src, 1));
	emit(prog, JMP_IMM(JMP_JA, 0, 0, 1));
	emit(prog, MOV_IMM(src, 0));

	return emit_xfer_dyns(prog, not->dyn, &dyn_reg[src]);
}

int emit_binop_alu(prog_t *prog, node_t *binop, int imm)
{
	node_t *l = binop->binop.left, *r = binop->binop.right;

	if (imm)
		emit(prog, ALU_IMM(binop->binop.alu, l->dyn->reg, r->integer));
	else
		emit(prog, ALU(binop->binop.alu, l->dyn->reg, r->dyn->reg));
	return 0;
}

int emit_binop_jmp(prog_t *prog, node_t *binop, int imm)
{
	node_t *l = binop->binop.left, *r = binop->binop.right;

	if (imm)
		emit(prog, JMP_IMM(binop->binop.jmp, l->dyn->reg, r->integer, 2));
	else
		emit(prog, JMP(binop->binop.jmp, l->dyn->reg, r->dyn->reg, 2));

	emit(prog, MOV_IMM(l->dyn->reg, 0));
	emit(prog, JMP_IMM(JMP_JA, 0, 0, 1));
	emit(prog, MOV_IMM(l->dyn->reg, 1));
	return 0;
}

int emit_binop(prog_t *prog, node_t *binop)
{
	node_t *l = binop->binop.left, *r = binop->binop.right;
	int imm = 0;

	if (l->dyn->loc == LOC_STACK)
		l->dyn->reg = BPF_REG_0;

	if (l->type == TYPE_INT || l->dyn->loc != LOC_REG)
		emit_xfer_dyn(prog, &dyn_reg[l->dyn->reg], l);

	if (r->dyn->loc == LOC_STACK)
		r->dyn->reg = BPF_REG_1;

	if (r->type == TYPE_INT || r->dyn->loc != LOC_REG) {
		if (r->type == TYPE_INT &&
		    r->integer >= INT32_MIN &&
		    r->integer <= INT32_MAX)
			imm = 1;
		else
			emit_xfer_dyn(prog, &dyn_reg[r->dyn->reg], r);
	}

	switch (binop->binop.type) {
	case BINOP_ALU:
		emit_binop_alu(prog, binop, imm);
		break;
	case BINOP_JMP:
		emit_binop_jmp(prog, binop, imm);
		break;
	}

	return emit_xfer_dyns(prog, binop->dyn, &dyn_reg[l->dyn->reg]);
}

int emit_assign(prog_t *prog, node_t *assign)
{
	node_t *lval = assign->assign.lval, *expr = assign->assign.expr;
	int err;

	if (lval->type == TYPE_MAP && !expr) {
		emit_map_delete_raw(prog, lval->dyn->map.fd,
				    lval->map.rec->dyn->addr);
		return 0;
	}
	
	if (expr->type == TYPE_INT) {
		err = emit_xfer(prog, lval, expr);
		if (err)
			return err;
	}

	if (lval->type == TYPE_MAP)
		emit_map_update_raw(prog, lval->dyn->map.fd,
				    lval->map.rec->dyn->addr, lval->dyn->addr);
	return 0;
}

int emit_method(prog_t *prog, node_t *method)
{
	node_t *map = method->method.map;

	emit_map_update_raw(prog, map->dyn->map.fd,
			    map->map.rec->dyn->addr, map->dyn->addr);
	return 0;
}

int emit_unroll(prog_t *prog, node_t *n)
{
	struct bpf_insn *start = n->dyn->unroll.start;
	ptrdiff_t insns = prog->ip - start;
	int i, j;

	for (i = 1; i < n->unroll.count; i++) {
		_D("%d/%"PRId64, i, n->unroll.count - 1);
		for (j = 0; j < insns; j++)
			emit(prog, *start++);
	}

	return 0;
}

static int compile_pre(node_t *n, void *_prog)
{
	prog_t *prog = _prog;

	switch (n->type) {
	case TYPE_UNROLL:
		n->dyn->unroll.start = prog->ip;
		break;
	default:
		break;
	}
	return 0;
}

static int compile_post(node_t *n, void *_prog)
{
	prog_t *prog = _prog;
	int err = 0;

	(void)(prog);

	if (n->dyn->loc == LOC_VIRTUAL)
		return 0;

	_D("> %s%s%s (%s/%s/%#zx)", n->string ? "" : "<",
	   n->string ? : type_str(n->type), n->string ? "" : ">",
	   type_str(n->type), type_str(n->dyn->type), n->dyn->size);

	switch (n->type) {
	case TYPE_INT:
		if (n->dyn->loc != LOC_STACK)
			break;
		/* fall-through */
	case TYPE_STR:
		emit_xfer(prog, n, n);
		break;

	case TYPE_REC:
		err = emit_rec_load(prog, n);
		break;

	case TYPE_VAR:
		/* err = emit_var_load(prog, n); */
		break;

	case TYPE_MAP:
		err = emit_map_load(prog, n);
		break;

	case TYPE_NOT:
		err = emit_not(prog, n);
		break;

	case TYPE_RETURN:
		/* TODO */
		break;
	case TYPE_BINOP:
		err = emit_binop(prog, n);
		break;

	case TYPE_ASSIGN:
		err = emit_assign(prog, n);
		break;

	case TYPE_METHOD:
		err = emit_method(prog, n);
		break;

	case TYPE_CALL:
		err = n->dyn->call.func->compile(n, prog);
		break;

	case TYPE_UNROLL:
		err = emit_unroll(prog, n);
		break;

	case TYPE_PROBE:
	case TYPE_SCRIPT:
	case TYPE_NONE:
		_e("unable to compile %s <%s>", n->string, type_str(n->type));
		err = -ENOSYS;
		break;
	}

	_D("< %s%s%s (%s/%s/%#zx)", n->string ? "" : "<",
	   n->string ? : type_str(n->type), n->string ? "" : ">",
	   type_str(n->type), type_str(n->dyn->type), n->dyn->size);

	return err;
}

static int compile_walk(node_t *n, prog_t *prog)
{
	return node_walk(n, compile_pre, compile_post, prog);
}

static int compile_pred(node_t *pred, prog_t *prog)
{
	int err;

	if (!pred)
		return 0;

	_D(">");

	err = compile_walk(pred, prog);
	if (err)
		return err;

	switch (pred->dyn->loc) {
	case LOC_REG:
		emit(prog, JMP_IMM(JMP_JNE, pred->dyn->reg, 0, 2));
		break;

	default:
		_e("predicate %s was not in a register as expected", node_str(pred));
		return -EINVAL;
	}

	emit(prog, MOV_IMM(BPF_REG_0, 0));
	emit(prog, EXIT);
	_D("<");
	return 0;
}

prog_t *compile_probe(node_t *probe)
{
	prog_t *prog;
	node_t *stmt;
	int err;

	prog = calloc(1, sizeof(*prog));
	if (!prog)
		return NULL;

	prog->ip = prog->insns;

	_d("");

	/* context (pt_regs) pointer is supplied in r1 */
	emit(prog, MOV(BPF_REG_9, BPF_REG_1));

	err = compile_pred(probe->probe.pred, prog);
	if (err)
		goto err_free;

	node_foreach(stmt, probe->probe.stmts) {
		err = compile_walk(stmt, prog);
		if (err)
			goto err_free;

		if (!stmt->next)
			break;
	}
	
	if (stmt->type == TYPE_RETURN)
		return prog;

	emit(prog, MOV_IMM(BPF_REG_0, 0));
	emit(prog, EXIT);
	return prog;

err_free:
	free(prog);
	return NULL;
}
