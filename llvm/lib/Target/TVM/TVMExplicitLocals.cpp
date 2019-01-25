//===-- TVMExplicitLocals.cpp - Make Locals Explicit ----------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file converts any remaining registers into TVM locals.
///
/// After register stackification and register coloring, convert non-stackified
/// registers into locals, inserting explicit get_local and set_local
/// instructions.
///
//===----------------------------------------------------------------------===//

#include <deque>

#include "MCTargetDesc/TVMMCTargetDesc.h"
#include "TVM.h"
#include "TVMExtras.h"
#include "TVMMachineFunctionInfo.h"
#include "TVMSubtarget.h"
#include "TVMUtilities.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/MachineBlockFrequencyInfo.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include "TVMInstMappingInfo.inc"

using namespace llvm;

#define DEBUG_TYPE "tvm-explicit-locals"

namespace {

enum class StackReorderingKind { Copy, Xchg };

struct StackReordering {
  /// The register we get data from.
  unsigned RegFrom;
  /// The number of slot we put data to.
  size_t SlotTo;
  /// If we copy (Push) or move (Xchg) the data.
  StackReorderingKind ReorderingKind;
  /// Checks if it is Copy.
  bool isCopy() const { return ReorderingKind == StackReorderingKind::Copy; }
  /// Checks if it is Xchg.
  bool isXchg() const { return ReorderingKind == StackReorderingKind::Xchg; }
  StackReordering(unsigned RegFrom, size_t SlotTo,
                  StackReorderingKind ReorderingKind)
      : RegFrom(RegFrom), SlotTo(SlotTo), ReorderingKind(ReorderingKind) {}
};

// Most of the instructions have at most 2 arguments.
using StackReorderings = SmallVector<StackReordering, 2>;

/// Implement the programming model of the hardware stack and keep it in sync
/// with the emitted code.
/// Provide interfaces to track positions of local variables and mutate the
/// stack.
class Stack {
public:
  Stack(const TargetInstrInfo *TII, size_t Size)
      : TII(TII), Data(Size, TVMFunctionInfo::UnusedReg) {}
  /// Insert POP instructions to clean up the stack, preserving the specified
  /// element of it.
  /// \par InsertPoint specify instruction to insert after.
  /// \par Preserved virtual register needs to be kept in the stack.
  bool clear(MachineInstr *InsertPoint,
             unsigned Preserved = TVMFunctionInfo::UnusedReg);
  /// PUSH the specified slot to the specified position of the stack.
  /// Do nothing if \p Reg is already in \p Slot.
  /// Precondition: Register is present in Data.
  /// TODO: Stack PUSH limitations aren't handled yet.
  /// \par InsertPoint specify instruction to insert after.
  /// \par Reg virtual register number for the data source.
  bool push(MachineInstr *InsertPoint, unsigned Reg);
  /// \par InsertPoint specify instruction to insert after.
  /// \par RegFrom register number to be exchanged in the stack.
  /// \par SlotTo slot number to be exchanged with.
  /// Precondition: Slot number for RegFrom != SlotTo.
  bool xchg(MachineInstr *InsertPoint, unsigned RegFrom, size_t SlotTo);
  /// A helper function for general xchg()
  bool xchg(MachineInstr *InsertPoint, const StackReordering &Reordering) {
    return xchg(InsertPoint, Reordering.RegFrom, Reordering.SlotTo);
  }
  /// Return position of \par Reg in the stack.
  /// Precondition: \par Reg is in the stack.
  size_t position(unsigned Reg) const {
    return std::distance(std::begin(Data), llvm::find_or_fail(Data, Reg));
  }
  /// Return register for \par Slot in the stack.
  /// Precondition: Slot < Data.size().
  unsigned reg(size_t Slot) const {
    assert(Slot < Data.size() && "Out of range access");
    return Data[Slot];
  }
  /// Fill the specified \p Slot with \p Reg. Doesn't generate any instruction.
  void set(size_t Slot, size_t Reg) {
    assert(Slot < Data.size() && "Out of range access");
    Data[Slot] = Reg;
  }
  /// Remove arguments an instruction consumed from the stack.
  /// Precondition: Stack has enough Slots to consume.
  void consumeArguments(size_t NumSlots) {
    assert(NumSlots <= Data.size());
    Data.erase(std::begin(Data), std::begin(Data) + NumSlots);
  }
  /// Pushes result of an instruction to the stack.
  void addDef(unsigned Reg) { Data.push_front(Reg); }
  /// TODO: we need to decide how to handle these limitations.
  /// They shouldn't be defined in this scope.
  /// Maximal N in a valid PUSH sN instruction.
  static inline constexpr size_t PushLimit = 255;
  /// Maximal N, M in a valid XCHG sN, sM instruction.
  static inline constexpr size_t XchgLimit = 15;

private:
  const TargetInstrInfo *TII;
  std::deque<unsigned> Data;
};

class TVMExplicitLocals final : public MachineFunctionPass {
public:
  StringRef getPassName() const override { return "TVM Explicit Locals"; }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<LiveIntervals>();
    AU.addPreserved<LiveIntervals>();
    AU.addPreservedID(LiveVariablesID);
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  bool runOnMachineFunction(MachineFunction &MF) override;
  /// Inserts necessary stack manipulation instructions to supply \par MI with
  /// the correct data.
  bool processInstruction(MachineInstr &MI, LiveIntervals &LIS,
                          const TargetInstrInfo *TII, Stack &TheStack);

