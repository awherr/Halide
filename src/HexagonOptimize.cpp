#include "HexagonOptimize.h"
#include "IRMutator.h"
#include "IROperator.h"
#include "IRMatch.h"
#include "IREquality.h"
#include "ExprUsesVar.h"
#include "CSE.h"
#include "Simplify.h"
#include "Substitute.h"
#include "Scope.h"
#include "Bounds.h"

namespace Halide {
namespace Internal {

using std::set;
using std::vector;
using std::string;
using std::pair;

Expr native_interleave(Expr x) {
    string fn;
    switch (x.type().bits()) {
    case 8: fn = "halide.hexagon.interleave.vb"; break;
    case 16: fn = "halide.hexagon.interleave.vh"; break;
    case 32: fn = "halide.hexagon.interleave.vw"; break;
    default: internal_error << "Cannot interleave native vectors of type " << x.type() << "\n";
    }
    return Call::make(x.type(), fn, {x}, Call::PureExtern);
}

Expr native_deinterleave(Expr x) {
    string fn;
    switch (x.type().bits()) {
    case 8: fn = "halide.hexagon.deinterleave.vb"; break;
    case 16: fn = "halide.hexagon.deinterleave.vh"; break;
    case 32: fn = "halide.hexagon.deinterleave.vw"; break;
    default: internal_error << "Cannot deinterleave native vectors of type " << x.type() << "\n";
    }
    return Call::make(x.type(), fn, {x}, Call::PureExtern);
}

bool is_native_interleave_op(Expr x, const char *name) {
    const Call *c = x.as<Call>();
    if (!c || c->args.size() != 1) return false;
    return starts_with(c->name, name);
}

bool is_native_interleave(Expr x) {
    return is_native_interleave_op(x, "halide.hexagon.interleave");
}

bool is_native_deinterleave(Expr x) {
    return is_native_interleave_op(x, "halide.hexagon.deinterleave");
}

namespace {

// This mutator rewrites patterns with an unknown number of lanes to
// have the specified number of lanes.
class WithLanes : public IRMutator {
    using IRMutator::visit;

    int lanes;

    Type with_lanes(Type t) { return t.with_lanes(lanes); }

    void visit(const Cast *op) {
        if (op->type.lanes() != lanes) {
            expr = Cast::make(with_lanes(op->type), mutate(op->value));
        } else {
            IRMutator::visit(op);
        }
    }

    void visit(const Variable *op) {
        if (op->type.lanes() != lanes) {
            expr = Variable::make(with_lanes(op->type), op->name);
        } else {
            expr = op;
        }
    }

    void visit(const Broadcast *op) {
        if (op->type.lanes() != lanes) {
            expr = Broadcast::make(op->value, lanes);
        } else {
            IRMutator::visit(op);
        }
    }

public:
    WithLanes(int lanes) : lanes(lanes) {}
};

Expr with_lanes(Expr x, int lanes) {
    return WithLanes(lanes).mutate(x);
}

Expr u8(Expr E) { return cast(UInt(8, E.type().lanes()), E); }
Expr i8(Expr E) { return cast(Int(8, E.type().lanes()), E); }
Expr u16(Expr E) { return cast(UInt(16, E.type().lanes()), E); }
Expr i16(Expr E) { return cast(Int(16, E.type().lanes()), E); }
Expr u32(Expr E) { return cast(UInt(32, E.type().lanes()), E); }
Expr i32(Expr E) { return cast(Int(32, E.type().lanes()), E); }
Expr u64(Expr E) { return cast(UInt(64, E.type().lanes()), E); }
Expr i64(Expr E) { return cast(Int(64, E.type().lanes()), E); }
Expr bc(Expr E) { return Broadcast::make(E, 0); }

Expr min_i8 = i8(Int(8).min());
Expr max_i8 = i8(Int(8).max());
Expr min_u8 = u8(UInt(8).min());
Expr max_u8 = u8(UInt(8).max());
Expr min_i16 = i16(Int(16).min());
Expr max_i16 = i16(Int(16).max());
Expr min_u16 = u16(UInt(16).min());
Expr max_u16 = u16(UInt(16).max());
Expr min_i32 = i32(Int(32).min());
Expr max_i32 = i32(Int(32).max());
Expr min_u32 = u32(UInt(32).min());
Expr max_u32 = u32(UInt(32).max());

// The simplifier eliminates max(x, 0) for unsigned x, so make sure
// our patterns reflect the same.
Expr simplified_clamp(Expr x, Expr min, Expr max) {
    if (x.type().is_uint() && is_zero(min)) {
        return Halide::min(x, max);
    } else {
        return clamp(x, min, max);
    }
}

Expr i32c(Expr e) { return i32(simplified_clamp(e, min_i32, max_i32)); }
Expr u32c(Expr e) { return u32(simplified_clamp(e, min_u32, max_u32)); }
Expr i16c(Expr e) { return i16(simplified_clamp(e, min_i16, max_i16)); }
Expr u16c(Expr e) { return u16(simplified_clamp(e, min_u16, max_u16)); }
Expr i8c(Expr e) { return i8(simplified_clamp(e, min_i8, max_i8)); }
Expr u8c(Expr e) { return u8(simplified_clamp(e, min_u8, max_u8)); }

struct Pattern {
    enum Flags {
        InterleaveResult = 1 << 0,  ///< After evaluating the pattern, interleave native vectors of the result.
        SwapOps01 = 1 << 1,  ///< Swap operands 0 and 1 prior to substitution.
        SwapOps12 = 1 << 2,  ///< Swap operands 1 and 2 prior to substitution.
        ExactLog2Op1 = 1 << 3, ///< Replace operand 1 with its log base 2, if the log base 2 is exact.
        ExactLog2Op2 = 1 << 4, ///< Save as above, but for operand 2.

