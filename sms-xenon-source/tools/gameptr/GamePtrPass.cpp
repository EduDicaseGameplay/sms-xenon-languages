// A Clang pass plugin for the native Apple Silicon build (build/macos-arm64).
//
// The game keeps pointers in 32-bit slots, so every address it can see has to
// survive a round trip through a u32. The other 64-bit builds keep that
// memory below 4 GiB; arm64 macOS reserves the whole low 4 GiB (PAGEZERO, and
// the kernel refuses a smaller one), and its shared library cache takes
// 0x180000000 to 0x300000000. So there a game address x lives at
//   0x100000000 + x  for x below 0x80000000 (the executable, which the kernel
//                    loads just above PAGEZERO, thread stacks, port_low_alloc)
//   0x10000000000 + x  from 0x80000000 (MEM1)
// (PORT_GAME_LO and PORT_GAME_HI in the port's dolphin/types.h). Truncating
// such a pointer gives the same 32-bit address as on the other builds; this
// pass makes the way back work by rebasing every integer the code turns into
// a pointer: a value from 64 KiB up to 4 GiB (never a valid address on arm64
// macOS) gets its half's base added. Smaller values stay as they are: no game
// address is that low, and they are small integers passed as pointers (null,
// the audio thread's OSMessage codes, which it reads back as integers).
//
// It rewrites each inttoptr instruction, and each constant inttoptr (the
// game's fixed low-memory addresses, such as 0x80000028). Pointer arithmetic
// on such a pointer is 32-bit arithmetic on the console, and relocations use
// it with a file offset as the pointer: (u8*)info->mLayerInfos + (u32)info,
// an offset rebased into the low half plus an address in MEM1. So each
// getelementptr whose base is one of those casts, and whose result is in or
// just past a game half, is rebased again from the low 32 bits of its result
// (no object spans the two halves, so this changes nothing else; a host
// pointer made from an integer, such as the heap's, keeps its address).
//
// It also gives float-to-integer conversions the results they have on x86
// (and, for the cases the game relies on, on the console), where C leaves
// them undefined: a negative float converted to an unsigned type wraps
// (-41.9 to u8 is 0xD7, J3DTevs' colour arithmetic; arm64 would clamp it to
// 0), and a value out of a signed 32-bit int's range gives 0x80000000 (arm64
// would saturate). So the x86_64 and arm64 builds compute the same frames.
//
// Built at configure time against the compiler's own LLVM, and loaded with
// -fpass-plugin= for the game and the platform layer (not platform/gx, whose
// GL calls pass buffer offsets as pointers).

#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#if __has_include("llvm/Plugins/PassPlugin.h")
#include "llvm/Plugins/PassPlugin.h"  // LLVM 22 on
#else
#include "llvm/Passes/PassPlugin.h"
#endif

#include <map>
#include <set>
#include <vector>

using namespace llvm;

namespace {

constexpr uint64_t kLo = 0x100000000ull;  // PORT_GAME_LO
constexpr uint64_t kHi = 0x10000000000ull;  // PORT_GAME_HI

constexpr uint64_t kMin = 0x10000;  // PORT_GAME_MIN

bool rebased(uint64_t v) { return v >= kMin && v <= 0xFFFFFFFFull; }
uint64_t rebase(uint64_t v) { return v | (v & 0x80000000u ? kHi : kLo); }

struct GamePtr : PassInfoMixin<GamePtr> {
    std::map<Constant*, Constant*> memo;