  static char ID; // Pass identification, replacement for typeid
  TVMExplicitLocals() : MachineFunctionPass(ID) {}

private:
  /// If \par MO is no longer used after \par MI.
  bool isKilled(const MachineInstr &MI, unsigned Register,
                const LiveIntervals &LIS) const;
  /// Forms vector of Pushes and Xchgs to supply an instruction with the right
  /// data.
  /// The function assumes that non-register arguments always come first.
  StackReorderings computeReorderings(MachineInstr &MI, LiveIntervals &LIS,
                                      Stack &TheStack,
                                      size_t NonStackOperands) const;
  /// Perform the specified stack manipulations and generates code for them.
  /// Insert instructions in the position specified by \par InsertPoint.
  void performReorderings(const StackReorderings &Reorderings,
                          MachineInstr *InsertPoint, Stack &TheStack);
};

} // end anonymous namespace

// Return true if reorderings might be optimized away for a commutative
// instruction.
static bool isCommutation(const StackReorderings &Reorderings,
                          const Stack &TheStack) {
  if (Reorderings.size() != 1u)
    return false;
  size_t SlotTo = Reorderings[0].SlotTo;
  size_t SlotFrom = TheStack.position(Reorderings[0].RegFrom);
  return (SlotTo == 1 && SlotFrom == 0) || (SlotTo == 0 && SlotFrom == 1);
}

// A shortcut overload for BuildMI() function
static inline MachineInstrBuilder BuildMI(MachineInstr *InsertPoint,
                                          const MCInstrDesc &InstrDesc) {
  return BuildMI(*InsertPoint->getParent(), InsertPoint,
                 InsertPoint->getDebugLoc(), InstrDesc);
}

char TVMExplicitLocals::ID = 0;
INITIALIZE_PASS(TVMExplicitLocals, DEBUG_TYPE,
                "Convert registers to TVM locals", false, false)

FunctionPass *llvm::createTVMExplicitLocals() {
  return new TVMExplicitLocals();
}

/// Get the appropriate Pop opcode for the given register class.
static unsigned getPopOpcode(const TargetRegisterClass *RC) {
  if (RC == &TVM::I64RegClass)
    return TVM::POP_S;
  llvm_unreachable("Unexpected register class");
}

/// Get the appropriate Push opcode for the given register class.
static unsigned getPushOpcode(const TargetRegisterClass *RC) {
  if (RC == &TVM::I64RegClass)
    return TVM::PUSH_S;
  llvm_unreachable("Unexpected register class");
}