        DeinterleaveOp0 = 1 << 5,  ///< Prior to evaluating the pattern, deinterleave native vectors of operand 0.
        DeinterleaveOp1 = 1 << 6,  ///< Same as above, but for operand 1.
        DeinterleaveOp2 = 1 << 7,
        DeinterleaveOps = DeinterleaveOp0 | DeinterleaveOp1 | DeinterleaveOp2,

        NarrowOp0 = 1 << 10,  ///< Replace operand 0 with its half-width equivalent.
        NarrowOp1 = 1 << 11,  ///< Same as above, but for operand 1.
        NarrowOp2 = 1 << 12,
        NarrowOps = NarrowOp0 | NarrowOp1 | NarrowOp2,

        NarrowUnsignedOp0 = 1 << 15,  ///< Similar to the above, but narrow to an unsigned half width type.
        NarrowUnsignedOp1 = 1 << 16,
        NarrowUnsignedOp2 = 1 << 17,
        NarrowUnsignedOps = NarrowUnsignedOp0 | NarrowUnsignedOp1 | NarrowUnsignedOp2,
    };
    string intrin;        ///< Name of the intrinsic
    Expr pattern;         ///< The pattern to match against
    int flags;

    Pattern() {}
    Pattern(const string &intrin, Expr p, int flags = 0)
        : intrin(intrin), pattern(p), flags(flags) {}
};

Expr wild_u8 = Variable::make(UInt(8), "*");
Expr wild_u16 = Variable::make(UInt(16), "*");
Expr wild_u32 = Variable::make(UInt(32), "*");
Expr wild_u64 = Variable::make(UInt(64), "*");
Expr wild_i8 = Variable::make(Int(8), "*");
Expr wild_i16 = Variable::make(Int(16), "*");
Expr wild_i32 = Variable::make(Int(32), "*");
Expr wild_i64 = Variable::make(Int(64), "*");

Expr wild_u8x = Variable::make(Type(Type::UInt, 8, 0), "*");
Expr wild_u16x = Variable::make(Type(Type::UInt, 16, 0), "*");
Expr wild_u32x = Variable::make(Type(Type::UInt, 32, 0), "*");
Expr wild_u64x = Variable::make(Type(Type::UInt, 64, 0), "*");
Expr wild_i8x = Variable::make(Type(Type::Int, 8, 0), "*");
Expr wild_i16x = Variable::make(Type(Type::Int, 16, 0), "*");
Expr wild_i32x = Variable::make(Type(Type::Int, 32, 0), "*");
Expr wild_i64x = Variable::make(Type(Type::Int, 64, 0), "*");

std::vector<Pattern> casts = {
    // Averaging
    { "halide.hexagon.avg.vub.vub", u8((wild_u16x + wild_u16x)/2), Pattern::NarrowOps },
    { "halide.hexagon.avg.vuh.vuh", u16((wild_u32x + wild_u32x)/2), Pattern::NarrowOps },
    { "halide.hexagon.avg.vh.vh", i16((wild_i32x + wild_i32x)/2), Pattern::NarrowOps },
    { "halide.hexagon.avg.vw.vw", i32((wild_i64x + wild_i64x)/2), Pattern::NarrowOps },

    { "halide.hexagon.avg_rnd.vub.vub", u8((wild_u16x + wild_u16x + 1)/2), Pattern::NarrowOps },
    { "halide.hexagon.avg_rnd.vuh.vuh", u16((wild_u32x + wild_u32x + 1)/2), Pattern::NarrowOps },
    { "halide.hexagon.avg_rnd.vh.vh", i16((wild_i32x + wild_i32x + 1)/2), Pattern::NarrowOps },
    { "halide.hexagon.avg_rnd.vw.vw", i32((wild_i64x + wild_i64x + 1)/2), Pattern::NarrowOps },

    { "halide.hexagon.navg.vub.vub", i8c((wild_i16x - wild_i16x)/2), Pattern::NarrowUnsignedOps },
    { "halide.hexagon.navg.vh.vh", i16c((wild_i32x - wild_i32x)/2), Pattern::NarrowOps },
    { "halide.hexagon.navg.vw.vw", i32c((wild_i64x - wild_i64x)/2), Pattern::NarrowOps },
    // vnavg.uw doesn't exist.

    // Saturating add/subtract
    { "halide.hexagon.satub_add.vub.vub", u8c(wild_u16x + wild_u16x), Pattern::NarrowOps },
    { "halide.hexagon.satuh_add.vuh.vuh", u16c(wild_u32x + wild_u32x), Pattern::NarrowOps },
    { "halide.hexagon.sath_add.vh.vh", i16c(wild_i32x + wild_i32x), Pattern::NarrowOps },
    { "halide.hexagon.satw_add.vw.vw", i32c(wild_i64x + wild_i64x), Pattern::NarrowOps },

    { "halide.hexagon.satub_sub.vub.vub", u8c(wild_i16x - wild_i16x), Pattern::NarrowUnsignedOps },
    { "halide.hexagon.satuh_sub.vuh.vuh", u16c(wild_i32x - wild_i32x), Pattern::NarrowUnsignedOps },
    { "halide.hexagon.sath_sub.vh.vh", i16c(wild_i32x - wild_i32x), Pattern::NarrowOps },
    { "halide.hexagon.satw_sub.vw.vw", i32c(wild_i64x - wild_i64x), Pattern::NarrowOps },

    // Saturating narrowing casts with rounding
    { "halide.hexagon.trunc_satub_rnd.vh", u8c((wild_i32x + 128)/256), Pattern::DeinterleaveOp0 | Pattern::NarrowOp0 },
    { "halide.hexagon.trunc_satb_rnd.vh",  i8c((wild_i32x + 128)/256), Pattern::DeinterleaveOp0 | Pattern::NarrowOp0 },
    { "halide.hexagon.trunc_satuh_rnd.vw", u16c((wild_i64x + 32768)/65536), Pattern::DeinterleaveOp0 | Pattern::NarrowOp0 },
    { "halide.hexagon.trunc_sath_rnd.vw",  i16c((wild_i64x + 32768)/65536), Pattern::DeinterleaveOp0 | Pattern::NarrowOp0 },

    // Saturating narrowing casts
    { "halide.hexagon.trunc_satub_shr.vh.h", u8c(wild_i16x >> wild_i16), Pattern::DeinterleaveOp0 },
    { "halide.hexagon.trunc_satuh_shr.vw.w", u16c(wild_i32x >> wild_i32), Pattern::DeinterleaveOp0 },
    { "halide.hexagon.trunc_sath_shr.vw.w",  i16c(wild_i32x >> wild_i32), Pattern::DeinterleaveOp0 },
    { "halide.hexagon.trunc_satub_shr.vh.h", u8c(wild_i16x/wild_i16), Pattern::DeinterleaveOp0 | Pattern::ExactLog2Op1 },
    { "halide.hexagon.trunc_satuh_shr.vw.w", u16c(wild_i32x/wild_i32), Pattern::DeinterleaveOp0 | Pattern::ExactLog2Op1 },
    { "halide.hexagon.trunc_sath_shr.vw.w",  i16c(wild_i32x/wild_i32), Pattern::DeinterleaveOp0 | Pattern::ExactLog2Op1 },

    // For these narrowing ops, we have the choice of non-interleaving
    // instructions (vpack), or instructions which interleave
    // (vsat). Because we don't know which one we prefer during
    // pattern matching, we match these for now and replace them with
    // the instructions that interleave later if it makes sense.
    { "halide.hexagon.pack_satub.vh", u8c(wild_i16x) },
    { "halide.hexagon.pack_satuh.vw", u16c(wild_i32x) },
    { "halide.hexagon.pack_satb.vh", i8c(wild_i16x) },
    { "halide.hexagon.pack_sath.vw", i16c(wild_i32x) },

    // Narrowing casts
    { "halide.hexagon.trunclo.vh", u8(wild_u16x/256), Pattern::DeinterleaveOp0 },
    { "halide.hexagon.trunclo.vh", u8(wild_i16x/256), Pattern::DeinterleaveOp0 },
    { "halide.hexagon.trunclo.vh", i8(wild_u16x/256), Pattern::DeinterleaveOp0 },
    { "halide.hexagon.trunclo.vh", i8(wild_i16x/256), Pattern::DeinterleaveOp0 },
    { "halide.hexagon.trunclo.vw", u16(wild_u32x/65536), Pattern::DeinterleaveOp0 },
    { "halide.hexagon.trunclo.vw", u16(wild_i32x/65536), Pattern::DeinterleaveOp0 },
    { "halide.hexagon.trunclo.vw", i16(wild_u32x/65536), Pattern::DeinterleaveOp0 },
    { "halide.hexagon.trunclo.vw", i16(wild_i32x/65536), Pattern::DeinterleaveOp0 },
    { "halide.hexagon.trunc_shr.vw.w",  i16(wild_i32x >> wild_i32), Pattern::DeinterleaveOp0 },
    { "halide.hexagon.trunc_shr.vw.w",  i16(wild_i32x/wild_i32), Pattern::DeinterleaveOp0 | Pattern::ExactLog2Op1 },

    // Similar to saturating narrows above, we have the choice of
    // non-interleaving or interleaving instructions.
    { "halide.hexagon.pack.vh", u8(wild_u16x) },
    { "halide.hexagon.pack.vh", u8(wild_i16x) },
    { "halide.hexagon.pack.vh", i8(wild_u16x) },
    { "halide.hexagon.pack.vh", i8(wild_i16x) },
    { "halide.hexagon.pack.vw", u16(wild_u32x) },
    { "halide.hexagon.pack.vw", u16(wild_i32x) },
    { "halide.hexagon.pack.vw", i16(wild_u32x) },
    { "halide.hexagon.pack.vw", i16(wild_i32x) },

    // Widening casts
    { "halide.hexagon.zxt.vub", u16(wild_u8x), Pattern::InterleaveResult },
    { "halide.hexagon.zxt.vub", i16(wild_u8x), Pattern::InterleaveResult },
    { "halide.hexagon.zxt.vuh", u32(wild_u16x), Pattern::InterleaveResult },
    { "halide.hexagon.zxt.vuh", i32(wild_u16x), Pattern::InterleaveResult },
    { "halide.hexagon.sxt.vb", u16(wild_i8x), Pattern::InterleaveResult },
    { "halide.hexagon.sxt.vb", i16(wild_i8x), Pattern::InterleaveResult },
    { "halide.hexagon.sxt.vh", u32(wild_i16x), Pattern::InterleaveResult },
    { "halide.hexagon.sxt.vh", i32(wild_i16x), Pattern::InterleaveResult },
};

std::vector<Pattern> muls = {
    // Vector by scalar widening multiplies.
    { "halide.hexagon.mpy.vub.ub", wild_u16x*bc(wild_u16), Pattern::InterleaveResult | Pattern::NarrowOps },
    { "halide.hexagon.mpy.vub.b",  wild_i16x*bc(wild_i16), Pattern::InterleaveResult | Pattern::NarrowUnsignedOp0 | Pattern::NarrowOp1 },
    { "halide.hexagon.mpy.vuh.uh", wild_u32x*bc(wild_u32), Pattern::InterleaveResult | Pattern::NarrowOps },
    { "halide.hexagon.mpy.vh.h",   wild_i32x*bc(wild_i32), Pattern::InterleaveResult | Pattern::NarrowOps },

    // Widening multiplication
    { "halide.hexagon.mpy.vub.vub", wild_u16x*wild_u16x, Pattern::InterleaveResult | Pattern::NarrowOps },
    { "halide.hexagon.mpy.vuh.vuh", wild_u32x*wild_u32x, Pattern::InterleaveResult | Pattern::NarrowOps },
    { "halide.hexagon.mpy.vb.vb",   wild_i16x*wild_i16x, Pattern::InterleaveResult | Pattern::NarrowOps },
    { "halide.hexagon.mpy.vh.vh",   wild_i32x*wild_i32x, Pattern::InterleaveResult | Pattern::NarrowOps },

    { "halide.hexagon.mpy.vub.vb",  wild_i16x*wild_i16x, Pattern::InterleaveResult | Pattern::NarrowUnsignedOp0 | Pattern::NarrowOp1 },
    { "halide.hexagon.mpy.vh.vuh",  wild_i32x*wild_i32x, Pattern::InterleaveResult | Pattern::NarrowOp0 | Pattern::NarrowUnsignedOp1 },
};

// Many of the following patterns are accumulating widening
// operations, which need to both deinterleave the accumulator, and
// reinterleave the result.
const int ReinterleaveOp0 = Pattern::InterleaveResult | Pattern::DeinterleaveOp0;

std::vector<Pattern> adds = {
    // Shift-accumulates.
    { "halide.hexagon.add_shr.vw.vw.w", wild_i32x + (wild_i32x >> bc(wild_i32)) },
    { "halide.hexagon.add_shl.vw.vw.w", wild_i32x + (wild_i32x << bc(wild_i32)) },
    { "halide.hexagon.add_shl.vw.vw.w", wild_u32x + (wild_u32x << bc(wild_u32)) },
    { "halide.hexagon.add_shr.vw.vw.w", wild_i32x + (wild_i32x/bc(wild_i32)), Pattern::ExactLog2Op2 },
    { "halide.hexagon.add_shl.vw.vw.w", wild_i32x + (wild_i32x*bc(wild_i32)), Pattern::ExactLog2Op2 },
    { "halide.hexagon.add_shl.vw.vw.w", wild_u32x + (wild_u32x*bc(wild_u32)), Pattern::ExactLog2Op2 },
    { "halide.hexagon.add_shl.vw.vw.w", wild_i32x + (bc(wild_i32)*wild_i32x), Pattern::ExactLog2Op1 | Pattern::SwapOps12 },
    { "halide.hexagon.add_shl.vw.vw.w", wild_u32x + (bc(wild_u32)*wild_u32x), Pattern::ExactLog2Op1 | Pattern::SwapOps12 },

    // Widening multiply-accumulates with a scalar.
    { "halide.hexagon.add_mpy.vuh.vub.ub", wild_u16x + wild_u16x*bc(wild_u16), ReinterleaveOp0 | Pattern::NarrowOp1 | Pattern::NarrowOp2 },
    { "halide.hexagon.add_mpy.vh.vub.b",   wild_i16x + wild_i16x*bc(wild_i16), ReinterleaveOp0 | Pattern::NarrowUnsignedOp1 | Pattern::NarrowOp2 },
    { "halide.hexagon.add_mpy.vuw.vuh.uh", wild_u32x + wild_u32x*bc(wild_u32), ReinterleaveOp0 | Pattern::NarrowOp1 | Pattern::NarrowOp2 },
    { "halide.hexagon.add_mpy.vuh.vub.ub", wild_u16x + bc(wild_u16)*wild_u16x, ReinterleaveOp0 | Pattern::NarrowOp1 | Pattern::NarrowOp2 | Pattern::SwapOps12 },
    { "halide.hexagon.add_mpy.vh.vub.b",   wild_i16x + bc(wild_i16)*wild_i16x, ReinterleaveOp0 | Pattern::NarrowOp1 | Pattern::NarrowUnsignedOp2 | Pattern::SwapOps12 },
    { "halide.hexagon.add_mpy.vuw.vuh.uh", wild_u32x + bc(wild_u32)*wild_u32x, ReinterleaveOp0 | Pattern::NarrowOp1 | Pattern::NarrowOp2 | Pattern::SwapOps12 },

    // These patterns aren't exactly right because the instruction
    // saturates the result. However, this is really the instruction
    // that we want to use in most cases, and we can exploit the fact
    // that 32 bit signed arithmetic overflow is undefined to argue
    // that these patterns are not completely incorrect.
    { "halide.hexagon.satw_add_mpy.vw.vh.h", wild_i32x + wild_i32x*bc(wild_i32), ReinterleaveOp0 | Pattern::NarrowOp1 | Pattern::NarrowOp2 },
    { "halide.hexagon.satw_add_mpy.vw.vh.h", wild_i32x + bc(wild_i32)*wild_i32x, ReinterleaveOp0 | Pattern::NarrowOp1 | Pattern::NarrowOp2 | Pattern::SwapOps12 },

    // Non-widening multiply-accumulates with a scalar.
    { "halide.hexagon.add_mul.vh.vh.b", wild_i16x + wild_i16x*bc(wild_i16), Pattern::NarrowOp2 },
    { "halide.hexagon.add_mul.vw.vw.h", wild_i32x + wild_i32x*bc(wild_i32), Pattern::NarrowOp2 },
    { "halide.hexagon.add_mul.vh.vh.b", wild_i16x + bc(wild_i16)*wild_i16x, Pattern::NarrowOp1 | Pattern::SwapOps12 },
    { "halide.hexagon.add_mul.vw.vw.h", wild_i32x + bc(wild_i32)*wild_i32x, Pattern::NarrowOp1 | Pattern::SwapOps12 },
    // TODO: There's also a add_mul.vw.vw.b

    // Widening multiply-accumulates.
    { "halide.hexagon.add_mpy.vuh.vub.vub", wild_u16x + wild_u16x*wild_u16x, ReinterleaveOp0 | Pattern::NarrowOp1 | Pattern::NarrowOp2 },
    { "halide.hexagon.add_mpy.vuw.vuh.vuh", wild_u32x + wild_u32x*wild_u32x, ReinterleaveOp0 | Pattern::NarrowOp1 | Pattern::NarrowOp2 },
    { "halide.hexagon.add_mpy.vh.vb.vb",    wild_i16x + wild_i16x*wild_i16x, ReinterleaveOp0 | Pattern::NarrowOp1 | Pattern::NarrowOp2 },
    { "halide.hexagon.add_mpy.vw.vh.vh",    wild_i32x + wild_i32x*wild_i32x, ReinterleaveOp0 | Pattern::NarrowOp1 | Pattern::NarrowOp2 },

    { "halide.hexagon.add_mpy.vh.vub.vb",   wild_i16x + wild_i16x*wild_i16x, ReinterleaveOp0 | Pattern::NarrowUnsignedOp1 | Pattern::NarrowOp2 },
    { "halide.hexagon.add_mpy.vw.vh.vuh",   wild_i32x + wild_i32x*wild_i32x, ReinterleaveOp0 | Pattern::NarrowOp1 | Pattern::NarrowUnsignedOp2 },
    { "halide.hexagon.add_mpy.vh.vub.vb",   wild_i16x + wild_i16x*wild_i16x, ReinterleaveOp0 | Pattern::NarrowOp1 | Pattern::NarrowUnsignedOp2 | Pattern::SwapOps12 },
    { "halide.hexagon.add_mpy.vw.vh.vuh",   wild_i32x + wild_i32x*wild_i32x, ReinterleaveOp0 | Pattern::NarrowUnsignedOp1 | Pattern::NarrowOp2 | Pattern::SwapOps12 },

    // This pattern is very general, so it must come last.
    { "halide.hexagon.add_mul.vh.vh.vh", wild_i16x + wild_i16x*wild_i16x },
};

Expr apply_patterns(Expr x, const std::vector<Pattern> &patterns, IRMutator *op_mutator) {
    std::vector<Expr> matches;
    for (const Pattern &p : patterns) {
        if (expr_match(p.pattern, x, matches)) {
            // The Pattern::Narrow*Op* flags are ordered such that
            // the operand corresponds to the bit (with operand 0
            // corresponding to the least significant bit), so we
            // can check for them all in a loop.
            bool is_match = true;
            for (size_t i = 0; i < matches.size() && is_match; i++) {
                Type t = matches[i].type();
                Type target_t = t.with_bits(t.bits()/2);
                if (p.flags & (Pattern::NarrowOp0 << i)) {
                    matches[i] = lossless_cast(target_t, matches[i]);
                } else if (p.flags & (Pattern::NarrowUnsignedOp0 << i)) {
                    matches[i] = lossless_cast(target_t.with_code(Type::UInt), matches[i]);
                }
                if (!matches[i].defined()) is_match = false;
            }
            if (!is_match) continue;

            for (size_t i = 1; i < matches.size() && is_match; i++) {
                // This flag is mainly to capture shifts. When the
                // operand of a div or mul is a power of 2, we can use
                // a shift instead.
                if (p.flags & (Pattern::ExactLog2Op1 << (i - 1))) {
                    int pow;
                    if (is_const_power_of_two_integer(matches[i], &pow)) {
                        matches[i] = cast(matches[i].type().with_lanes(1), pow);
                    } else {
                        is_match = false;
                    }
                }
            }
            if (!is_match) continue;

            for (size_t i = 0; i < matches.size(); i++) {
                if (p.flags & (Pattern::DeinterleaveOp0 << i)) {
                    internal_assert(matches[i].type().is_vector());
                    matches[i] = native_deinterleave(matches[i]);
                }
            }
            if (p.flags & Pattern::SwapOps01) {
                internal_assert(matches.size() >= 2);
                std::swap(matches[0], matches[1]);
            }
            if (p.flags & Pattern::SwapOps12) {
                internal_assert(matches.size() >= 3);
                std::swap(matches[1], matches[2]);
            }
            // Mutate the operands with the given mutator.
            for (Expr &op : matches) {
                op = op_mutator->mutate(op);
            }
            x = Call::make(x.type(), p.intrin, matches, Call::PureExtern);
            if (p.flags & Pattern::InterleaveResult) {
                // The pattern wants us to interleave the result.
                x = native_interleave(x);
            }
            return x;
        }
    }
    return x;
}

Expr lossless_negate(Expr x) {
    const Mul *m = x.as<Mul>();
    if (m) {
        Expr a = lossless_negate(m->a);
        if (a.defined()) {
            return Mul::make(a, m->b);
        }
        Expr b = lossless_negate(m->b);
        if (b.defined()) {
            return Mul::make(m->a, b);
        }
    }
    if (is_negative_negatable_const(x) || is_positive_const(x)) {
        return simplify(-x);
    }
    return Expr();
}

// Perform peephole optimizations on the IR, adding appropriate
// interleave and deinterleave calls.
class OptimizePatterns : public IRMutator {
private:
    using IRMutator::visit;

