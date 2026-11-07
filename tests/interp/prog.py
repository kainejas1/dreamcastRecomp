#!/usr/bin/env python3
"""Assembles a small SH-4 program that exercises every kind of control flow.

    prog.py OUT.bin EXPECTED.txt

The program stores what it computes in a results buffer. EXPECTED.txt holds
what should be there. harness.c runs the program recompiled and interpreted and
checks both against it and against each other.

Two addresses are hidden from the recompiler's analysis on purpose, by building
them at run time with xor: `mid`, the middle of a function, is the kind of target
it misses in a real game.
"""

import struct
import sys

BASE = 0x8C010000
RESULTS = 0x8C080000


class Asm:
    def __init__(self):
        self.pc = BASE
        self.labels = {}
        self.items = []  # (pc, function of pc giving the 16-bit word)

    def label(self, name):
        if name in self.labels:
            raise SystemExit(f"label {name} twice")
        self.labels[name] = self.pc

    def word(self, value):
        self.items.append((self.pc, value if callable(value) else (lambda pc: value)))
        self.pc += 2

    def align4(self):
        if self.pc & 2:
            self.nop()

    def long(self, value):
        if self.pc & 3:
            raise SystemExit("unaligned .long")
        get = value if callable(value) else (lambda: value)
        self.word(lambda pc: get() & 0xFFFF)
        self.word(lambda pc: get() >> 16 & 0xFFFF)

    def addr(self, name):
        return lambda: self.labels[name]

    def assemble(self):
        return b"".join(struct.pack("<H", f(pc)) for pc, f in self.items)

    # ---- instructions ----

    def nop(self): self.word(0x0009)
    def rts(self): self.word(0x000B)
    def rte(self): self.word(0x002B)
    def sett(self): self.word(0x0018)
    def clrt(self): self.word(0x0008)
    def mov_i(self, i, n): self.word(0xE000 | n << 8 | i & 0xFF)
    def mov(self, m, n): self.word(0x6003 | n << 8 | m << 4)
    def add_i(self, i, n): self.word(0x7000 | n << 8 | i & 0xFF)
    def add(self, m, n): self.word(0x300C | n << 8 | m << 4)
    def xor(self, m, n): self.word(0x200A | n << 8 | m << 4)
    def cmp_hs(self, m, n): self.word(0x3002 | n << 8 | m << 4)
    def tst(self, m, n): self.word(0x2008 | n << 8 | m << 4)
    def dt(self, n): self.word(0x4010 | n << 8)
    def movt(self, n): self.word(0x0029 | n << 8)
    def movl_store(self, m, n): self.word(0x2002 | n << 8 | m << 4)   # mov.l Rm,@Rn
    def movl_load(self, m, n): self.word(0x6002 | n << 8 | m << 4)    # mov.l @Rm,Rn
    def movw_r0(self, m, n): self.word(0x000D | n << 8 | m << 4)      # mov.w @(r0,Rm),Rn
    def sts_pr_push(self, n): self.word(0x4022 | n << 8)              # sts.l pr,@-Rn
    def lds_pr_pop(self, m): self.word(0x4026 | m << 8)               # lds.l @Rm+,pr
    def stc_sr(self, n): self.word(0x0002 | n << 8)
    def ldc_ssr(self, m): self.word(0x403E | m << 8)
    def jmp(self, n): self.word(0x402B | n << 8)
    def jsr(self, n): self.word(0x400B | n << 8)
    def braf(self, n): self.word(0x0023 | n << 8)
    def bsrf(self, n): self.word(0x0003 | n << 8)

    def _b8(self, base, name):
        def f(pc):
            d = self.labels[name] - (pc + 4)
            if d % 2 or not -256 <= d <= 254:
                raise SystemExit(f"branch to {name} out of range")
            return base | d >> 1 & 0xFF
        self.word(f)

    def _b12(self, base, name):
        def f(pc):
            d = self.labels[name] - (pc + 4)
            if d % 2 or not -4096 <= d <= 4094:
                raise SystemExit(f"branch to {name} out of range")
            return base | d >> 1 & 0xFFF
        self.word(f)

    def bt(self, name): self._b8(0x8900, name)
    def bf(self, name): self._b8(0x8B00, name)
    def bts(self, name): self._b8(0x8D00, name)
    def bfs(self, name): self._b8(0x8F00, name)
    def bra(self, name): self._b12(0xA000, name)
    def bsr(self, name): self._b12(0xB000, name)

    def _pcl(self, base, name):
        def f(pc):
            d = self.labels[name] - ((pc & ~3) + 4)
            if d % 4 or not 0 <= d <= 1020:
                raise SystemExit(f"literal {name} out of range")
            return base | d >> 2
        self.word(f)

    def movl_lit(self, name, n): self._pcl(0xD000 | n << 8, name)
    def mova(self, name): self._pcl(0xC700, name)

    def movw_lit(self, name, n):
        def f(pc):
            d = self.labels[name] - (pc + 4)
            if d % 2 or not 0 <= d <= 510:
                raise SystemExit(f"literal {name} out of range")
            return 0x9000 | n << 8 | d >> 1
        self.word(f)