static unsigned getXchgOpcode(const TargetRegisterClass *RC) {
  if (RC == &TVM::I64RegClass)
    return TVM::XCHG;
  llvm_unreachable("Unexpected register class");
}

bool Stack::clear(MachineInstr *InsertPoint, unsigned Preserved) {
  auto It = llvm::find(Data, Preserved);
  size_t NumDrops = 0, NumNips = 0;
  if (It == std::end(Data)) {
    NumDrops = Data.size();
  } else {
    NumDrops = std::distance(std::begin(Data), It);
    NumNips = Data.size() - NumDrops - 1;
  }
  unsigned Opc = getPopOpcode(&TVM::I64RegClass);
  // DROPs
  for (size_t i = 0; i < NumDrops; ++i)
    BuildMI(InsertPoint, TII->get(Opc)).addImm(0);
  // NIPs
  for (size_t i = 0; i < NumNips; ++i)
    BuildMI(InsertPoint, TII->get(Opc)).addImm(1);
  if (It == std::end(Data))
    Data.clear();
  else
    Data = {Data[NumDrops]};
  return NumDrops && NumNips;
}

bool Stack::push(MachineInstr *InsertPoint, unsigned Reg) {
  size_t RegSlot = position(Reg);
  assert(RegSlot <= PushLimit && "Unimplemented");
  unsigned Opc = getPushOpcode(&TVM::I64RegClass);
  BuildMI(InsertPoint, TII->get(Opc)).addImm(RegSlot);
  Data.push_front(Data[RegSlot]);
  return true;
}

bool Stack::xchg(MachineInstr *InsertPoint, unsigned RegFrom, size_t SlotTo) {
  auto It = llvm::find_or_fail(Data, RegFrom);
  size_t RegFromSlot = std::distance(std::begin(Data), It);
  assert(RegFromSlot <= XchgLimit && "Unimplemented");
  assert(RegFromSlot != SlotTo);
  unsigned Opc = getXchgOpcode(&TVM::I64RegClass);
  BuildMI(InsertPoint, TII->get(Opc))
      .addImm(std::min(RegFromSlot, SlotTo))
      .addImm(std::max(RegFromSlot, SlotTo));
  std::swap(*It, Data[SlotTo]);
  return true;
}

bool TVMExplicitLocals::isKilled(const MachineInstr &MI, unsigned Register,
                                 const LiveIntervals &LIS) const {
  const LiveInterval &LI = LIS.getInterval(Register);
  // If there is no live interval starting from the current instruction
  // for the given argument, the argument is killed.
  return !LI.getVNInfoAt(LIS.getInstructionIndex(MI).getRegSlot());
}