    template <typename T>
    void visit_commutative_op(const T *op, const vector<Pattern> &patterns) {
        if (op->type.is_vector()) {
            expr = apply_patterns(op, patterns, this);
            if (!expr.same_as(op)) return;

            // Try commuting the op
            Expr commuted = T::make(op->b, op->a);
            expr = apply_patterns(commuted, patterns, this);
            if (!expr.same_as(commuted)) return;
        }
        IRMutator::visit(op);
    }

    void visit(const Mul *op) { visit_commutative_op(op, muls); }
    void visit(const Add *op) { visit_commutative_op(op, adds); }

    void visit(const Sub *op) {
        if (op->type.is_vector()) {
            // Try negating op->b, and using an add pattern if successful.
            Expr neg_b = lossless_negate(op->b);
            if (neg_b.defined()) {
                Expr add = Add::make(op->a, neg_b);
                expr = apply_patterns(add, adds, this);
                if (!expr.same_as(add)) return;

                add = Add::make(neg_b, op->a);
                expr = apply_patterns(add, adds, this);
                if (!expr.same_as(add)) return;
            }
        }
        IRMutator::visit(op);
    }

    void visit(const Max *op) {
        IRMutator::visit(op);

        if (op->type.is_vector()) {
            // This pattern is weird (wo operands must match, result
            // needs 1 added) and we're unlikely to need another
            // pattern for max, so just match it directly.
            static std::pair<std::string, Expr> cl[] = {
                { "halide.hexagon.cls.vh", max(count_leading_zeros(wild_i16x), count_leading_zeros(~wild_i16x)) },
                { "halide.hexagon.cls.vw", max(count_leading_zeros(wild_i32x), count_leading_zeros(~wild_i32x)) },
            };
            std::vector<Expr> matches;
            for (const auto &i : cl) {
                if (expr_match(i.second, expr, matches) && equal(matches[0], matches[1])) {
                    expr = Call::make(op->type, i.first, {matches[0]}, Call::PureExtern) + 1;
                    return;
                }
            }
        }
    }