def build():
    a = Asm()
    expected = []

    def put(reg, value):
        a.movl_store(reg, 14)
        a.add_i(4, 14)
        expected.append(value & 0xFFFFFFFF)

    # ---- entry ----
    a.sts_pr_push(15)
    a.movl_lit("lit_results", 14)

    # a counted loop: a backward conditional branch
    a.mov_i(5, 1)
    a.mov_i(0, 2)
    a.label("loop")
    a.add(1, 2)
    a.dt(1)
    a.bf("loop")
    put(2, 15)

    # bt/s reads T before its slot clears it
    a.mov_i(0, 3)
    a.sett()
    a.bts("after_bts")
    a.clrt()
    a.mov_i(99, 3)
    a.label("after_bts")
    put(3, 0)
    a.movt(3)
    put(3, 0)

    # and bf/s before its slot sets it
    a.mov_i(0, 3)
    a.clrt()
    a.bfs("after_bfs_slot")
    a.sett()
    a.mov_i(99, 3)
    a.label("after_bfs_slot")
    put(3, 0)
    a.movt(3)
    put(3, 1)

    # bf/s not taken: the slot runs, and so does what follows
    a.sett()
    a.mov_i(1, 4)
    a.bfs("after_bfs_1")
    a.mov_i(2, 4)
    a.add_i(10, 4)
    a.label("after_bfs_1")
    put(4, 12)

    # bf/s taken: the slot runs, what follows doesn't
    a.clrt()
    a.mov_i(1, 4)
    a.bfs("after_bfs_2")
    a.mov_i(3, 4)
    a.add_i(10, 4)
    a.label("after_bfs_2")
    put(4, 3)

    # bsr with the argument set in the delay slot
    a.bsr("double")
    a.mov_i(21, 4)
    put(0, 42)

    # jsr reads its register before the slot overwrites it
    a.mov_i(4, 4)
    a.movl_lit("lit_double", 1)
    a.jsr(1)
    a.mov_i(0, 1)
    put(0, 8)

    # bsrf is relative to its own address + 4
    a.mov_i(5, 4)
    a.movl_lit("lit_bsrf_offset", 1)
    a.label("at_bsrf")
    a.bsrf(1)
    a.nop()
    put(0, 10)

    # a tail call with jmp: double returns straight to here
    a.mov_i(6, 4)
    a.bsr("tail")
    a.nop()
    put(0, 14)

    # a jump table the analysis recognises, with its range check
    a.mov_i(2, 1)
    a.bsr("switcher")
    a.nop()
    put(0, 120)
    a.mov_i(0, 1)
    a.bsr("switcher")
    a.nop()
    put(0, 100)
    a.mov_i(5, 1)
    a.bsr("switcher")
    a.nop()
    put(0, 7)

    # PC-relative loads, one of them in a delay slot
    a.movl_lit("lit_magic", 5)
    put(5, 0x12345678)
    a.movw_lit("lit_short", 6)
    put(6, 0xFFFF8765)
    a.mova("lit_magic")
    a.movl_load(0, 7)
    put(7, 0x12345678)
    a.bra("after_slot_literal")
    a.movl_lit("lit_magic", 8)
    a.label("after_slot_literal")
    put(8, 0x12345678)

    # a literal the program writes to: a variable, not a constant
    a.mova("lit_variable")
    a.mov_i(77, 1)
    a.movl_store(1, 0)
    a.movl_lit("lit_variable", 9)
    put(9, 77)

    # the same function reached the normal way
    a.bsr("has_mid")
    a.nop()
    put(0, 102)

    # a computed call into the middle of it, which the analysis can't see
    a.movl_lit("lit_mid_a", 2)
    a.movl_lit("lit_mid_b", 3)
    a.xor(3, 2)
    a.mov_i(3, 4)
    a.jsr(2)
    a.nop()
    put(0, 106)

    # a computed tail jump there
    a.mov_i(4, 4)
    a.bsr("tail_to_mid")
    a.nop()
    put(0, 108)

    # computed call and computed jump to a known function
    a.movl_lit("lit_double_a", 2)
    a.movl_lit("lit_double_b", 3)
    a.xor(3, 2)
    a.mov_i(9, 4)
    a.jsr(2)
    a.nop()
    put(0, 18)
    a.mov_i(10, 4)
    a.bsr("computed_tail")
    a.nop()
    put(0, 20)

    # computed braf and bsrf, both relative
    a.mov_i(11, 4)
    a.bsr("computed_braf")
    a.nop()
    put(0, 22)
    a.movl_lit("lit_rel_a", 2)
    a.movl_lit("lit_rel_b", 3)
    a.xor(3, 2)
    a.mov_i(12, 4)
    a.label("at_computed_bsrf")
    a.bsrf(2)
    a.mov_i(0, 2)
    put(0, 24)

    # a register set on the way to a merge point isn't a constant after it:
    # the same jsr has to call whichever function the path chose
    a.mov_i(0, 5)
    a.bsr("pick")
    a.nop()
    put(0, 12)
    a.mov_i(1, 5)
    a.bsr("pick")
    a.nop()
    put(0, 18)

    # and the same for jmp
    a.mov_i(0, 5)
    a.bsr("pick_tail")
    a.nop()
    put(0, 14)
    a.mov_i(1, 5)
    a.bsr("pick_tail")
    a.nop()
    put(0, 21)

    # rte puts SR back from SSR
    a.stc_sr(1)
    a.ldc_ssr(1)
    a.bsr("ret_by_rte")
    a.nop()
    put(0, 55)

    a.lds_pr_pop(15)
    a.rts()
    a.nop()

    a.align4()
    a.label("lit_results")
    a.long(RESULTS)
    a.label("lit_double")
    a.long(a.addr("double"))
    a.label("lit_bsrf_offset")
    a.long(lambda: a.labels["double"] - (a.labels["at_bsrf"] + 4))
    a.label("lit_magic")
    a.long(0x12345678)
    a.label("lit_variable")
    a.long(0)
    a.label("lit_mid_a")
    a.long(lambda: a.labels["mid"] ^ 0x5A5A0000)
    a.label("lit_mid_b")
    a.long(0x5A5A0000)
    a.label("lit_double_a")
    a.long(lambda: a.labels["double"] ^ 0x5A5A0000)
    a.label("lit_double_b")
    a.long(0x5A5A0000)
    a.label("lit_rel_a")
    a.long(lambda: (a.labels["double"] - (a.labels["at_computed_bsrf"] + 4)) ^ 0x5A5A0000)
    a.label("lit_rel_b")
    a.long(0x5A5A0000)
    a.label("lit_short")
    a.word(0x8765)
    a.word(0)

    # ---- functions ----
    a.label("double")
    a.mov(4, 0)
    a.rts()
    a.add(4, 0)

    a.label("triple")
    a.mov(4, 0)
    a.add(4, 0)
    a.rts()
    a.add(4, 0)

    a.label("pick")
    a.sts_pr_push(15)
    a.movl_lit("pick_double", 1)
    a.tst(5, 5)
    a.bt("pick_merge")
    a.movl_lit("pick_triple", 1)
    a.label("pick_merge")
    a.jsr(1)
    a.mov_i(6, 4)
    a.lds_pr_pop(15)
    a.rts()
    a.nop()
    a.align4()
    a.label("pick_double")
    a.long(a.addr("double"))
    a.label("pick_triple")
    a.long(a.addr("triple"))

    a.label("pick_tail")
    a.movl_lit("pt_double", 1)
    a.tst(5, 5)
    a.bt("pick_tail_merge")
    a.movl_lit("pt_triple", 1)
    a.label("pick_tail_merge")
    a.jmp(1)
    a.mov_i(7, 4)
    a.align4()
    a.label("pt_double")
    a.long(a.addr("double"))
    a.label("pt_triple")
    a.long(a.addr("triple"))

    a.label("tail")
    a.movl_lit("tail_double", 1)
    a.add_i(1, 4)
    a.jmp(1)
    a.mov_i(0, 1)   # jmp read r1 before this
    a.align4()
    a.label("tail_double")
    a.long(a.addr("double"))

    # sets up its argument, then falls into `mid`, which calls a known function
    a.label("has_mid")
    a.mov_i(1, 4)
    a.label("mid")
    a.sts_pr_push(15)
    a.bsr("double")
    a.nop()
    a.add_i(100, 0)
    a.lds_pr_pop(15)
    a.rts()
    a.nop()

    a.label("tail_to_mid")
    a.movl_lit("tail_mid_a", 2)
    a.movl_lit("tail_mid_b", 3)
    a.xor(3, 2)
    a.jmp(2)
    a.mov_i(0, 2)
    a.align4()
    a.label("tail_mid_a")
    a.long(lambda: a.labels["mid"] ^ 0x5A5A0000)
    a.label("tail_mid_b")
    a.long(0x5A5A0000)

    a.label("computed_tail")
    a.movl_lit("ct_a", 2)
    a.movl_lit("ct_b", 3)
    a.xor(3, 2)
    a.jmp(2)
    a.mov_i(0, 2)
    a.align4()
    a.label("ct_a")
    a.long(lambda: a.labels["double"] ^ 0x5A5A0000)
    a.label("ct_b")
    a.long(0x5A5A0000)

    a.label("computed_braf")
    a.movl_lit("cb_a", 2)
    a.movl_lit("cb_b", 3)
    a.xor(3, 2)
    a.label("at_computed_braf")
    a.braf(2)
    a.mov_i(0, 2)
    a.align4()
    a.label("cb_a")
    a.long(lambda: (a.labels["double"] - (a.labels["at_computed_braf"] + 4)) ^ 0x5A5A0000)
    a.label("cb_b")
    a.long(0x5A5A0000)

    a.label("switcher")
    a.mov_i(3, 2)
    a.cmp_hs(2, 1)
    a.bt("case_default")
    a.add(1, 1)
    a.mova("switch_table")
    a.movw_r0(1, 0)
    a.label("at_switch")
    a.braf(0)
    a.nop()
    a.align4()
    a.label("switch_table")
    for case in ("case_0", "case_1", "case_2"):
        a.word(lambda pc, case=case: a.labels[case] - (a.labels["at_switch"] + 4) & 0xFFFF)
    a.word(0)
    a.label("case_0")
    a.rts()
    a.mov_i(100, 0)
    a.label("case_1")
    a.rts()
    a.mov_i(110, 0)
    a.label("case_2")
    a.rts()
    a.mov_i(120, 0)
    a.label("case_default")
    a.rts()
    a.mov_i(7, 0)

    a.label("ret_by_rte")
    a.mov_i(55, 0)
    a.rte()
    a.nop()

    return a.assemble(), expected


def main():
    code, expected = build()
    with open(sys.argv[1], "wb") as f:
        f.write(code)
    with open(sys.argv[2], "w") as f:
        f.writelines(f"{v:08x}\n" for v in expected)


if __name__ == "__main__":
    main()
