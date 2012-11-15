/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ART_SRC_COMPILER_COMPILER_IR_H_
#define ART_SRC_COMPILER_COMPILER_IR_H_

#include <vector>
#include "dex_instruction.h"
#include "compiler.h"
#include "compiler_utility.h"
#include "oat_compilation_unit.h"
#include "safe_map.h"
#include "greenland/ir_builder.h"
#include "llvm/Module.h"

namespace art {

#define SLOW_FIELD_PATH (cUnit->enableDebug & (1 << kDebugSlowFieldPath))
#define SLOW_INVOKE_PATH (cUnit->enableDebug & (1 << kDebugSlowInvokePath))
#define SLOW_STRING_PATH (cUnit->enableDebug & (1 << kDebugSlowStringPath))
#define SLOW_TYPE_PATH (cUnit->enableDebug & (1 << kDebugSlowTypePath))
#define EXERCISE_SLOWEST_STRING_PATH (cUnit->enableDebug & \
  (1 << kDebugSlowestStringPath))

// Minimum field size to contain Dalvik vReg number
#define VREG_NUM_WIDTH 16

struct ArenaBitVector;
struct LIR;
class LLVMInfo;

enum RegisterClass {
  kCoreReg,
  kFPReg,
  kAnyReg,
};

enum SpecialTargetRegister {
  kSelf,            // Thread
  kSuspend,         // Used to reduce suspend checks
  kLr,
  kPc,
  kSp,
  kArg0,
  kArg1,
  kArg2,
  kArg3,
  kFArg0,
  kFArg1,
  kFArg2,
  kFArg3,
  kRet0,
  kRet1,
  kInvokeTgt,
  kCount
};

enum RegLocationType {
  kLocDalvikFrame = 0, // Normal Dalvik register
  kLocPhysReg,
  kLocCompilerTemp,
  kLocInvalid
};

struct PromotionMap {
  RegLocationType coreLocation:3;
  uint8_t coreReg;
  RegLocationType fpLocation:3;
  uint8_t fpReg;
  bool firstInPair;
};

struct RegLocation {
  RegLocationType location:3;
  unsigned wide:1;
  unsigned defined:1;   // Do we know the type?
  unsigned isConst:1;   // Constant, value in cUnit->constantValues[]
  unsigned fp:1;        // Floating point?
  unsigned core:1;      // Non-floating point?
  unsigned ref:1;       // Something GC cares about
  unsigned highWord:1;  // High word of pair?
  unsigned home:1;      // Does this represent the home location?
  uint8_t lowReg;            // First physical register
  uint8_t highReg;           // 2nd physical register (if wide)
  int32_t sRegLow;      // SSA name for low Dalvik word
  int32_t origSReg;     // TODO: remove after Bitcode gen complete
                        // and consolodate usage w/ sRegLow
};

struct CompilerTemp {
  int sReg;
  ArenaBitVector* bv;
};

struct CallInfo {
  int numArgWords;      // Note: word count, not arg count
  RegLocation* args;    // One for each word of arguments
  RegLocation result;   // Eventual target of MOVE_RESULT
  int optFlags;
  InvokeType type;
  uint32_t dexIdx;
  uint32_t index;       // Method idx for invokes, type idx for FilledNewArray
  uintptr_t directCode;
  uintptr_t directMethod;
  RegLocation target;    // Target of following move_result
  bool skipThis;
  bool isRange;
  int offset;            // Dalvik offset
};

 /*
 * Data structure tracking the mapping between a Dalvik register (pair) and a
 * native register (pair). The idea is to reuse the previously loaded value
 * if possible, otherwise to keep the value in a native register as long as
 * possible.
 */
struct RegisterInfo {
  int reg;                    // Reg number
  bool inUse;                 // Has it been allocated?
  bool isTemp;                // Can allocate as temp?
  bool pair;                  // Part of a register pair?
  int partner;                // If pair, other reg of pair
  bool live;                  // Is there an associated SSA name?
  bool dirty;                 // If live, is it dirty?
  int sReg;                   // Name of live value
  LIR *defStart;              // Starting inst in last def sequence
  LIR *defEnd;                // Ending inst in last def sequence
};