StackReorderings
TVMExplicitLocals::computeReorderings(MachineInstr &MI, LiveIntervals &LIS,
                                      Stack &TheStack,
                                      size_t NonStackOperands) const {
  StackReorderings Result{};
  size_t NumDefs = MI.getNumDefs();
  size_t NumOperands = MI.getNumOperands();
  size_t NumRegs = NumOperands - NumDefs - NonStackOperands;

  llvm::SmallSet<unsigned, 2> RegUsed{};

  // The same register could be used multiple times, but the stack keeps
  // the only copy of it, so we need to produce copies for each but the last
  // usage of the register even if its killed by MI.
  llvm::DenseMap<unsigned, size_t> LastUseOperandIndex(NextPowerOf2(2 * 4 / 3));
  for (size_t ROpNo = 0; ROpNo < NumRegs; ++ROpNo) {
    size_t OpNo = NumOperands - NonStackOperands - 1 - ROpNo;
    const auto &Operand = MI.getOperand(OpNo);
    assert(Operand.isReg());
    RegUsed.insert(Operand.getReg());
    LastUseOperandIndex[Operand.getReg()] = OpNo;
  }

  // Instruction format: INST %defs..., %register args... %non-register args...
  // Let's ensure that all instructions have expected type
  for (unsigned I = 0; I < NumOperands; ++I) {
    const auto &Op = MI.getOperand(I);
    if (I < NumDefs)
      assert(Op.isDef() && "Expected Def");
    else if (I < NumDefs + NumRegs)
      assert(Op.isReg() && "Expected Reg");
    else
      assert(Op.isImm() && "Expected Imm");
  }

  for (size_t ROpNo = 0; ROpNo < NumRegs; ++ROpNo) {
    size_t OpNo = NumOperands - 1 - NonStackOperands - ROpNo;
    const auto &Operand = MI.getOperand(OpNo);
    assert(Operand.isReg());
    unsigned RegFrom = Operand.getReg();
    auto Kind =
        (isKilled(MI, RegFrom, LIS) && LastUseOperandIndex[RegFrom] == OpNo)
            ? StackReorderingKind::Xchg
            : StackReorderingKind::Copy;
    Result.emplace_back(RegFrom, ROpNo, Kind);
  }

  // We need to adjust XChgs to number of non-register operands together with
  // number of Pushes followed by it.
  size_t NumPushes = 0;
  // Collect reorderings that are supposed to be removed later.
  llvm::SmallVector<const StackReordering *, 2> PseudoXchg{};
  for (auto &Reordering : Result) {
    size_t &SlotTo = Reordering.SlotTo;
    assert(SlotTo >= NumPushes);
    SlotTo -= NumPushes;
    if (Reordering.isCopy()) {
      ++NumPushes;
    } else {
      // Collect XCHG sN, sN pseudos.
      size_t SlotFrom = TheStack.position(Reordering.RegFrom) + NumPushes;
      if (SlotTo == SlotFrom)
        PseudoXchg.push_back(&Reordering);
      // Collect XCHG sM, sN (M > N) if XCHG sN, sM is also present and cyclic
      // dependencies of a bigger length.
      if (SlotTo < SlotFrom && SlotFrom < NumOperands - NumDefs &&
          exist(RegUsed, TheStack.reg(SlotTo)) &&
          isKilled(MI, TheStack.reg(SlotTo), LIS))
        PseudoXchg.push_back(&Reordering);
    }
  }

  // Remove redundant reorderings.
  Result.erase(
      llvm::remove_if(Result,
                      [&PseudoXchg](const StackReordering &Reordering) {
                        return llvm::find(PseudoXchg, &Reordering) !=
                               std::end(PseudoXchg);
                      }),
      std::end(Result));
  return Result;
}

void TVMExplicitLocals::performReorderings(const StackReorderings &Reorderings,
                                           MachineInstr *InsertPoint,
                                           Stack &TheStack) {
  if (Reorderings.empty())
    return;
  // We need to perform reorderings in reverse order except for a sequence of
  // XCHGs. E.g. if we have XCHG1, XCHG2, PUSH, it should be executed as PUSH,
  // XCHG1, XCHG2.
  size_t NumElements = Reorderings.size();
  size_t LastPush = NumElements;
  size_t RevIdx = 0;
  while (RevIdx < NumElements) {
    size_t Idx = NumElements - RevIdx - 1;
    if (Reorderings[Idx].isCopy()) {
      // If the current reordering is PUSH, execute preceding (in reverse order)
      // XCHGs
      for (size_t I = Idx + 1; I < LastPush; ++I) {
        assert(Reorderings[I].isXchg() && "Unexpected reordering");
        TheStack.xchg(InsertPoint, Reorderings[I]);
      }
      TheStack.push(InsertPoint, Reorderings[Idx].RegFrom);
      if (Reorderings[Idx].SlotTo > 0)
        TheStack.xchg(InsertPoint, Reorderings[Idx]);
      LastPush = Idx;
    }
    ++RevIdx;
  }
  // If reorderings start with XCHGs.
  for (size_t I = 0; I < LastPush; ++I) {
    assert(Reorderings[I].isXchg() && "Unexpected reordering");
    TheStack.xchg(InsertPoint, Reorderings[I]);
  }
}