    void visit(const Cast *op) {
        // To hit more of the patterns we want, rewrite "double casts"
        // as two stage casts. This also avoids letting vector casts
        // fall through to LLVM, which will generate large unoptimized
        // shuffles.
        static vector<pair<Expr, Expr>> cast_rewrites = {
            // Saturating narrowing
            { u8c(wild_u32x), u8c(u16c(wild_u32x)) },
            { u8c(wild_i32x), u8c(i16c(wild_i32x)) },
            { i8c(wild_u32x), i8c(u16c(wild_u32x)) },
            { i8c(wild_i32x), i8c(i16c(wild_i32x)) },

            // Narrowing
            { u8(wild_u32x), u8(u16(wild_u32x)) },
            { u8(wild_i32x), u8(i16(wild_i32x)) },
            { i8(wild_u32x), i8(u16(wild_u32x)) },
            { i8(wild_i32x), i8(i16(wild_i32x)) },

            // Widening
            { u32(wild_u8x), u32(u16(wild_u8x)) },
            { u32(wild_i8x), u32(i16(wild_i8x)) },
            { i32(wild_u8x), i32(u16(wild_u8x)) },
            { i32(wild_i8x), i32(i16(wild_i8x)) },
        };

        if (op->type.is_vector()) {
            Expr cast = op;

            expr = apply_patterns(cast, casts, this);
            if (!expr.same_as(cast)) return;

            // If we didn't find a pattern, try using one of the
            // rewrites above.
            std::vector<Expr> matches;
            for (auto i : cast_rewrites) {
                if (expr_match(i.first, cast, matches)) {
                    Expr replacement = with_lanes(i.second, op->type.lanes());
                    expr = substitute("*", matches[0], replacement);
                    expr = mutate(expr);
                    return;
                }
            }
        }
        IRMutator::visit(op);
    }

public:
    OptimizePatterns() {}
};

// Attempt to cancel out redundant interleave/deinterleave pairs. The
// basic strategy is to push interleavings toward the end of the
// program, using the fact that interleaves can pass through pointwise
// IR operations. When an interleave collides with a deinterleave,
// they cancel out.
class EliminateInterleaves : public IRMutator {
private:
    Scope<bool> vars;