struct RegisterPool {
  int numCoreRegs;
  RegisterInfo *coreRegs;
  int nextCoreReg;
  int numFPRegs;
  RegisterInfo *FPRegs;
  int nextFPReg;
};

#define INVALID_SREG (-1)
#define INVALID_VREG (0xFFFFU)
#define INVALID_REG (0xFF)
#define INVALID_OFFSET (0xDEADF00FU)

/* SSA encodings for special registers */
#define SSA_METHOD_BASEREG (-2)
/* First compiler temp basereg, grows smaller */
#define SSA_CTEMP_BASEREG (SSA_METHOD_BASEREG - 1)

/*
 * Some code patterns cause the generation of excessively large
 * methods - in particular initialization sequences.  There isn't much
 * benefit in optimizing these methods, and the cost can be very high.
 * We attempt to identify these cases, and avoid performing most dataflow
 * analysis.  Two thresholds are used - one for known initializers and one
 * for everything else.
 */
#define MANY_BLOCKS_INITIALIZER 1000 /* Threshold for switching dataflow off */
#define MANY_BLOCKS 4000 /* Non-initializer threshold */

enum BBType {
  kEntryBlock,
  kDalvikByteCode,
  kExitBlock,
  kExceptionHandling,
  kDead,
};

/* Utility macros to traverse the LIR list */
#define NEXT_LIR(lir) (lir->next)
#define PREV_LIR(lir) (lir->prev)

/* Defines for aliasInfo (tracks Dalvik register references) */
#define DECODE_ALIAS_INFO_REG(X)        (X & 0xffff)
#define DECODE_ALIAS_INFO_WIDE_FLAG     (0x80000000)
#define DECODE_ALIAS_INFO_WIDE(X)       ((X & DECODE_ALIAS_INFO_WIDE_FLAG) ? 1 : 0)
#define ENCODE_ALIAS_INFO(REG, ISWIDE)  (REG | (ISWIDE ? DECODE_ALIAS_INFO_WIDE_FLAG : 0))

/*
 * Def/Use encoding in 64-bit useMask/defMask.  Low positions used for target-specific
 * registers (and typically use the register number as the position).  High positions
 * reserved for common and abstract resources.
 */

enum ResourceEncodingPos {
  kMustNotAlias = 63,
  kHeapRef = 62,          // Default memory reference type
  kLiteral = 61,          // Literal pool memory reference
  kDalvikReg = 60,        // Dalvik vReg memory reference
  kFPStatus = 59,
  kCCode = 58,
  kLowestCommonResource = kCCode
};

/* Common resource macros */
#define ENCODE_CCODE            (1ULL << kCCode)
#define ENCODE_FP_STATUS        (1ULL << kFPStatus)

/* Abstract memory locations */
#define ENCODE_DALVIK_REG       (1ULL << kDalvikReg)
#define ENCODE_LITERAL          (1ULL << kLiteral)
#define ENCODE_HEAP_REF         (1ULL << kHeapRef)
#define ENCODE_MUST_NOT_ALIAS   (1ULL << kMustNotAlias)

#define ENCODE_ALL              (~0ULL)
#define ENCODE_MEM              (ENCODE_DALVIK_REG | ENCODE_LITERAL | \
                                 ENCODE_HEAP_REF | ENCODE_MUST_NOT_ALIAS)

struct LIR {
  int offset;                        // Offset of this instruction
  int dalvikOffset;                  // Offset of Dalvik opcode
  LIR* next;
  LIR* prev;
  LIR* target;
  int opcode;
  int operands[5];            // [0..4] = [dest, src1, src2, extra, extra2]
  struct {
    bool isNop:1;           // LIR is optimized away
    bool pcRelFixup:1;      // May need pc-relative fixup
    unsigned int size:5;    // in bytes
    unsigned int unused:25;
  } flags;
  int aliasInfo;              // For Dalvik register & litpool disambiguation
  uint64_t useMask;           // Resource mask for use
  uint64_t defMask;           // Resource mask for def
};