bool TVMExplicitLocals::processInstruction(MachineInstr &MI, LiveIntervals &LIS,
                                           const TargetInstrInfo *TII,
                                           Stack &TheStack) {
  if (MI.isImplicitDef())
    return false;

  size_t NumDefs = MI.getNumDefs();
  size_t NumOperands = MI.getNumOperands();

  if (MI.isReturn()) {
    assert(NumOperands <= 2 && "Multiple returns are not implemented yet");
    if (NumOperands == 0)
      TheStack.clear(&MI);
    else
      TheStack.clear(&MI, MI.getOperand(0).getReg());
    MI.eraseFromParent();
    return true;
  }

  size_t NonStackOperands =
      count_if(MI.operands(), [](MachineOperand MOP) { return !MOP.isReg(); });
  bool Changed = false;
  StackReorderings Reorderings =
      computeReorderings(MI, LIS, TheStack, NonStackOperands);
  int NewOpcode = -1;
  if (isCommutation(Reorderings, TheStack)) {
    if (MI.isCommutable())
      Reorderings.clear();
    else if (MI.getOpcode() == TVM::SUB) {
      NewOpcode = TVM::SUBR_S;
      Reorderings.clear();
    }
  }
  if (NewOpcode < 0)
    NewOpcode = TVM::RegForm2SForm[MI.getOpcode()];
  performReorderings(Reorderings, &MI, TheStack);
  TheStack.consumeArguments(NumOperands - NonStackOperands - NumDefs);
  for (size_t ROpNo = 0; ROpNo < NumDefs; ++ROpNo) {
    const auto &Operand = MI.getOperand(NumDefs - ROpNo - 1);
    assert(Operand.isReg() && "Def must be a register");
    TheStack.addDef(Operand.getReg());
  }
  if (NewOpcode >= 0) {
    MachineInstrBuilder MIB = BuildMI(&MI, TII->get(NewOpcode));
    for (unsigned I = 0; I < NonStackOperands; I++) {
      const auto &Op = MI.getOperand(NumOperands - NonStackOperands + I);
      assert(Op.isImm());
      MIB.addImm(Op.getImm());
    }
    MI.removeFromParent();
    Changed = true;
  }
  return Changed;
}

// TODO: For now it only stackifies function arguments. Extend.
bool TVMExplicitLocals::runOnMachineFunction(MachineFunction &MF) {
  LLVM_DEBUG(dbgs() << "********** Make Locals Explicit **********\n"
                       "********** Function: "
                    << MF.getName() << '\n');

  bool Changed = false;
  TVMFunctionInfo &MFI = *MF.getInfo<TVMFunctionInfo>();
  const auto *TII = MF.getSubtarget<TVMSubtarget>().getInstrInfo();
  LiveIntervals &LIS = getAnalysis<LiveIntervals>();
  size_t NumArgs = MFI.numParams();
  Stack TheStack(TII, NumArgs);

  // Handle ARGUMENTS first to ensure that they get the designated numbers.
  for (MachineBasicBlock::iterator I = MF.begin()->begin(),
                                   E = MF.begin()->end();
       I != E;) {
    MachineInstr &MI = *I++;
    if (!TVM::isArgument(MI))
      break;
    unsigned Reg = MI.getOperand(0).getReg();
    assert(!MFI.isVRegStackified(Reg));
    unsigned ArgNo = NumArgs - MI.getOperand(1).getImm() - 1;
    TheStack.set(ArgNo, Reg);
    MI.eraseFromParent();
    Changed = true;
  }

  // Visit each instruction in the function.
  for (MachineBasicBlock &MBB : MF) {
    for (MachineBasicBlock::iterator I = MBB.begin(), E = MBB.end(); I != E;) {
      MachineInstr &MI = *I++;
      assert(!TVM::isArgument(MI));

      if (MI.isDebugInstr() || MI.isLabel())
        continue;

      // TODO: multiple defs should be handled separately.
      assert(MI.getDesc().getNumDefs() <= 1);
      Changed |= processInstruction(MI, LIS, TII, TheStack);
    }
  }

  return Changed;
}