    // The constant with every inttoptr of a value in (0, 4 GiB) rebased.
    Constant* fixConst(Constant* c) {
        if (isa<GlobalValue>(c) || isa<ConstantData>(c)) return c;
        auto it = memo.find(c);
        if (it != memo.end()) return it->second;
        Constant* out = c;
        if (auto* ce = dyn_cast<ConstantExpr>(c)) {
            if (ce->getOpcode() == Instruction::IntToPtr) {
                if (auto* ci = dyn_cast<ConstantInt>(ce->getOperand(0))) {
                    const APInt& v = ci->getValue();
                    if (v.getActiveBits() <= 32 && rebased(v.getZExtValue())) {
                        Type* i64 = Type::getInt64Ty(c->getContext());
                        out = ConstantExpr::getIntToPtr(ConstantInt::get(i64, rebase(v.getZExtValue())), ce->getType());
                    }
                }
            } else {
                std::vector<Constant*> ops;
                bool changed = false;
                for (Use& u : ce->operands()) {
                    Constant* o = fixConst(cast<Constant>(u.get()));
                    changed |= o != u.get();
                    ops.push_back(o);
                }
                if (changed) out = ce->getWithOperands(ops);
            }
        } else if (isa<ConstantAggregate>(c)) {
            std::vector<Constant*> ops;
            bool changed = false;
            for (Use& u : c->operands()) {
                Constant* o = fixConst(cast<Constant>(u.get()));
                changed |= o != u.get();
                ops.push_back(o);
            }
            if (changed) {
                if (auto* s = dyn_cast<ConstantStruct>(c)) out = ConstantStruct::get(s->getType(), ops);
                else if (auto* a = dyn_cast<ConstantArray>(c)) out = ConstantArray::get(a->getType(), ops);
                else out = ConstantVector::get(ops);
            }
        }
        memo[c] = out;
        return out;
    }