/* Shared pseudo opcodes - must be < 0 */
enum LIRPseudoOpcode {
  kPseudoExportedPC = -18,
  kPseudoSafepointPC = -17,
  kPseudoIntrinsicRetry = -16,
  kPseudoSuspendTarget = -15,
  kPseudoThrowTarget = -14,
  kPseudoCaseLabel = -13,
  kPseudoMethodEntry = -12,
  kPseudoMethodExit = -11,
  kPseudoBarrier = -10,
  kPseudoExtended = -9,
  kPseudoSSARep = -8,
  kPseudoEntryBlock = -7,
  kPseudoExitBlock = -6,
  kPseudoTargetLabel = -5,
  kPseudoDalvikByteCodeBoundary = -4,
  kPseudoPseudoAlign4 = -3,
  kPseudoEHBlockLabel = -2,
  kPseudoNormalBlockLabel = -1,
};

enum ExtendedMIROpcode {
  kMirOpFirst = kNumPackedOpcodes,
  kMirOpPhi = kMirOpFirst,
  kMirOpCopy,
  kMirOpFusedCmplFloat,
  kMirOpFusedCmpgFloat,
  kMirOpFusedCmplDouble,
  kMirOpFusedCmpgDouble,
  kMirOpFusedCmpLong,
  kMirOpNop,
  kMirOpNullCheck,
  kMirOpRangeCheck,
  kMirOpDivZeroCheck,
  kMirOpCheck,
  kMirOpLast,
};

struct SSARepresentation;

enum MIROptimizationFlagPositons {
  kMIRIgnoreNullCheck = 0,
  kMIRNullCheckOnly,
  kMIRIgnoreRangeCheck,
  kMIRRangeCheckOnly,
  kMIRInlined,                        // Invoke is inlined (ie dead)
  kMIRInlinedPred,                    // Invoke is inlined via prediction
  kMIRCallee,                         // Instruction is inlined from callee
  kMIRIgnoreSuspendCheck,
  kMIRDup,
  kMIRMark,                           // Temporary node mark
};

#define MIR_IGNORE_NULL_CHECK           (1 << kMIRIgnoreNullCheck)
#define MIR_NULL_CHECK_ONLY             (1 << kMIRNullCheckOnly)
#define MIR_IGNORE_RANGE_CHECK          (1 << kMIRIgnoreRangeCheck)
#define MIR_RANGE_CHECK_ONLY            (1 << kMIRRangeCheckOnly)
#define MIR_INLINED                     (1 << kMIRInlined)
#define MIR_INLINED_PRED                (1 << kMIRInlinedPred)
#define MIR_CALLEE                      (1 << kMIRCallee)
#define MIR_IGNORE_SUSPEND_CHECK        (1 << kMIRIgnoreSuspendCheck)
#define MIR_DUP                         (1 << kMIRDup)
#define MIR_MARK                        (1 << kMIRMark)

struct Checkstats {
  int nullChecks;
  int nullChecksEliminated;
  int rangeChecks;
  int rangeChecksEliminated;
};

struct MIR {
  DecodedInstruction dalvikInsn;
  unsigned int width;
  unsigned int offset;
  MIR* prev;
  MIR* next;
  SSARepresentation* ssaRep;
  int optimizationFlags;
  union {
    // Used to quickly locate all Phi opcodes
    MIR* phiNext;
    // Establish link between two halves of throwing instructions
    MIR* throwInsn;
  } meta;
};

struct BasicBlockDataFlow;

/* For successorBlockList */
enum BlockListType {
  kNotUsed = 0,
  kCatch,
  kPackedSwitch,
  kSparseSwitch,
};