    // Check if x is an expression that is either an interleave, or
    // can pretend to be one (is a scalar or a broadcast).
    bool yields_interleave(Expr x) {
        if (is_native_interleave(x)) {
            return true;
        } else if (x.type().is_scalar() || x.as<Broadcast>()) {
            return true;
        }
        const Variable *var = x.as<Variable>();
        if (var && vars.contains(var->name + ".deinterleaved")) {
            return true;
        }
        return false;
    }

    // Check that at least one of exprs is an interleave, and that all
    // of the exprs can yield an interleave.
    bool yields_removable_interleave(const std::vector<Expr> &exprs) {
        bool any_is_interleave = false;
        for (const Expr &i : exprs) {
            if (is_native_interleave(i)) {
                any_is_interleave = true;
            } else if (!yields_interleave(i)) {
                return false;
            }
        }
        return any_is_interleave;
    }

    // Asserting that x is an expression that can yield an interleave
    // operation, return the expression being interleaved.
    Expr remove_interleave(Expr x) {
        if (is_native_interleave(x)) {
            return x.as<Call>()->args[0];
        } else if (x.type().is_scalar() || x.as<Broadcast>()) {
            return x;
        }
        const Variable *var = x.as<Variable>();
        if (var) {
            internal_assert(vars.contains(var->name + ".deinterleaved"));
            return Variable::make(var->type, var->name + ".deinterleaved");
        }
        internal_error << "Expression '" << x << "' does not yield an interleave.\n";
        return x;
    }