    PreservedAnalyses run(Module& m, ModuleAnalysisManager&) {
        bool changed = false;
        for (GlobalVariable& g : m.globals()) {
            if (!g.hasInitializer()) continue;
            Constant* init = g.getInitializer();
            Constant* fixed = fixConst(init);
            if (fixed != init) {
                g.setInitializer(fixed);
                changed = true;
            }
        }
        Type* i64 = Type::getInt64Ty(m.getContext());
        std::vector<IntToPtrInst*> casts;
        std::vector<CastInst*> fcasts;
        for (Function& f : m) {
            for (BasicBlock& bb : f) {
                for (Instruction& in : bb) {
                    if (auto* ip = dyn_cast<IntToPtrInst>(&in)) casts.push_back(ip);
                    if ((isa<FPToUIInst>(in) || isa<FPToSIInst>(in)) && in.getType()->getScalarSizeInBits() <= 32)
                        fcasts.push_back(cast<CastInst>(&in));
                    for (Use& u : in.operands()) {
                        auto* c = dyn_cast<Constant>(u.get());
                        if (!c || isa<GlobalValue>(c) || isa<ConstantData>(c)) continue;
                        Constant* fixed = fixConst(c);
                        if (fixed != c) {
                            u.set(fixed);
                            changed = true;
                        }
                    }
                }
            }
        }
        auto emitRebase = [&](IRBuilder<>& b, Value* w) -> Value* {
            Value* low = b.CreateICmpULT(b.CreateSub(w, ConstantInt::get(i64, kMin)),
                                         ConstantInt::get(i64, 0x100000000ull - kMin));
            Value* half = b.CreateSelect(b.CreateICmpUGE(w, ConstantInt::get(i64, 0x80000000u)), ConstantInt::get(i64, kHi),
                                         ConstantInt::get(i64, kLo));
            return b.CreateOr(w, b.CreateSelect(low, half, ConstantInt::get(i64, 0)));
        };
        std::set<Value*> fromInt;  // the pointers made from integers
        for (IntToPtrInst* ip : casts) {
            Value* v = ip->getOperand(0);
            if (isa<Constant>(v)) {  // folded by fixConst's rules
                if (auto* ci = dyn_cast<ConstantInt>(v)) {
                    const APInt& a = ci->getValue();
                    if (a.getActiveBits() <= 32 && rebased(a.getZExtValue())) {
                        ip->setOperand(0, ConstantInt::get(i64, rebase(a.getZExtValue())));
                        changed = true;
                    }
                }
                continue;
            }
            if (v->getType()->isVectorTy()) continue;
            IRBuilder<> b(ip);
            ip->setOperand(0, emitRebase(b, b.CreateZExtOrTrunc(v, i64)));
            fromInt.insert(ip);
            changed = true;
        }
        // getelementptrs on those pointers (through casts and other geps)
        std::vector<GetElementPtrInst*> geps;
        for (Function& f : m)
            for (BasicBlock& bb : f)
                for (Instruction& in : bb)
                    if (auto* g = dyn_cast<GetElementPtrInst>(&in)) {
                        if (g->getType()->isVectorTy()) continue;
                        Value* root = g->getPointerOperand()->stripPointerCasts();
                        while (auto* gg = dyn_cast<GEPOperator>(root)) root = gg->getPointerOperand()->stripPointerCasts();
                        if (fromInt.count(root)) geps.push_back(g);
                    }
        for (GetElementPtrInst* g : geps) {
            IRBuilder<> b(g->getNextNode());
            Value* p = b.CreatePtrToInt(g, i64);
            Value* w = b.CreateAnd(p, ConstantInt::get(i64, 0xFFFFFFFFull));
            // Only a result in a game half, or just past one (32-bit sums of
            // game addresses: 0x1xxxxxxxx-0x2xxxxxxxx, 0x100xxxxxxxx-0x101xxxxxxxx),
            // comes back from its low 32 bits. A host pointer made from an
            // integer (a small struct returned in registers, such as
            // unordered_map's pair<iterator, bool>; the malloc heap is far
            // above the game window) keeps its address.
            Value* hi = b.CreateLShr(p, 32);
            Value* inGame = b.CreateOr(b.CreateICmpULT(b.CreateSub(hi, ConstantInt::get(i64, kLo >> 32)), ConstantInt::get(i64, 2)),
                                       b.CreateICmpULT(b.CreateSub(hi, ConstantInt::get(i64, kHi >> 32)), ConstantInt::get(i64, 2)));
            Value* np = b.CreateIntToPtr(b.CreateSelect(inGame, emitRebase(b, w), p), g->getType());
            g->replaceUsesWithIf(np, [&](Use& u) { return u.getUser() != p; });
            changed = true;
        }
        for (CastInst* ci : fcasts) {
            IRBuilder<> b(ci);
            Type* dt = ci->getDestTy();
            Value* x = ci->getOperand(0);
            auto like = [&](Type* elem) -> Type* {
                if (auto* vt = dyn_cast<VectorType>(dt)) return VectorType::get(elem, vt->getElementCount());
                return elem;
            };
            Type* t64 = like(i64);
            // x86's cvttss2si/cvttsd2si: truncation toward zero, in a 64-bit
            // register for unsigned targets (the low bits kept), in a 32-bit
            // one for signed targets (0x80000000 when out of range or NaN)
            Value* s = b.CreateIntrinsic(t64, Intrinsic::fptosi_sat, {x});
            Value* res;
            if (isa<FPToUIInst>(ci)) {
                res = b.CreateTrunc(s, dt);
            } else {
                Type* t32 = like(Type::getInt32Ty(m.getContext()));
                Value* in = b.CreateAnd(b.CreateAnd(b.CreateICmpSGE(s, ConstantInt::get(t64, -0x80000000ll, true)),
                                                    b.CreateICmpSLE(s, ConstantInt::get(t64, 0x7FFFFFFFll))),
                                        b.CreateFCmpORD(x, x));
                Value* r32 = b.CreateSelect(in, b.CreateTrunc(s, t32), ConstantInt::get(t32, 0x80000000u));
                res = dt == t32 ? r32 : b.CreateTrunc(r32, dt);
            }
            ci->replaceAllUsesWith(res);
            ci->eraseFromParent();
            changed = true;
        }
        return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
    }
};

}  // namespace

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
    return {LLVM_PLUGIN_API_VERSION, "SmsGamePtr", "1", [](PassBuilder& pb) {
                pb.registerOptimizerLastEPCallback(
                    [](ModulePassManager& mpm, OptimizationLevel, ThinOrFullLTOPhase) { mpm.addPass(GamePtr()); });
            }};
}