struct BasicBlock {
  int id;
  int dfsId;
  bool visited;
  bool hidden;
  bool catchEntry;
  bool explicitThrow;
  bool conditionalBranch;
  bool hasReturn;
  uint16_t startOffset;
  uint16_t nestingDepth;
  BBType blockType;
  MIR* firstMIRInsn;
  MIR* lastMIRInsn;
  BasicBlock* fallThrough;
  BasicBlock* taken;
  BasicBlock* iDom;            // Immediate dominator
  BasicBlockDataFlow* dataFlowInfo;
  GrowableList* predecessors;
  ArenaBitVector* dominators;
  ArenaBitVector* iDominated;         // Set nodes being immediately dominated
  ArenaBitVector* domFrontier;        // Dominance frontier
  struct {                            // For one-to-many successors like
    BlockListType blockListType;    // switch and exception handling
    GrowableList blocks;
  } successorBlockList;
};

/*
 * The "blocks" field in "successorBlockList" points to an array of
 * elements with the type "SuccessorBlockInfo".
 * For catch blocks, key is type index for the exception.
 * For swtich blocks, key is the case value.
 */
struct SuccessorBlockInfo {
  BasicBlock* block;
  int key;
};

struct LoopAnalysis;
struct RegisterPool;
struct ArenaMemBlock;
struct Memstats;

enum AssemblerStatus {
  kSuccess,
  kRetryAll,
};

#define NOTVISITED (-1)

struct CompilationUnit {
  CompilationUnit()
    : numBlocks(0),
      compiler(NULL),
      class_linker(NULL),
      dex_file(NULL),
      class_loader(NULL),
      method_idx(0),
      code_item(NULL),
      access_flags(0),
      invoke_type(kDirect),
      shorty(NULL),
      firstLIRInsn(NULL),
      lastLIRInsn(NULL),
      literalList(NULL),
      methodLiteralList(NULL),
      codeLiteralList(NULL),
      disableOpt(0),
      enableDebug(0),
      dataOffset(0),
      totalSize(0),
      assemblerStatus(kSuccess),
      assemblerRetries(0),
      printMe(false),
      hasLoop(false),
      hasInvoke(false),
      qdMode(false),
      regPool(NULL),
      instructionSet(kNone),
      numSSARegs(0),
      ssaBaseVRegs(NULL),
      ssaSubscripts(NULL),
      ssaStrings(NULL),
      vRegToSSAMap(NULL),
      SSALastDefs(NULL),
      isConstantV(NULL),
      constantValues(NULL),
      phiAliasMap(NULL),
      phiList(NULL),
      regLocation(NULL),
      promotionMap(NULL),
      methodSReg(0),
      numReachableBlocks(0),
      numDalvikRegisters(0),
      entryBlock(NULL),
      exitBlock(NULL),
      curBlock(NULL),
      iDomList(NULL),
      tryBlockAddr(NULL),
      defBlockMatrix(NULL),
      tempBlockV(NULL),
      tempDalvikRegisterV(NULL),
      tempSSARegisterV(NULL),
      tempSSABlockIdV(NULL),
      blockLabelList(NULL),
      numIns(0),
      numOuts(0),
      numRegs(0),
      numCoreSpills(0),
      numFPSpills(0),
      numCompilerTemps(0),
      frameSize(0),
      coreSpillMask(0U),
      fpSpillMask(0U),
      attrs(0U),
      currentDalvikOffset(0),
      insns(NULL),
      insnsSize(0U),
      disableDataflow(false),
      defCount(0),
      compilerFlipMatch(false),
      arenaHead(NULL),
      currentArena(NULL),
      numArenaBlocks(0),
      mstats(NULL),
      checkstats(NULL),
      genBitcode(false),
      context(NULL),
      module(NULL),
      func(NULL),
      intrinsic_helper(NULL),
      irb(NULL),
      placeholderBB(NULL),
      entryBB(NULL),
      entryTargetBB(NULL),
      tempName(0),
      numShadowFrameEntries(0),
      shadowMap(NULL),
#ifndef NDEBUG
      liveSReg(0),
#endif
      opcodeCount(NULL) {}

