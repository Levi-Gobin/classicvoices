import sys, collections
sys.path.insert(0, sys.argv[1])
import vocalizer6 as v
from unicorn import UC_HOOK_BLOCK
from unicorn.arm_const import UC_ARM_REG_CPSR
import capstone
blocks = collections.Counter()
voc = v.Vocalizer("en-US")
e = voc.e
def hb(uc, addr, size, ud):
    thumb = (uc.reg_read(UC_ARM_REG_CPSR) >> 5) & 1
    blocks[(addr, size, thumb)] += 1
e.uc.hook_add(UC_HOOK_BLOCK, hb)
pcm = voc.speak("VoiceOver on. Settings. Button. Double tap to open. Messages, 3 new items.")
secs = len(pcm)/2/22050
md_t = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB)
md_a = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
mn = collections.Counter(); total = 0; arm_instrs = 0; traps = 0
for (addr, size, thumb), cnt in blocks.items():
    if addr >= v.TRAP_BASE: traps += cnt; continue
    code = bytes(e.uc.mem_read(addr, size))
    for ins in (md_t if thumb else md_a).disasm(code, addr):
        mn[ins.mnemonic.split('.')[0]] += cnt; total += cnt
        if not thumb: arm_instrs += cnt
print(f"audio {secs:.2f}s, instructions {total/1e6:.1f}M => {total/secs/1e6:.1f} M instr per audio second")
print(f"ARM-mode instrs {arm_instrs}, trap calls {traps}, unique blocks {len(blocks)}")
print(f"ARM-mode block addrs: {sorted(set(hex(a) for (a,s,t) in blocks if not t and a < v.TRAP_BASE))[:20]}")
print(len(mn), "mnemonics:")
print(' '.join(f"{k}:{c}" for k, c in mn.most_common()))