    template <typename T>
    void visit_binary(const T* op) {
        Expr a = mutate(op->a);
        Expr b = mutate(op->b);
        // We only want to pull out an interleave if at least one of
        // the operands is an actual interleave.
        if (yields_removable_interleave({a, b})) {
            a = remove_interleave(a);
            b = remove_interleave(b);
            expr = native_interleave(T::make(a, b));
        } else if (!a.same_as(op->a) || !b.same_as(op->b)) {
            expr = T::make(a, b);
        } else {
            expr = op;
        }
    }

    void visit(const Add *op) { visit_binary(op); }
    void visit(const Sub *op) { visit_binary(op); }
    void visit(const Mul *op) { visit_binary(op); }
    void visit(const Div *op) { visit_binary(op); }
    void visit(const Mod *op) { visit_binary(op); }
    void visit(const Min *op) { visit_binary(op); }
    void visit(const Max *op) { visit_binary(op); }
    void visit(const EQ *op) { visit_binary(op); }
    void visit(const NE *op) { visit_binary(op); }
    void visit(const LT *op) { visit_binary(op); }
    void visit(const LE *op) { visit_binary(op); }
    void visit(const GT *op) { visit_binary(op); }
    void visit(const GE *op) { visit_binary(op); }
    void visit(const And *op) { visit_binary(op); }
    void visit(const Or *op) { visit_binary(op); }

    void visit(const Not *op) {
        Expr a = mutate(op->a);
        if (is_native_interleave(a)) {
            a = remove_interleave(a);
            expr = native_interleave(Not::make(a));
        } else if (!a.same_as(op->a)) {
            expr = Not::make(a);
        } else {
            expr = op;
        }
    }