  int numBlocks;
  GrowableList blockList;
  Compiler* compiler;            // Compiler driving this compiler
  ClassLinker* class_linker;     // Linker to resolve fields and methods
  const DexFile* dex_file;       // DexFile containing the method being compiled
  jobject class_loader;          // compiling method's class loader
  uint32_t method_idx;                // compiling method's index into method_ids of DexFile
  const DexFile::CodeItem* code_item;  // compiling method's DexFile code_item
  uint32_t access_flags;              // compiling method's access flags
  InvokeType invoke_type;             // compiling method's invocation type
  const char* shorty;                 // compiling method's shorty
  LIR* firstLIRInsn;
  LIR* lastLIRInsn;
  LIR* literalList;                   // Constants
  LIR* methodLiteralList;             // Method literals requiring patching
  LIR* codeLiteralList;               // Code literals requiring patching
  uint32_t disableOpt;                // optControlVector flags
  uint32_t enableDebug;               // debugControlVector flags
  int dataOffset;                     // starting offset of literal pool
  int totalSize;                      // header + code size
  AssemblerStatus assemblerStatus;    // Success or fix and retry
  int assemblerRetries;
  std::vector<uint8_t> codeBuffer;
  /*
   * Holds mapping from native PC to dex PC for safepoints where we may deoptimize.
   * Native PC is on the return address of the safepointed operation.  Dex PC is for
   * the instruction being executed at the safepoint.
   */
  std::vector<uint32_t> pc2dexMappingTable;
  /*
   * Holds mapping from Dex PC to native PC for catch entry points.  Native PC and Dex PC
   * immediately preceed the instruction.
   */
  std::vector<uint32_t> dex2pcMappingTable;
  std::vector<uint32_t> combinedMappingTable;
  std::vector<uint32_t> coreVmapTable;
  std::vector<uint32_t> fpVmapTable;
  std::vector<uint8_t> nativeGcMap;
  bool printMe;
  bool hasLoop;                       // Contains a loop
  bool hasInvoke;                     // Contains an invoke instruction
  bool qdMode;                        // Compile for code size/compile time
  RegisterPool* regPool;
  InstructionSet instructionSet;
  /* Number of total regs used in the whole cUnit after SSA transformation */
  int numSSARegs;
  /* Map SSA reg i to the base virtual register/subscript */
  GrowableList* ssaBaseVRegs;
  GrowableList* ssaSubscripts;
  GrowableList* ssaStrings;

  /* The following are new data structures to support SSA representations */
  /* Map original Dalvik virtual reg i to the current SSA name */
  int* vRegToSSAMap;                  // length == method->registersSize
  int* SSALastDefs;                   // length == method->registersSize
  ArenaBitVector* isConstantV;        // length == numSSAReg
  int* constantValues;                // length == numSSAReg
  int* phiAliasMap;                   // length == numSSAReg
  MIR* phiList;

  /* Use counts of ssa names */
  GrowableList useCounts;             // Weighted by nesting depth
  GrowableList rawUseCounts;          // Not weighted

  /* Optimization support */
  GrowableList loopHeaders;

  /* Map SSA names to location */
  RegLocation* regLocation;

  /* Keep track of Dalvik vReg to physical register mappings */
  PromotionMap* promotionMap;

  /* SSA name for Method* */
  int methodSReg;
  RegLocation methodLoc;            // Describes location of method*

  int numReachableBlocks;
  int numDalvikRegisters;             // method->registersSize
  BasicBlock* entryBlock;
  BasicBlock* exitBlock;
  BasicBlock* curBlock;
  GrowableList dfsOrder;
  GrowableList dfsPostOrder;
  GrowableList domPostOrderTraversal;
  GrowableList throwLaunchpads;
  GrowableList suspendLaunchpads;
  GrowableList intrinsicLaunchpads;
  GrowableList compilerTemps;
  int* iDomList;
  ArenaBitVector* tryBlockAddr;
  ArenaBitVector** defBlockMatrix;    // numDalvikRegister x numBlocks
  ArenaBitVector* tempBlockV;
  ArenaBitVector* tempDalvikRegisterV;
  ArenaBitVector* tempSSARegisterV;   // numSSARegs
  int* tempSSABlockIdV;               // working storage for Phi labels
  LIR* blockLabelList;
  /*
   * Frame layout details.
   * NOTE: for debug support it will be necessary to add a structure
   * to map the Dalvik virtual registers to the promoted registers.
   * NOTE: "num" fields are in 4-byte words, "Size" and "Offset" in bytes.
   */
  int numIns;
  int numOuts;
  int numRegs;            // Unlike numDalvikRegisters, does not include ins
  int numCoreSpills;
  int numFPSpills;
  int numCompilerTemps;
  int frameSize;
  unsigned int coreSpillMask;
  unsigned int fpSpillMask;
  unsigned int attrs;
  /*
   * CLEANUP/RESTRUCTURE: The code generation utilities don't have a built-in
   * mechanism to propagate the original Dalvik opcode address to the
   * associated generated instructions.  For the trace compiler, this wasn't
   * necessary because the interpreter handled all throws and debugging
   * requests.  For now we'll handle this by placing the Dalvik offset
   * in the CompilationUnit struct before codegen for each instruction.
   * The low-level LIR creation utilites will pull it from here.  Should
   * be rewritten.
   */
  int currentDalvikOffset;
  GrowableList switchTables;
  GrowableList fillArrayData;
  const uint16_t* insns;
  uint32_t insnsSize;
  bool disableDataflow; // Skip dataflow analysis if possible
  SafeMap<unsigned int, BasicBlock*> blockMap; // findBlock lookup cache
  SafeMap<unsigned int, unsigned int> blockIdMap; // Block collapse lookup cache
  SafeMap<unsigned int, LIR*> boundaryMap; // boundary lookup cache
  int defCount;         // Used to estimate number of SSA names

  // If non-empty, apply optimizer/debug flags only to matching methods.
  std::string compilerMethodMatch;
  // Flips sense of compilerMethodMatch - apply flags if doesn't match.
  bool compilerFlipMatch;
  ArenaMemBlock* arenaHead;
  ArenaMemBlock* currentArena;
  int numArenaBlocks;
  Memstats* mstats;
  Checkstats* checkstats;
  bool genBitcode;
  LLVMInfo* llvm_info;
  llvm::LLVMContext* context;
  llvm::Module* module;
  llvm::Function* func;
  greenland::IntrinsicHelper* intrinsic_helper;
  greenland::IRBuilder* irb;
  llvm::BasicBlock* placeholderBB;
  llvm::BasicBlock* entryBB;
  llvm::BasicBlock* entryTargetBB;
  std::string bitcode_filename;
  GrowableList llvmValues;
  int32_t tempName;
  SafeMap<llvm::BasicBlock*, LIR*> blockToLabelMap; // llvm bb -> LIR label
  SafeMap<int32_t, llvm::BasicBlock*> idToBlockMap; // block id -> llvm bb
  SafeMap<llvm::Value*, RegLocation> locMap; // llvm Value to loc rec
  int numShadowFrameEntries;
  int* shadowMap;
  std::set<llvm::BasicBlock*> llvmBlocks;
#ifndef NDEBUG
  /*
   * Sanity checking for the register temp tracking.  The same ssa
   * name should never be associated with one temp register per
   * instruction compilation.
   */
  int liveSReg;
#endif
  std::set<uint32_t> catches;
  int* opcodeCount;    // Count Dalvik opcodes for tuning
};

enum OpSize {
  kWord,
  kLong,
  kSingle,
  kDouble,
  kUnsignedHalf,
  kSignedHalf,
  kUnsignedByte,
  kSignedByte,
};

enum OpKind {
  kOpMov,
  kOpMvn,
  kOpCmp,
  kOpLsl,
  kOpLsr,
  kOpAsr,
  kOpRor,
  kOpNot,
  kOpAnd,
  kOpOr,
  kOpXor,
  kOpNeg,
  kOpAdd,
  kOpAdc,
  kOpSub,
  kOpSbc,
  kOpRsub,
  kOpMul,
  kOpDiv,
  kOpRem,
  kOpBic,
  kOpCmn,
  kOpTst,
  kOpBkpt,
  kOpBlx,
  kOpPush,
  kOpPop,
  kOp2Char,
  kOp2Short,
  kOp2Byte,
  kOpCondBr,
  kOpUncondBr,
  kOpBx,
  kOpInvalid,
};