    void visit(const Select *op) {
        Expr cond = mutate(op->condition);
        Expr true_value = mutate(op->true_value);
        Expr false_value = mutate(op->false_value);
        if (yields_removable_interleave({cond, true_value, false_value})) {
            cond = remove_interleave(cond);
            true_value = remove_interleave(true_value);
            false_value = remove_interleave(false_value);
            expr = native_interleave(Select::make(cond, true_value, false_value));
        } else if (!cond.same_as(op->condition) ||
                   !true_value.same_as(op->true_value) ||
                   !false_value.same_as(op->false_value)) {
            expr = Select::make(cond, true_value, false_value);
        } else {
            expr = op;
        }
    }

    // Make overloads of stmt/expr uses var so we can use it in a template.
    static bool uses_var(Stmt s, const std::string &var) {
        return stmt_uses_var(s, var);
    }
    static bool uses_var(Expr e, const std::string &var) {
        return expr_uses_var(e, var);
    }

    template <typename NodeType, typename LetType>
    void visit_let(NodeType &result, const LetType *op) {
        Expr value = mutate(op->value);
        string deinterleaved_name = op->name + ".deinterleaved";
        NodeType body;
        if (is_native_interleave(value)) {
            // We can provide a deinterleaved version of this let value.
            vars.push(deinterleaved_name, true);
            body = mutate(op->body);
            vars.pop(deinterleaved_name);
        } else {
            body = mutate(op->body);
        }
        if (value.same_as(op->value) && body.same_as(op->body)) {
            result = op;
        } else if (body.same_as(op->body)) {
            // If the body didn't change, we must not have used the deinterleaved value.
            result = LetType::make(op->name, value, body);
        } else {
            // We need to rewrap the body with new lets.
            result = body;
            bool deinterleaved_used = uses_var(result, deinterleaved_name);
            bool interleaved_used = uses_var(result, op->name);
            if (deinterleaved_used && interleaved_used) {
                // The body uses both the interleaved and
                // deinterleaved version of this let. Generate both
                // lets, using the deinterleaved one to generate the
                // interleaved one.
                Expr deinterleaved = remove_interleave(value);
                Expr deinterleaved_var = Variable::make(deinterleaved.type(), deinterleaved_name);
                result = LetType::make(op->name, native_interleave(deinterleaved_var), result);
                result = LetType::make(deinterleaved_name, deinterleaved, result);
            } else if (deinterleaved_used) {
                // Only the deinterleaved value is used, we can eliminate the interleave.
                result = LetType::make(deinterleaved_name, remove_interleave(value), result);
            } else if (interleaved_used) {
                // Only the original value is used, regenerate the let.
                result = LetType::make(op->name, value, result);
            } else {
                // The let must have been dead.
                internal_assert(!uses_var(op->body, op->name)) << "EliminateInterleaves eliminated a non-dead let.\n";
            }
        }
    }

    void visit(const Let *op) { visit_let(expr, op); }
    void visit(const LetStmt *op) { visit_let(stmt, op); }

    void visit(const Cast *op) {
        if (op->type.bits() == op->value.type().bits()) {
            // We can move interleaves through casts of the same size.
            Expr value = mutate(op->value);
            if (is_native_interleave(value)) {
                value = remove_interleave(value);
                expr = native_interleave(Cast::make(op->type, value));
            } else if (!value.same_as(op->value)) {
                expr = Cast::make(op->type, value);
            } else {
                expr = op;
            }
        } else {
            IRMutator::visit(op);
        }
    }

    bool is_interleavable(const Call *op) {
        // These calls can have interleaves moved from operands to the
        // result.
        static set<string> interleavable = {
            Call::bitwise_and,
            Call::bitwise_not,
            Call::bitwise_xor,
            Call::bitwise_or,
            Call::shift_left,
            Call::shift_right,
            Call::abs,
            Call::absd,
        };
        if (interleavable.count(op->name) != 0) return true;

        // These calls cannot. Furthermore, these calls have the same
        // return type as the arguments, which means our test below
        // will be inaccurate.
        static set<string> not_interleavable = {
            "halide.hexagon.interleave.vb",
            "halide.hexagon.interleave.vh",
            "halide.hexagon.interleave.vw",
            "halide.hexagon.deinterleave.vb",
            "halide.hexagon.deinterleave.vh",
            "halide.hexagon.deinterleave.vw",
        };
        if (not_interleavable.count(op->name) != 0) return false;

        if (starts_with(op->name, "halide.hexagon.")) {
            // We assume that any hexagon intrinsic is interleavable
            // as long as all of the vector operands have the same
            // number of lanes and lane width as the return type.
            for (Expr i : op->args) {
                if (i.type().is_scalar()) continue;
                if (i.type().bits() != op->type.bits() || i.type().lanes() != op->type.lanes()) {
                    return false;
                }
            }
        }
        return true;
    }

    void visit(const Call *op) {
        vector<Expr> args(op->args);

        // mutate all the args.
        bool changed = false;
        for (Expr &i : args) {
            Expr new_i = mutate(i);
            changed = changed || !new_i.same_as(i);
            i = new_i;
        }

        // For a few operations, we have a choice of several
        // instructions, an interleaving or a non-inerleaving
        // variant. We handle this by generating the instruction that
        // does not deinterleave, and then opportunistically select
        // the interleaving alternative when we can cancel out to the
        // interleave.
        struct DeinterleavingAlternative {
            string name;
            vector<Expr> extra_args;
        };
        static std::map<string, DeinterleavingAlternative> deinterleaving_alts = {
            { "halide.hexagon.pack.vh", { "halide.hexagon.trunc.vh" } },
            { "halide.hexagon.pack.vw", { "halide.hexagon.trunc.vw" } },
            { "halide.hexagon.pack_satub.vh", { "halide.hexagon.trunc_satub.vh" } },
            { "halide.hexagon.pack_sath.vw", { "halide.hexagon.trunc_sath.vw" } },
            // For this one, we don't have a simple alternative. But,
            // we have a shift-saturate-narrow that we can use with a
            // shift of 0.
            { "halide.hexagon.pack_satuh.vw", { "halide.hexagon.trunc_satuh_shr.vw.w", { 0 } } },
        };

        if (is_native_deinterleave(op) && yields_interleave(args[0])) {
            // This is a deinterleave of an interleave! Remove them both.
            expr = remove_interleave(args[0]);
        } else if (is_interleavable(op) && yields_removable_interleave(args)) {
            // All the arguments yield interleaves (and one of
            // them is an interleave), create a new call with the
            // interleave removed from the arguments.
            for (Expr &i : args) {
                i = remove_interleave(i);
            }
            expr = Call::make(op->type, op->name, args, op->call_type,
                              op->func, op->value_index, op->image, op->param);
            // Add the interleave back to the result of the call.
            expr = native_interleave(expr);
        } else if (deinterleaving_alts.find(op->name) != deinterleaving_alts.end() &&
                   yields_removable_interleave(args)) {
            // This call has a deinterleaving alternative, and the
            // arguments are interleaved, so we should use the
            // alternative instead.
            const DeinterleavingAlternative &alt = deinterleaving_alts[op->name];
            for (Expr &i : args) {
                i = remove_interleave(i);
            }
            for (Expr i : alt.extra_args) {
                args.push_back(i);
            }
            expr = Call::make(op->type, alt.name, args, op->call_type);
        } else if (changed) {
            expr = Call::make(op->type, op->name, args, op->call_type,
                              op->func, op->value_index, op->image, op->param);
        } else {
            expr = op;
        }
    }

    using IRMutator::visit;
};

// Find a conservative upper bound of an expression.
class UpperBound : public IRMutator {
    using IRMutator::visit;

    void visit(const Sub *op) {
        Expr a = mutate(op->a);
        Expr b = mutate(op->b);

        const Min *min_a = a.as<Min>();
        const Min *min_b = b.as<Min>();
        const Max *max_a = a.as<Max>();
        const Max *max_b = b.as<Max>();

        if (max_a && max_b) {
            if (equal(max_a->b, max_b->b)) {
                expr = mutate(simplify(max_a->a - max_b->a));
                return;
            }
        }

        if (min_a && min_b) {
            if (equal(min_a->b, min_b->b)) {
                expr = mutate(simplify(min_a->a - min_b->a));
                return;
            }
        }

        if (!a.same_as(op->a) || !b.same_as(op->b)) {
            expr = Sub::make(a, b);
        } else {
            expr = op;
        }
    }
};

Expr upper_bound(Expr x) {
    return simplify(UpperBound().mutate(x));
}

// Replace indirect loads with dynamic_shuffle intrinsics where
// possible.
class OptimizeShuffles : public IRMutator {
    Scope<Interval> bounds;

    using IRMutator::visit;

    template <typename T>
    void visit_let(const T *op) {
        // We only care about vector lets.
        if (op->value.type().is_vector()) {
            bounds.push(op->name, bounds_of_expr_in_scope(op->value, bounds));
        }
        IRMutator::visit(op);
        if (op->value.type().is_vector()) {
            bounds.pop(op->name);
        }
    }

    void visit(const Let *op) { visit_let(op); }
    void visit(const LetStmt *op) { visit_let(op); }

    void visit(const Load *op) {
        if (!op->type.is_vector() || op->index.as<Ramp>()) {
            // Don't handle scalar or simple vector loads.
            IRMutator::visit(op);
            return;
        }

        Expr index = mutate(op->index);
        Interval index_bounds = bounds_of_expr_in_scope(index, bounds);
        Expr index_span = index_bounds.max - index_bounds.min;
        index_span = common_subexpression_elimination(index_span);
        index_span = simplify(index_span);
        index_span = upper_bound(index_span);

        if (is_one(simplify(index_span < 256))) {
            // This is a lookup within an up to 256 element array. We
            // can use dynamic_shuffle for this.
            int const_extent = as_const_int(index_span) ? *as_const_int(index_span) + 1 : 256;
            Expr base = simplify(index_bounds.min);

            // Load all of the possible indices loaded from the
            // LUT. Note that for clamped ramps, this loads up to 1
            // vector past the max. CodeGen_Hexagon::allocation_padding
            // returns a native vector size to account for this.
            Expr lut = Load::make(op->type.with_lanes(const_extent), op->name,
                                  Ramp::make(base, 1, const_extent),
                                  op->image, op->param);

            // We know the size of the LUT is not more than 256, so we
            // can safely cast the index to 8 bit, which
            // dynamic_shuffle requires.
            index = simplify(cast(UInt(8).with_lanes(op->type.lanes()), index - base));

            expr = Call::make(op->type, "dynamic_shuffle", {lut, index, 0, const_extent}, Call::PureIntrinsic);
        } else if (!index.same_as(op->index)) {
            expr = Load::make(op->type, op->name, index, op->image, op->param);
        } else {
            expr = op;
        }
    }
};

}  // namespace

Stmt optimize_hexagon_shuffles(Stmt s) {
    // Replace indirect and other complicated loads with
    // dynamic_shuffle (vlut) calls.
    return OptimizeShuffles().mutate(s);
}

Stmt optimize_hexagon_instructions(Stmt s) {
    // Peephole optimize for Hexagon instructions. These can generate
    // interleaves and deinterleaves alongside the HVX intrinsics.
    s = OptimizePatterns().mutate(s);

    // Try to eliminate any redundant interleave/deinterleave pairs.
    s = EliminateInterleaves().mutate(s);

    // TODO: If all of the stores to a buffer are interleaved, and all
    // of the loads are immediately deinterleaved, then we can remove
    // all of the interleave/deinterleaves, and just let the storage
    // be deinterleaved.

    return s;
}

}  // namespace Internal
}  // namespace Halide