std::ostream& operator<<(std::ostream& os, const OpKind& kind);

enum ConditionCode {
  kCondEq,  // equal
  kCondNe,  // not equal
  kCondCs,  // carry set (unsigned less than)
  kCondUlt = kCondCs,
  kCondCc,  // carry clear (unsigned greater than or same)
  kCondUge = kCondCc,
  kCondMi,  // minus
  kCondPl,  // plus, positive or zero
  kCondVs,  // overflow
  kCondVc,  // no overflow
  kCondHi,  // unsigned greater than
  kCondLs,  // unsigned lower or same
  kCondGe,  // signed greater than or equal
  kCondLt,  // signed less than
  kCondGt,  // signed greater than
  kCondLe,  // signed less than or equal
  kCondAl,  // always
  kCondNv,  // never
};

// Target specific condition encodings
enum ArmConditionCode {
  kArmCondEq = 0x0,  /* 0000 */
  kArmCondNe = 0x1,  /* 0001 */
  kArmCondCs = 0x2,  /* 0010 */
  kArmCondCc = 0x3,  /* 0011 */
  kArmCondMi = 0x4,  /* 0100 */
  kArmCondPl = 0x5,  /* 0101 */
  kArmCondVs = 0x6,  /* 0110 */
  kArmCondVc = 0x7,  /* 0111 */
  kArmCondHi = 0x8,  /* 1000 */
  kArmCondLs = 0x9,  /* 1001 */
  kArmCondGe = 0xa,  /* 1010 */
  kArmCondLt = 0xb,  /* 1011 */
  kArmCondGt = 0xc,  /* 1100 */
  kArmCondLe = 0xd,  /* 1101 */
  kArmCondAl = 0xe,  /* 1110 */
  kArmCondNv = 0xf,  /* 1111 */
};

enum X86ConditionCode {
  kX86CondO   = 0x0,    // overflow
  kX86CondNo  = 0x1,    // not overflow

  kX86CondB   = 0x2,    // below
  kX86CondNae = kX86CondB,  // not-above-equal
  kX86CondC   = kX86CondB,  // carry

  kX86CondNb  = 0x3,    // not-below
  kX86CondAe  = kX86CondNb, // above-equal
  kX86CondNc  = kX86CondNb, // not-carry

  kX86CondZ   = 0x4,    // zero
  kX86CondEq  = kX86CondZ,  // equal

  kX86CondNz  = 0x5,    // not-zero
  kX86CondNe  = kX86CondNz, // not-equal

  kX86CondBe  = 0x6,    // below-equal
  kX86CondNa  = kX86CondBe, // not-above

  kX86CondNbe = 0x7,    // not-below-equal
  kX86CondA   = kX86CondNbe,// above

  kX86CondS   = 0x8,    // sign
  kX86CondNs  = 0x9,    // not-sign

  kX86CondP   = 0xA,    // 8-bit parity even
  kX86CondPE  = kX86CondP,

  kX86CondNp  = 0xB,    // 8-bit parity odd
  kX86CondPo  = kX86CondNp,

  kX86CondL   = 0xC,    // less-than
  kX86CondNge = kX86CondL,  // not-greater-equal

  kX86CondNl  = 0xD,    // not-less-than
  kX86CondGe  = kX86CondNl, // not-greater-equal

  kX86CondLe  = 0xE,    // less-than-equal
  kX86CondNg  = kX86CondLe, // not-greater

  kX86CondNle = 0xF,    // not-less-than
  kX86CondG   = kX86CondNle,// greater
};


enum ThrowKind {
  kThrowNullPointer,
  kThrowDivZero,
  kThrowArrayBounds,
  kThrowNoSuchMethod,
  kThrowStackOverflow,
};

struct SwitchTable {
  int offset;
  const uint16_t* table;            // Original dex table
  int vaddr;                  // Dalvik offset of switch opcode
  LIR* anchor;                // Reference instruction for relative offsets
  LIR** targets;              // Array of case targets
};

struct FillArrayData {
  int offset;
  const uint16_t* table;           // Original dex table
  int size;
  int vaddr;                 // Dalvik offset of FILL_ARRAY_DATA opcode
};

#define MAX_PATTERN_LEN 5

enum SpecialCaseHandler {
  kNoHandler,
  kNullMethod,
  kConstFunction,
  kIGet,
  kIGetBoolean,
  kIGetObject,
  kIGetByte,
  kIGetChar,
  kIGetShort,
  kIGetWide,
  kIPut,
  kIPutBoolean,
  kIPutObject,
  kIPutByte,
  kIPutChar,
  kIPutShort,
  kIPutWide,
  kIdentity,
};

struct CodePattern {
  const Instruction::Code opcodes[MAX_PATTERN_LEN];
  const SpecialCaseHandler handlerCode;
};

static const CodePattern specialPatterns[] = {
  {{Instruction::RETURN_VOID}, kNullMethod},
  {{Instruction::CONST, Instruction::RETURN}, kConstFunction},
  {{Instruction::CONST_4, Instruction::RETURN}, kConstFunction},
  {{Instruction::CONST_4, Instruction::RETURN_OBJECT}, kConstFunction},
  {{Instruction::CONST_16, Instruction::RETURN}, kConstFunction},
  {{Instruction::IGET, Instruction:: RETURN}, kIGet},
  {{Instruction::IGET_BOOLEAN, Instruction::RETURN}, kIGetBoolean},
  {{Instruction::IGET_OBJECT, Instruction::RETURN_OBJECT}, kIGetObject},
  {{Instruction::IGET_BYTE, Instruction::RETURN}, kIGetByte},
  {{Instruction::IGET_CHAR, Instruction::RETURN}, kIGetChar},
  {{Instruction::IGET_SHORT, Instruction::RETURN}, kIGetShort},
  {{Instruction::IGET_WIDE, Instruction::RETURN_WIDE}, kIGetWide},
  {{Instruction::IPUT, Instruction::RETURN_VOID}, kIPut},
  {{Instruction::IPUT_BOOLEAN, Instruction::RETURN_VOID}, kIPutBoolean},
  {{Instruction::IPUT_OBJECT, Instruction::RETURN_VOID}, kIPutObject},
  {{Instruction::IPUT_BYTE, Instruction::RETURN_VOID}, kIPutByte},
  {{Instruction::IPUT_CHAR, Instruction::RETURN_VOID}, kIPutChar},
  {{Instruction::IPUT_SHORT, Instruction::RETURN_VOID}, kIPutShort},
  {{Instruction::IPUT_WIDE, Instruction::RETURN_VOID}, kIPutWide},
  {{Instruction::RETURN}, kIdentity},
  {{Instruction::RETURN_OBJECT}, kIdentity},
  {{Instruction::RETURN_WIDE}, kIdentity},
};

BasicBlock* oatNewBB(CompilationUnit* cUnit, BBType blockType, int blockId);

void oatAppendMIR(BasicBlock* bb, MIR* mir);

void oatPrependMIR(BasicBlock* bb, MIR* mir);

void oatInsertMIRAfter(BasicBlock* bb, MIR* currentMIR, MIR* newMIR);

void oatAppendLIR(CompilationUnit* cUnit, LIR* lir);

void oatInsertLIRBefore(LIR* currentLIR, LIR* newLIR);

void oatInsertLIRAfter(LIR* currentLIR, LIR* newLIR);

MIR* oatFindMoveResult(CompilationUnit* cUnit, BasicBlock* bb, MIR* mir);
/* Debug Utilities */
void oatDumpCompilationUnit(CompilationUnit* cUnit);

}  // namespace art

#endif // ART_SRC_COMPILER_COMPILER_IR_H_
