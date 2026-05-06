#include "castLower.hpp"
#include <iostream>
#include "castLexer.h"
#include <mlir/IR/Builders.h>
#include <mlir/IR/BuiltinOps.h>
#include <mlir/IR/Location.h>
#include <mlir/IR/OwningOpRef.h>
#include "mlir/IR/MLIRContext.h"

#include <circt/Dialect/HW/HWTypes.h>
#include <circt/Dialect/HW/HWOps.h>
#include <circt/Dialect/ESI/ESIOps.h>
#include <circt/Dialect/ESI/ESITypes.h>
#include <circt/Dialect/FSM/FSMOps.h>
#include <circt/Dialect/SV/SVOps.h>
#include <circt/Dialect/SV/SVTypes.h>
#include <circt/Dialect/SV/SVDialect.h>
#include <circt/Dialect/SV/SVAttributes.h>
#include <circt/Dialect/HW/HWDialect.h>
#include <circt/Dialect/Comb/CombDialect.h>
#include <circt/Dialect/Comb/CombOps.h>
#include <circt/Dialect/Seq/SeqDialect.h>
#include <circt/Dialect/Seq/SeqOps.h>
#include "llvm/Support/raw_ostream.h"

#include <print>

using namespace std;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

// Extend or truncate `v` to match `t`. If types already agree, returns v.
// Comb ops require signless integers, so signed/unsigned MLIR IntegerTypes
// are first cast to signless of the same width via UnrealizedConversionCastOp.
mlir::Value LowerVisitor::coerce(mlir::Value v, mlir::Type t, mlir::Location loc)
{
    if (!v || !t) return v;
    auto srcInt = mlir::dyn_cast<mlir::IntegerType>(v.getType());
    auto dstInt = mlir::dyn_cast<mlir::IntegerType>(t);
    if (!srcInt || !dstInt) return v;

    // Drop signedness on the source so comb ops accept it.
    if (!srcInt.isSignless())
    {
        mlir::Type signless = mlir::IntegerType::get(&this->ctx, srcInt.getWidth());
        v = mlir::UnrealizedConversionCastOp::create(builder, loc, signless, mlir::ValueRange{v}).getResult(0);
        srcInt = mlir::cast<mlir::IntegerType>(v.getType());
    }
    mlir::Type targetSignless = dstInt.isSignless()
        ? t
        : mlir::IntegerType::get(&this->ctx, dstInt.getWidth());

    if (v.getType() == targetSignless) return v;
    if (srcInt.getWidth() == dstInt.getWidth()) return v;
    if (srcInt.getWidth() < dstInt.getWidth())
    {
        unsigned padW = dstInt.getWidth() - srcInt.getWidth();
        mlir::Value zero = circt::hw::ConstantOp::create(
            builder, loc, builder.getIntegerType(padW), 0);
        return circt::comb::ConcatOp::create(builder, loc, mlir::ValueRange{zero, v});
    }
    return circt::comb::ExtractOp::create(builder, loc, targetSignless, v, 0);
}

// Read the live value of a shared variable (the register's current output).
mlir::Value LowerVisitor::readVar(const std::string &name)
{
    auto &mvars = this->varRegs[this->currentModuleName];
    auto it = mvars.find(name);
    if (it == mvars.end()) return nullptr;
    return it->second.getResult();
}

// Append a conditional update to the variable's next-value chain.
//   varNext = mux(cond, newVal, varNext_prev)
// At module finalize, varNext is wired into the register input as operand 0.
void LowerVisitor::writeVar(const std::string &name, mlir::Value newVal, mlir::Value cond)
{
    auto &mvars = this->varRegs[this->currentModuleName];
    auto it = mvars.find(name);
    if (it == mvars.end()) return;
    mlir::Location loc = builder.getUnknownLoc();
    mlir::Value prev = this->varNext[this->currentModuleName][name];
    mlir::Value coerced = coerce(newVal, prev.getType(), loc);
    mlir::Value next = circt::comb::MuxOp::create(builder, loc, cond, coerced, prev);
    this->varNext[this->currentModuleName][name] = next;
}

// ─────────────────────────────────────────────────────────────────────────────
// Top-level declarations
// ─────────────────────────────────────────────────────────────────────────────

std::any LowerVisitor::visitDecl_enum(castParser::Decl_enumContext *antlrCtx)
{
    // enum Name = { A, B, C };  →  fold each member into an i8 encoding table.
    auto idents = antlrCtx->ident();
    if (idents.size() < 2) return std::any();
    // First ident is the enum name; the rest are members (numbered 0..N-1).
    unsigned width = 8;
    for (size_t i = 1; i < idents.size(); ++i)
    {
        std::string name = idents[i]->getText();
        this->enumValues[name] = {static_cast<uint64_t>(i - 1), width};
    }
    return std::any();
}

std::any LowerVisitor::visitDecl_machine(castParser::Decl_machineContext *antlrCtx)
{
    mlir::OpBuilder &b = this->builder;
    mlir::Location loc = b.getUnknownLoc();
    b.setInsertionPointToStart(this->topModule.getBody());

    // ── Pass 1: collect interface ports only, so we can declare the module ───
    this->currentPorts.clear();
    std::string moduleName = antlrCtx->func_name()->getText();
    this->currentModuleName = moduleName;
    this->variables[moduleName];
    this->portValues[moduleName];
    this->portDirs[moduleName];
    this->varRegs[moduleName];
    this->varNext[moduleName];
    this->stateIds[moduleName];
    this->stateActive[moduleName];
    this->outputDataNext[moduleName];
    this->outputValidNext[moduleName];

    // Walk only the interface block on this pass.
    if (auto blk = antlrCtx->machine_block())
        if (auto iface = blk->decl_interface())
            visit(iface);

    // Add implicit clk + rst ports (inserted at the front, in order rst, clk).
    circt::hw::PortInfo rstPort;
    rstPort.name = b.getStringAttr("rst");
    rstPort.type = b.getI1Type();
    rstPort.dir = circt::hw::ModulePort::Direction::Input;
    this->currentPorts.insert(this->currentPorts.begin(), rstPort);

    circt::hw::PortInfo clkPort;
    clkPort.name = b.getStringAttr("clk");
    clkPort.type = circt::seq::ClockType::get(&this->ctx);
    clkPort.dir = circt::hw::ModulePort::Direction::Input;
    this->currentPorts.insert(this->currentPorts.begin(), clkPort);

    circt::hw::ModulePortInfo portInfo(this->currentPorts);
    mlir::StringAttr modNameAttr = b.getStringAttr(moduleName);
    circt::hw::HWModuleOp hwModule = circt::hw::HWModuleOp::create(b, loc, modNameAttr, portInfo);
    this->modules[moduleName] = hwModule;

    mlir::Block *body = &hwModule.getBody().front();
    for (auto &op : llvm::make_early_inc_range(body->getOperations()))
        if (mlir::isa<circt::hw::OutputOp>(op)) op.erase();

    // Block arguments hold ONLY input ports (output ports come back via hw.output).
    // Order is: clk(0), rst(1), then user-declared input channels.
    this->currentClock = body->getArgument(0);
    unsigned argIdx = 0;
    for (auto &p : this->currentPorts)
    {
        std::string pname = p.name.str();
        this->portDirs[moduleName][pname] = p.dir;
        if (p.dir == circt::hw::ModulePort::Direction::Input)
        {
            this->portValues[moduleName][pname] = body->getArgument(argIdx++);
        }
    }
    // rst is the second block arg.
    b.setInsertionPointToStart(body);
    this->currentReset = body->getArgument(1);

    // Visit shared variables first so they exist before states reference them.
    if (auto blk = antlrCtx->machine_block())
    {
        if (auto sh = blk->decl_shared()) visit(sh);
        if (auto st = blk->decl_states()) visit(st);
    }

    // ── Finalize: backpatch register inputs and emit module outputs ─────────
    // 1. Variable registers: input <= varNext chain (or hold if no writes).
    for (auto &kv : this->varRegs[moduleName])
    {
        const std::string &name = kv.first;
        circt::seq::CompRegOp reg = kv.second;
        mlir::Value next = this->varNext[moduleName][name];
        if (next) reg->setOperand(0, next);
    }
    // 2. State register: input <= stateNext (defaults to hold if no goto).
    if (this->stateRegs.count(moduleName))
    {
        circt::seq::CompRegOp sreg = this->stateRegs[moduleName];
        mlir::Value next = this->stateNext[moduleName];
        if (next) sreg->setOperand(0, next);
    }
    // 3. Output ports: wrap (data, valid) into a channel and emit hw.output.
    llvm::SmallVector<mlir::Value, 4> outputOperands;
    for (auto &p : this->currentPorts)
    {
        if (p.dir != circt::hw::ModulePort::Direction::Output) continue;
        std::string pname = p.name.str();
        mlir::Value data = this->outputDataNext[moduleName][pname];
        mlir::Value valid = this->outputValidNext[moduleName][pname];
        auto chTy = mlir::dyn_cast<circt::esi::ChannelType>(p.type);
        if (!data && chTy)
            data = circt::hw::ConstantOp::create(b, loc, chTy.getInner(), 0);
        if (!valid)
            valid = circt::hw::ConstantOp::create(b, loc, b.getI1Type(), 0);
        if (chTy)
        {
            circt::esi::WrapValidReadyOp wrap = circt::esi::WrapValidReadyOp::create(
                b, loc, p.type, b.getI1Type(), data, valid);
            outputOperands.push_back(wrap.getResult(0));
        }
    }
    circt::hw::OutputOp::create(b, loc, outputOperands);

    // Restore builder to the top-level module so subsequent declarations
    // (other machines, the instantiate block) land at the right scope.
    b.setInsertionPointToEnd(this->topModule.getBody());

    return std::any();
}

std::any LowerVisitor::visitDecl_interface(castParser::Decl_interfaceContext *antlrCtx)
{
    // Each io declaration adds one ESI-channel port per ident.
    for (auto decl : antlrCtx->interface_block()->decl_io())
    {
        auto typedIdent = decl->ident_typed();
        auto idents = decl->ident();
        for (auto id : idents)
        {
            circt::hw::PortInfo port;
            port.name = builder.getStringAttr(id->getText());
            auto maybeType = getMlirType(typedIdent);
            mlir::Type inner = maybeType.value_or(builder.getI8Type());
            port.type = circt::esi::ChannelType::get(&this->ctx, inner);
            this->channels[id->getText()] = port.type;
            switch (decl->direction->getType())
            {
            case castLexer::INPUT:
                port.dir = circt::hw::ModulePort::Direction::Input;
                break;
            case castLexer::OUTPUT:
                port.dir = circt::hw::ModulePort::Direction::Output;
                break;
            default:
                println("Machine port input/output error");
                exit(0);
            }
            this->currentPorts.push_back(port);
        }
    }
    return std::any();
}

std::any LowerVisitor::visitDecl_shared(castParser::Decl_sharedContext *antlrCtx)
{
    mlir::Location loc = builder.getUnknownLoc();
    for (auto v : antlrCtx->shared_block()->decl_var())
    {
        auto typedIdent = v->ident_typed();
        auto maybeType = getMlirType(typedIdent);
        if (!maybeType) continue;
        for (auto id : typedIdent->ident())
        {
            std::string name = id->getText();
            this->variables[this->currentModuleName][name] = maybeType.value();
            mlir::Value zero = circt::hw::ConstantOp::create(builder, loc, maybeType.value(), 0);
            // Register starts with `zero` as a placeholder input — backpatched after states lower.
            circt::seq::CompRegOp reg = circt::seq::CompRegOp::create(
                builder, loc, zero, this->currentClock, this->currentReset, zero);
            this->varRegs[this->currentModuleName][name] = reg;
            // The default next-value is the register's own output (hold).
            this->varNext[this->currentModuleName][name] = reg.getResult();
        }
    }
    return std::any();
}

// ─────────────────────────────────────────────────────────────────────────────
// State machine
// ─────────────────────────────────────────────────────────────────────────────

std::any LowerVisitor::visitDecl_states(castParser::Decl_statesContext *antlrCtx)
{
    mlir::Location loc = builder.getUnknownLoc();
    auto sblock = antlrCtx->states_block();

    // Assign integer encodings to each state and create the state register.
    auto stateDecls = sblock->decl_state();
    if (stateDecls.empty()) return std::any();

    unsigned needed = 1;
    while ((1u << needed) < stateDecls.size()) ++needed;
    if (needed < 1) needed = 1;
    mlir::Type stateTy = builder.getIntegerType(needed);

    uint32_t id = 0;
    for (auto sd : stateDecls)
    {
        std::string name = sd->ident()->getText();
        this->stateIds[this->currentModuleName][name] = id;
        ++id;
    }

    // The first declared state (id 0) is the reset state.
    mlir::Value resetVal = circt::hw::ConstantOp::create(builder, loc, stateTy, 0);
    // Placeholder input — backpatched after we visit each state body.
    circt::seq::CompRegOp stateReg = circt::seq::CompRegOp::create(
        builder, loc, resetVal, this->currentClock, this->currentReset, resetVal);
    this->stateRegs[this->currentModuleName] = stateReg;
    // Default next-state = hold the current value.
    this->stateNext[this->currentModuleName] = stateReg.getResult();

    // Build "state == X" comparators for each state.
    for (auto &kv : this->stateIds[this->currentModuleName])
    {
        mlir::Value sId = circt::hw::ConstantOp::create(builder, loc, stateTy, kv.second);
        mlir::Value eq = circt::comb::ICmpOp::create(
            builder, loc, circt::comb::ICmpPredicate::eq, stateReg.getResult(), sId);
        this->stateActive[this->currentModuleName][kv.first] = eq;
    }

    // Now lower each state body. The state-active signal becomes the fire mask.
    for (auto sd : stateDecls)
        visit(sd);

    return std::any();
}

std::any LowerVisitor::visitDecl_state(castParser::Decl_stateContext *antlrCtx)
{
    std::string name = antlrCtx->ident()->getText();
    mlir::Value active = this->stateActive[this->currentModuleName][name];
    if (!active) return std::any();

    // Save outer context, push state context.
    mlir::Value prevFire = this->currentFire;
    std::string prevName = this->currentStateName;
    bool prevIn = this->inStateBody;

    this->currentFire = active;
    this->currentStateName = name;
    this->inStateBody = true;
    this->localBindings.clear();

    // State entry guards: channel receives that gate the state.
    // While inStateHeader is true, visitStmt_binary will narrow currentFire
    // to (active && valid) and bind each received variable as a local so the
    // body sees the just-received value instead of the stale register output.
    this->inStateHeader = true;
    for (auto sb : antlrCtx->stmt_binary())
        visit(sb);
    this->inStateHeader = false;
    // currentFire is now (active && valid1 && valid2 && ...) for all header receives.

    // Body block.
    if (auto blk = antlrCtx->stmt_block())
        for (auto s : blk->stmt())
            visit(s);

    this->localBindings.clear();
    this->currentFire = prevFire;
    this->currentStateName = prevName;
    this->inStateBody = prevIn;
    return std::any();
}

std::any LowerVisitor::visitStmt_nextstate(castParser::Stmt_nextstateContext *antlrCtx)
{
    // `nextstate <ident>` and `goto <ident>` both transition the FSM.
    // Find the target by scanning children for the state name.
    std::string target;
    for (auto c : antlrCtx->children)
    {
        if (auto ident = dynamic_cast<castParser::IdentContext *>(c))
        {
            target = ident->getText();
            break;
        }
    }
    if (target.empty()) return std::any();

    auto &ids = this->stateIds[this->currentModuleName];
    auto it = ids.find(target);
    if (it == ids.end()) return std::any();

    mlir::Location loc = builder.getUnknownLoc();
    circt::seq::CompRegOp sreg = this->stateRegs[this->currentModuleName];
    mlir::Type stateTy = sreg.getResult().getType();
    mlir::Value targetId = circt::hw::ConstantOp::create(builder, loc, stateTy, it->second);

    // Append: stateNext = mux(currentFire, targetId, stateNext_prev)
    mlir::Value prev = this->stateNext[this->currentModuleName];
    mlir::Value next = circt::comb::MuxOp::create(builder, loc, this->currentFire, targetId, prev);
    this->stateNext[this->currentModuleName] = next;
    return std::any();
}

// Lower print("literal", val, ...) → always_ff @(posedge clk) if(fire) $fwrite(...)
// Each print fires exactly once per cycle the owning state is active.
std::any LowerVisitor::visitExpr_func_call(castParser::Expr_func_callContext *antlrCtx)
{
    std::string funcName = antlrCtx->func_name()->getText();
    if (funcName != "print" || !this->inStateBody || !this->currentFire)
        return std::any();

    mlir::Location loc = builder.getUnknownLoc();

    // Build the $fwrite format string and collect runtime value substitutions.
    // String literals become verbatim text; all other exprs become %0d.
    std::string fmtStr;
    llvm::SmallVector<mlir::Value, 4> subs;
    for (auto argExpr : antlrCtx->expr())
    {
        if (argExpr->string_literal())
        {
            std::string raw = argExpr->string_literal()->getText();
            if (raw.size() >= 2) raw = raw.substr(1, raw.size() - 2); // strip quotes
            fmtStr += raw;
        }
        else
        {
            auto any = visit(argExpr);
            if (any.has_value() && any.type() == typeid(mlir::Value))
            {
                mlir::Value v = std::any_cast<mlir::Value>(any);
                if (v) { fmtStr += "%0d"; subs.push_back(v); }
            }
        }
    }
    fmtStr += "\n";

    // sv.alwaysff needs i1; convert from seq.clock.
    mlir::Value i1Clk = circt::seq::FromClockOp::create(builder, loc, this->currentClock);
    // Capture by value so the lambdas are self-contained.
    mlir::Value fire = this->currentFire;
    mlir::StringAttr fmtAttr = builder.getStringAttr(fmtStr);
    // stdout MCD in Verilog: 32'h80000001
    mlir::Value fd = circt::hw::ConstantOp::create(
        builder, loc, builder.getIntegerType(32), 0x80000001LL);

    circt::sv::AlwaysFFOp::create(builder, loc,
        circt::sv::EventControl::AtPosEdge, i1Clk,
        [&]() {
            circt::sv::IfOp::create(builder, loc, fire,
                [&]() {
                    circt::sv::FWriteOp::create(builder, loc, fd, fmtAttr, subs);
                });
        });

    return std::any();
}

// ─────────────────────────────────────────────────────────────────────────────
// Expression / identifier resolution
// ─────────────────────────────────────────────────────────────────────────────

std::optional<mlir::Type> LowerVisitor::getMlirType(castParser::Ident_typedContext *typedIdentCtx)
{
    auto typeCtx = typedIdentCtx->type();
    if (!typeCtx) return std::nullopt;
    auto litCtx = typeCtx->type_lit();
    if (!litCtx) return std::nullopt;
    size_t tokenType = litCtx->getStart()->getType();
    switch (tokenType)
    {
    // All hardware values are signless integers — comb ops require this.
    // Signedness is a language-level concept handled by op selection (e.g.
    // DivU vs DivS, ICmp ult vs slt) rather than carried in the type.
    case castLexer::T_INTEGER: return builder.getI32Type();
    case castLexer::T_BYTE:    return builder.getI8Type();
    case castLexer::T_INT32:   return builder.getI32Type();
    case castLexer::T_UINT32:  return builder.getIntegerType(32);
    case castLexer::T_UINT16:  return builder.getIntegerType(16);
    case castLexer::T_BOOL:    return builder.getI1Type();
    case castLexer::T_STRING:  return circt::hw::StringType::get(&this->ctx);
    }
    return std::nullopt;
}

std::any LowerVisitor::visitIdent(castParser::IdentContext *antlrCtx)
{
    std::string name = antlrCtx->getText();
    // 0. State-header local binding — the just-received channel data.
    if (this->localBindings.count(name))
        return mlir::Value(this->localBindings[name]);
    // 1. Shared variable register read
    if (this->varRegs.count(this->currentModuleName) &&
        this->varRegs[this->currentModuleName].count(name))
        return mlir::Value(this->varRegs[this->currentModuleName][name].getResult());
    // 2. Interface port (block argument)
    if (this->portValues.count(this->currentModuleName) &&
        this->portValues[this->currentModuleName].count(name))
        return this->portValues[this->currentModuleName][name];
    // 3. Enum member literal
    if (this->enumValues.count(name))
    {
        auto [val, w] = this->enumValues[name];
        return mlir::Value(circt::hw::ConstantOp::create(
            builder, builder.getUnknownLoc(), builder.getIntegerType(w), val));
    }
    return std::any();
}

std::any LowerVisitor::visitNumber_literal(castParser::Number_literalContext *antlrCtx)
{
    int64_t value = std::stoll(antlrCtx->getText());
    // Use the surrounding type hint when available; otherwise default to i32.
    mlir::Type t = this->currentExprType ? this->currentExprType : builder.getI32Type();
    if (!mlir::isa<mlir::IntegerType>(t)) t = builder.getI32Type();
    return mlir::Value(circt::hw::ConstantOp::create(
        builder, builder.getUnknownLoc(), t, value));
}

std::any LowerVisitor::visitType_lit(castParser::Type_litContext *antlrCtx)
{
    return antlrCtx->getStart()->getType();
}

std::any LowerVisitor::visitAssignment_op(castParser::Assignment_opContext *antlrCtx)
{
    return antlrCtx->getStart()->getType();
}

// expr can be: ident | ident_field | literal | '(' expr ')' | unary expr |
//              expr [..] | expr (...) | expr bin_op expr | expr update_op | expr_func_call.
// We dispatch by inspecting which children are present.
std::any LowerVisitor::visitExpr(castParser::ExprContext *antlrCtx)
{
    mlir::Location loc = builder.getUnknownLoc();

    // Single-child terminals: ident, ident_field, literals, expr_func_call.
    if (antlrCtx->ident()) return visit(antlrCtx->ident());
    if (antlrCtx->ident_field()) return visit(antlrCtx->ident_field());
    if (antlrCtx->number_literal()) return visit(antlrCtx->number_literal());
    if (antlrCtx->string_literal()) return std::any();
    if (antlrCtx->nil_literal()) return std::any();
    if (antlrCtx->expr_func_call()) return visit(antlrCtx->expr_func_call());

    auto subExprs = antlrCtx->expr();

    // Parenthesised: '(' expr ')'.
    if (subExprs.size() == 1 && !antlrCtx->unary_op() && !antlrCtx->update_op())
        return visit(subExprs[0]);

    // Unary: unary_op expr.
    if (antlrCtx->unary_op() && subExprs.size() == 1)
    {
        auto inner = visit(subExprs[0]);
        if (!inner.has_value()) return std::any();
        mlir::Value v = std::any_cast<mlir::Value>(inner);
        std::string opTxt = antlrCtx->unary_op()->getText();
        if (opTxt == "!")
        {
            mlir::Value one = circt::hw::ConstantOp::create(builder, loc, v.getType(), 1);
            return mlir::Value(circt::comb::XorOp::create(builder, loc, v, one));
        }
        return v;
    }

    // Binary: expr bin_op expr.
    if (antlrCtx->bin_op() && subExprs.size() == 2)
    {
        auto la = visit(subExprs[0]);
        auto ra = visit(subExprs[1]);
        if (!la.has_value() || !ra.has_value()) return std::any();
        mlir::Value lhs = std::any_cast<mlir::Value>(la);
        mlir::Value rhs = std::any_cast<mlir::Value>(ra);
        if (!lhs || !rhs) return std::any();

        // Width-equalize if both are integer.
        auto li = mlir::dyn_cast<mlir::IntegerType>(lhs.getType());
        auto ri = mlir::dyn_cast<mlir::IntegerType>(rhs.getType());
        if (li && ri && li.getWidth() != ri.getWidth())
        {
            unsigned w = std::max(li.getWidth(), ri.getWidth());
            mlir::Type wide = builder.getIntegerType(w);
            lhs = coerce(lhs, wide, loc);
            rhs = coerce(rhs, wide, loc);
        }

        std::string op = antlrCtx->bin_op()->getText();
        if (op == "+")  return mlir::Value(circt::comb::AddOp::create(builder, loc, lhs, rhs));
        if (op == "-")  return mlir::Value(circt::comb::SubOp::create(builder, loc, lhs, rhs));
        if (op == "*")  return mlir::Value(circt::comb::MulOp::create(builder, loc, lhs, rhs));
        if (op == "/")  return mlir::Value(circt::comb::DivUOp::create(builder, loc, lhs, rhs));
        if (op == "%")  return mlir::Value(circt::comb::ModUOp::create(builder, loc, lhs, rhs));
        if (op == "&")  return mlir::Value(circt::comb::AndOp::create(builder, loc, lhs, rhs));
        if (op == "|")  return mlir::Value(circt::comb::OrOp::create(builder, loc, lhs, rhs));
        if (op == "^")  return mlir::Value(circt::comb::XorOp::create(builder, loc, lhs, rhs));
        if (op == "<<") return mlir::Value(circt::comb::ShlOp::create(builder, loc, lhs, rhs));
        if (op == ">>") return mlir::Value(circt::comb::ShrUOp::create(builder, loc, lhs, rhs));
        if (op == "&&") return mlir::Value(circt::comb::AndOp::create(builder, loc, lhs, rhs));
        if (op == "||") return mlir::Value(circt::comb::OrOp::create(builder, loc, lhs, rhs));
        if (op == "==") return mlir::Value(circt::comb::ICmpOp::create(builder, loc, circt::comb::ICmpPredicate::eq, lhs, rhs));
        if (op == "!=") return mlir::Value(circt::comb::ICmpOp::create(builder, loc, circt::comb::ICmpPredicate::ne, lhs, rhs));
        if (op == "<")  return mlir::Value(circt::comb::ICmpOp::create(builder, loc, circt::comb::ICmpPredicate::ult, lhs, rhs));
        if (op == "<=") return mlir::Value(circt::comb::ICmpOp::create(builder, loc, circt::comb::ICmpPredicate::ule, lhs, rhs));
        if (op == ">")  return mlir::Value(circt::comb::ICmpOp::create(builder, loc, circt::comb::ICmpPredicate::ugt, lhs, rhs));
        if (op == ">=") return mlir::Value(circt::comb::ICmpOp::create(builder, loc, circt::comb::ICmpPredicate::uge, lhs, rhs));
        return std::any();
    }

    // Update: expr++ / expr-- — when the operand is a known variable, emit a
    // dataflow-conditional increment/decrement of its register. Returns the
    // current value so the expression can also be used as an rvalue.
    if (antlrCtx->update_op() && subExprs.size() == 1)
    {
        auto inner = visit(subExprs[0]);
        if (!inner.has_value()) return std::any();
        mlir::Value v = std::any_cast<mlir::Value>(inner);
        if (!v) return std::any();

        if (auto idCtx = subExprs[0]->ident())
        {
            std::string nm = idCtx->getText();
            if (this->varRegs[this->currentModuleName].count(nm) && this->currentFire)
            {
                mlir::Value one = circt::hw::ConstantOp::create(builder, loc, v.getType(), 1);
                std::string opTxt = antlrCtx->update_op()->getText();
                mlir::Value updated = (opTxt == "--")
                    ? mlir::Value(circt::comb::SubOp::create(builder, loc, v, one))
                    : mlir::Value(circt::comb::AddOp::create(builder, loc, v, one));
                writeVar(nm, updated, this->currentFire);
            }
        }
        return v;
    }

    // Function call: expr '(' args ')' — first subExpr is the callee ident.
    // This is the left-recursive parse form for calls like print("msg", val).
    if (subExprs.size() >= 1 && !antlrCtx->bin_op() &&
        !antlrCtx->unary_op() && !antlrCtx->update_op() && subExprs[0]->ident())
    {
        bool isFuncCall = false;
        for (auto *c : antlrCtx->children)
            if (auto *t = dynamic_cast<antlr4::tree::TerminalNode *>(c))
                if (t->getText() == "(") { isFuncCall = true; break; }

        if (isFuncCall)
        {
            std::string funcName = subExprs[0]->ident()->getText();
            if (funcName == "print" && this->inStateBody && this->currentFire)
            {
                std::string fmtStr;
                llvm::SmallVector<mlir::Value, 4> subs;
                // subExprs[0] is the callee; [1..] are the arguments.
                for (size_t i = 1; i < subExprs.size(); ++i)
                {
                    auto *arg = subExprs[i];
                    if (arg->string_literal())
                    {
                        std::string raw = arg->string_literal()->getText();
                        if (raw.size() >= 2) raw = raw.substr(1, raw.size() - 2);
                        fmtStr += raw;
                    }
                    else
                    {
                        auto a = visit(arg);
                        if (a.has_value() && a.type() == typeid(mlir::Value))
                        {
                            mlir::Value v2 = std::any_cast<mlir::Value>(a);
                            if (v2) { fmtStr += "%0d"; subs.push_back(v2); }
                        }
                    }
                }
                fmtStr += "\n";

                // sv.alwaysff needs i1; convert from seq.clock.
                mlir::Value i1Clk = circt::seq::FromClockOp::create(builder, loc, this->currentClock);
                mlir::Value fire  = this->currentFire;
                mlir::StringAttr fmtAttr = builder.getStringAttr(fmtStr);
                // stdout MCD in Verilog: 32'h80000001
                mlir::Value fd = circt::hw::ConstantOp::create(
                    builder, loc, builder.getIntegerType(32), 0x80000001LL);

                circt::sv::AlwaysFFOp::create(builder, loc,
                    circt::sv::EventControl::AtPosEdge, i1Clk,
                    [&]() {
                        circt::sv::IfOp::create(builder, loc, fire,
                            [&]() {
                                circt::sv::FWriteOp::create(builder, loc, fd, fmtAttr, subs);
                            });
                    });
            }
            return std::any();
        }
    }

    return visitChildren(antlrCtx);
}

std::any LowerVisitor::visitDecl_var(castParser::Decl_varContext *antlrCtx)
{
    // `var T name (= expr)?` — when used inside a state, treat it as a local
    // shared variable: allocate a register and seed with the rhs if present.
    auto typedIdent = antlrCtx->ident_typed();
    auto maybeType = getMlirType(typedIdent);
    if (!maybeType) return std::any();
    mlir::Location loc = builder.getUnknownLoc();
    for (auto id : typedIdent->ident())
    {
        std::string name = id->getText();
        if (this->varRegs[this->currentModuleName].count(name)) continue;
        this->variables[this->currentModuleName][name] = maybeType.value();
        mlir::Value zero = circt::hw::ConstantOp::create(builder, loc, maybeType.value(), 0);
        circt::seq::CompRegOp reg = circt::seq::CompRegOp::create(
            builder, loc, zero, this->currentClock, this->currentReset, zero);
        this->varRegs[this->currentModuleName][name] = reg;
        this->varNext[this->currentModuleName][name] = reg.getResult();
    }
    if (antlrCtx->expr())
    {
        auto rhs = visit(antlrCtx->expr());
        if (rhs.has_value() && this->currentFire)
        {
            mlir::Value v = std::any_cast<mlir::Value>(rhs);
            if (v && !typedIdent->ident().empty())
                writeVar(typedIdent->ident()[0]->getText(), v, this->currentFire);
        }
    }
    return std::any();
}

// ─────────────────────────────────────────────────────────────────────────────
// Statements
// ─────────────────────────────────────────────────────────────────────────────

std::any LowerVisitor::visitStmt(castParser::StmtContext *antlrCtx)
{
    return visitChildren(antlrCtx);
}

std::any LowerVisitor::visitStmt_if(castParser::Stmt_ifContext *antlrCtx)
{
    // if <expr> { thenBlock } (else (block | ifChain))?
    // Predicate-gate the body by ANDing currentFire with the condition.
    mlir::Location loc = builder.getUnknownLoc();
    auto condA = visit(antlrCtx->expr());
    if (!condA.has_value()) return std::any();
    mlir::Value cond = std::any_cast<mlir::Value>(condA);
    if (!cond) return std::any();
    // Force cond to i1
    if (auto it = mlir::dyn_cast<mlir::IntegerType>(cond.getType()))
        if (it.getWidth() != 1)
            cond = circt::comb::ICmpOp::create(
                builder, loc, circt::comb::ICmpPredicate::ne, cond,
                circt::hw::ConstantOp::create(builder, loc, cond.getType(), 0));

    mlir::Value outerFire = this->currentFire;
    mlir::Value thenFire = outerFire
        ? mlir::Value(circt::comb::AndOp::create(builder, loc, outerFire, cond))
        : cond;

    auto blocks = antlrCtx->stmt_block();
    if (!blocks.empty())
    {
        this->currentFire = thenFire;
        for (auto s : blocks[0]->stmt()) visit(s);
        this->currentFire = outerFire;
    }
    // else-branch: invert cond.
    mlir::Value notCond = circt::comb::XorOp::create(
        builder, loc, cond,
        circt::hw::ConstantOp::create(builder, loc, builder.getI1Type(), 1));
    mlir::Value elseFire = outerFire
        ? mlir::Value(circt::comb::AndOp::create(builder, loc, outerFire, notCond))
        : notCond;
    if (blocks.size() > 1)
    {
        this->currentFire = elseFire;
        for (auto s : blocks[1]->stmt()) visit(s);
        this->currentFire = outerFire;
    }
    else if (auto chained = antlrCtx->stmt_if())
    {
        this->currentFire = elseFire;
        visit(chained);
        this->currentFire = outerFire;
    }
    return std::any();
}

std::any LowerVisitor::visitStmt_binary(castParser::Stmt_binaryContext *antlrCtx)
{
    mlir::Location loc = builder.getUnknownLoc();
    size_t op = antlrCtx->assignment_op()->getStart()->getType();

    auto lhsExpr = antlrCtx->expr(0);
    auto rhsExpr = antlrCtx->expr(1);

    // Determine LHS lvalue kind without polluting register-read paths.
    std::string lhsName;
    bool lhsIsVar = false;
    if (lhsExpr->ident())
    {
        lhsName = lhsExpr->ident()->getText();
        lhsIsVar = true;
    }

    // Visit RHS for value.
    mlir::Type hint;
    if (lhsIsVar && this->variables[this->currentModuleName].count(lhsName))
        hint = this->variables[this->currentModuleName][lhsName];
    mlir::Type prevHint = this->currentExprType;
    this->currentExprType = hint;
    auto rhsAny = visit(rhsExpr);
    this->currentExprType = prevHint;
    mlir::Value rhsVal;
    if (rhsAny.has_value() && rhsAny.type() == typeid(mlir::Value))
        rhsVal = std::any_cast<mlir::Value>(rhsAny);

    // The fire condition for this statement.
    mlir::Value fire = this->currentFire
        ? this->currentFire
        : circt::hw::ConstantOp::create(builder, loc, builder.getI1Type(), 1);

    // ── Channel arrow ops (`<-` and `->`) ────────────────────────────────────
    // Normalize to (sourceExpr, destExpr) using the arrow direction:
    //   `lhs <- rhs`  →  data flows rhs → lhs   (source=rhs, dest=lhs)
    //   `lhs -> rhs`  →  data flows lhs → rhs   (source=lhs, dest=rhs)
    // Then dispatch by what each side is (channel port vs variable).
    if (op == castLexer::ASSIGN_CHANNEL_RECEIVE || op == castLexer::ASSIGN_CHANNEL_SEND)
    {
        bool rev = (op == castLexer::ASSIGN_CHANNEL_SEND);
        castParser::ExprContext *srcExpr = rev ? lhsExpr : rhsExpr;
        castParser::ExprContext *dstExpr = rev ? rhsExpr : lhsExpr;

        // Names for the destination (if it's an ident or instance.port).
        std::string dstName;
        std::string dstInst, dstPort;
        bool dstIsIdent = false, dstIsField = false;
        if (dstExpr->ident()) { dstName = dstExpr->ident()->getText(); dstIsIdent = true; }
        else if (dstExpr->ident_field())
        {
            dstInst = dstExpr->ident_field()->ident(0)->getText();
            dstPort = dstExpr->ident_field()->ident(1)->getText();
            dstIsField = true;
        }

        // Source value — visit and try to extract a channel or scalar value.
        auto srcAny = visit(srcExpr);
        mlir::Value srcVal;
        if (srcAny.has_value() && srcAny.type() == typeid(mlir::Value))
            srcVal = std::any_cast<mlir::Value>(srcAny);

        // Case A: source is an ESI channel → receive (always-ready dataflow)
        // and capture into the destination variable.
        if (srcVal)
        {
            if (auto chSrc = mlir::dyn_cast<mlir::TypedValue<circt::esi::ChannelType>>(srcVal))
            {
                mlir::Type inner = chSrc.getType().getInner();
                circt::esi::UnwrapValidReadyOp unwrap = circt::esi::UnwrapValidReadyOp::create(
                    builder, loc, chSrc, fire);
                mlir::Value data  = unwrap.getRawOutput();
                mlir::Value valid = unwrap.getValid();
                mlir::Value cond  = circt::comb::AndOp::create(builder, loc, fire, valid);

                if (dstIsIdent)
                {
                    // Auto-declare the destination as a register if unknown.
                    if (!this->varRegs[this->currentModuleName].count(dstName))
                    {
                        mlir::Value zero = circt::hw::ConstantOp::create(builder, loc, inner, 0);
                        circt::seq::CompRegOp reg = circt::seq::CompRegOp::create(
                            builder, loc, zero, this->currentClock, this->currentReset, zero);
                        this->varRegs[this->currentModuleName][dstName] = reg;
                        this->varNext[this->currentModuleName][dstName] = reg.getResult();
                        this->variables[this->currentModuleName][dstName] = inner;
                    }
                    writeVar(dstName, data, cond);
                    if (this->inStateHeader)
                    {
                        // Gate the body on this channel being valid.
                        this->currentFire = cond;
                        // Bind to the received data so reads in this state
                        // see the new value rather than the stale register.
                        this->localBindings[dstName] = data;
                    }
                }
                return std::any();
            }
        }

        // Case B: destination is an output port → drive it with the source value.
        // Output ports aren't block arguments, so look up by direction map only.
        if (dstIsIdent && this->portDirs[this->currentModuleName].count(dstName) &&
            this->portDirs[this->currentModuleName][dstName] ==
                circt::hw::ModulePort::Direction::Output && srcVal)
        {
            // Find the port's type from the recorded port list.
            mlir::Type portType;
            for (auto &p : this->currentPorts)
                if (p.name == dstName) { portType = p.type; break; }
            auto chTy = mlir::dyn_cast<circt::esi::ChannelType>(portType);
            if (!chTy) return std::any();
            mlir::Value zero   = circt::hw::ConstantOp::create(builder, loc, chTy.getInner(), 0);
            mlir::Value falseV = circt::hw::ConstantOp::create(builder, loc, builder.getI1Type(), 0);
            mlir::Value prevData  = this->outputDataNext[this->currentModuleName][dstName];
            mlir::Value prevValid = this->outputValidNext[this->currentModuleName][dstName];
            if (!prevData)  prevData  = zero;
            if (!prevValid) prevValid = falseV;
            mlir::Value coercedSrc = coerce(srcVal, chTy.getInner(), loc);
            mlir::Value newData  = circt::comb::MuxOp::create(builder, loc, fire, coercedSrc, prevData);
            mlir::Value newValid = circt::comb::OrOp::create(builder, loc, fire, prevValid);
            this->outputDataNext[this->currentModuleName][dstName]  = newData;
            this->outputValidNext[this->currentModuleName][dstName] = newValid;
            return std::any();
        }

        // Case C: destination is `instance.port` — patch the FIFO feed registers
        // so the instance receives the specified value continuously.
        if (dstIsField && this->insideInsantiate && srcVal)
        {
            auto instIt = this->instanceFifoRegs.find(dstInst);
            if (instIt != this->instanceFifoRegs.end())
            {
                auto portIt = instIt->second.find(dstPort);
                if (portIt != instIt->second.end())
                {
                    auto [dataReg, validReg] = portIt->second;
                    mlir::Type innerTy = dataReg.getResult().getType();
                    mlir::Value data = coerce(srcVal, innerTy, loc);
                    mlir::Value trueV = circt::hw::ConstantOp::create(
                        builder, loc, builder.getI1Type(), 1);
                    // operand 0 = input, operand 3 = resetValue
                    dataReg->setOperand(0, data);
                    dataReg->setOperand(3, data);
                    validReg->setOperand(0, trueV);
                    validReg->setOperand(3, trueV);
                }
            }
        }
        return std::any();
    }

    // ── Compound + plain assignments to a shared variable ───────────────────
    if (lhsIsVar && this->varRegs[this->currentModuleName].count(lhsName) && rhsVal)
    {
        mlir::Value current = readVar(lhsName);
        mlir::Value newVal;
        switch (op)
        {
        case castLexer::ASSIGN:
        case castLexer::ASSIGN_WALRUS:
            newVal = rhsVal;
            break;
        case castLexer::ASSIGN_ADD:
            newVal = circt::comb::AddOp::create(builder, loc, current, coerce(rhsVal, current.getType(), loc));
            break;
        case castLexer::ASSIGN_SUB:
            newVal = circt::comb::SubOp::create(builder, loc, current, coerce(rhsVal, current.getType(), loc));
            break;
        case castLexer::ASSIGN_MUL:
            newVal = circt::comb::MulOp::create(builder, loc, current, coerce(rhsVal, current.getType(), loc));
            break;
        case castLexer::ASSIGN_DIV:
            newVal = circt::comb::DivUOp::create(builder, loc, current, coerce(rhsVal, current.getType(), loc));
            break;
        case castLexer::ASSIGN_XOR:
            newVal = circt::comb::XorOp::create(builder, loc, current, coerce(rhsVal, current.getType(), loc));
            break;
        case castLexer::ASSIGN_SHL:
            newVal = circt::comb::ShlOp::create(builder, loc, current, coerce(rhsVal, current.getType(), loc));
            break;
        case castLexer::ASSIGN_SHR:
            newVal = circt::comb::ShrUOp::create(builder, loc, current, coerce(rhsVal, current.getType(), loc));
            break;
        default:
            newVal = rhsVal;
            break;
        }
        writeVar(lhsName, newVal, fire);
        return std::any();
    }

    return std::any();
}

// ─────────────────────────────────────────────────────────────────────────────
// Instantiation (top-level)
// ─────────────────────────────────────────────────────────────────────────────

std::any LowerVisitor::visitDecl_instantiate(castParser::Decl_instantiateContext *antlrCtx)
{
    // Wrap the instantiate block in a top-level `Main` HWModule so all the
    // FIFOs and instances live inside a hw.module (otherwise SSA values like
    // %arg0 have no defining scope). Main has a single `clk` port, which
    // every nested machine instance shares.
    mlir::Location loc = builder.getUnknownLoc();
    builder.setInsertionPointToEnd(this->topModule.getBody());

    llvm::SmallVector<circt::hw::PortInfo, 2> mainPorts;
    circt::hw::PortInfo clkPort;
    clkPort.name = builder.getStringAttr("clk");
    clkPort.type = circt::seq::ClockType::get(&this->ctx);
    clkPort.dir = circt::hw::ModulePort::Direction::Input;
    mainPorts.push_back(clkPort);

    circt::hw::PortInfo rstPort;
    rstPort.name = builder.getStringAttr("rst");
    rstPort.type = builder.getI1Type();
    rstPort.dir = circt::hw::ModulePort::Direction::Input;
    mainPorts.push_back(rstPort);

    circt::hw::ModulePortInfo mainPortInfo(mainPorts);
    circt::hw::HWModuleOp mainMod = circt::hw::HWModuleOp::create(
        builder, loc, builder.getStringAttr("Main"), mainPortInfo);

    mlir::Block *body = &mainMod.getBody().front();
    for (auto &op : llvm::make_early_inc_range(body->getOperations()))
        if (mlir::isa<circt::hw::OutputOp>(op)) op.erase();
    builder.setInsertionPointToStart(body);
    this->currentClock = body->getArgument(0);
    this->currentReset = body->getArgument(1);

    this->insideInsantiate = true;
    std::string moduleName = "instantiate";
    this->variables[moduleName];
    this->channels[moduleName];
    this->states[moduleName];
    this->portValues[moduleName];
    this->varRegs[moduleName];
    this->varNext[moduleName];
    this->currentModuleName = moduleName;

    std::any result = visitChildren(antlrCtx);

    // Terminate Main with a parameterless hw.output (no outputs declared).
    circt::hw::OutputOp::create(builder, loc, mlir::ValueRange{});
    builder.setInsertionPointToEnd(this->topModule.getBody());

    this->insideInsantiate = false;
    return result;
}

std::any LowerVisitor::visitInst_module(castParser::Inst_moduleContext *antlrCtx)
{
    std::string moduleName = antlrCtx->expr_func_call()->func_name()->getText();
    std::string varName = antlrCtx->ident()->getText();

    if (!this->modules.contains(moduleName))
    {
        println("Error: module '{}' not found for instantiation!", moduleName);
        exit(0);
    }
    circt::hw::HWModuleOp mod = this->modules[moduleName];
    llvm::SmallVector<circt::hw::PortInfo, 4> portInfo = mod.getPortList();
    std::vector<mlir::Value> instanceOperands;

    for (auto port : portInfo)
    {
        // Output ports are results of hw.instance, not operands — skip them.
        if (port.dir == circt::hw::ModulePort::Direction::Output) continue;

        mlir::Type portType = port.type;
        if (auto channelType = mlir::dyn_cast<circt::esi::ChannelType>(portType))
        {
            mlir::Type innerType = channelType.getInner();
            mlir::Value dummyData  = circt::hw::ConstantOp::create(builder, builder.getUnknownLoc(), innerType, 0);
            mlir::Value dummyValid = circt::hw::ConstantOp::create(builder, builder.getUnknownLoc(), builder.getI1Type(), 0);
            circt::seq::CompRegOp channel_data  = circt::seq::CompRegOp::create(builder, builder.getUnknownLoc(), dummyData,  this->currentClock, this->currentReset, dummyData);
            circt::seq::CompRegOp channel_valid = circt::seq::CompRegOp::create(builder, builder.getUnknownLoc(), dummyValid, this->currentClock, this->currentReset, dummyValid);
            // Record so `varName.portName <- value` can patch operands 0 and 3.
            this->instanceFifoRegs[varName][port.name.str()] = {channel_data, channel_valid};
            circt::esi::WrapValidReadyOp vr = circt::esi::WrapValidReadyOp::create(
                builder, builder.getUnknownLoc(), portType, builder.getI1Type(), channel_data, channel_valid);
            mlir::TypedValue<circt::esi::ChannelType> ch_in = vr.getChanOutput();
            circt::esi::FIFOOp fifo = circt::esi::FIFOOp::create(builder, builder.getUnknownLoc(),
                                                                 port.type, this->currentClock, this->currentReset, ch_in, 5);
            instanceOperands.push_back(fifo.getResult());
        }
        else if (mlir::isa<circt::seq::ClockType>(portType))
        {
            instanceOperands.push_back(this->currentClock);
        }
        else if (mlir::isa<mlir::IntegerType>(portType) &&
                 mlir::cast<mlir::IntegerType>(portType).getWidth() == 1)
        {
            instanceOperands.push_back(this->currentReset);
        }
        else
        {
            println("Error: unsupported port type for port '{}' in module '{}'!", port.name, moduleName);
            exit(0);
        }
    }
    circt::hw::InstanceOp instance = circt::hw::InstanceOp::create(
        builder, builder.getUnknownLoc(), mod, varName, instanceOperands);
    if (this->instances.contains(varName))
    {
        println("instance: {} is already declared!", varName);
        exit(0);
    }
    this->instances[varName] = instance;
    std::any result = visitChildren(antlrCtx);
    return result;
}

std::any LowerVisitor::visitIdent_field(castParser::Ident_fieldContext *antlrCtx)
{
    if (!this->insideInsantiate)
    {
        // Inside a machine body, ident_field could be reading an instance's
        // output port (cross-module wiring) — not currently supported.
        return std::any();
    }
    auto inst = antlrCtx->ident(0)->getText();
    auto port = antlrCtx->ident(1)->getText();
    if (this->instances.count(inst))
    {
        circt::hw::InstanceOp op = this->instances[inst];
        // Find the port index by name.
        auto plist = op.getPortList();
        unsigned outIdx = 0;
        for (auto &p : plist)
        {
            if (p.dir == circt::hw::ModulePort::Direction::Output && p.name == port)
                return mlir::Value(op.getResult(outIdx));
            if (p.dir == circt::hw::ModulePort::Direction::Output) ++outIdx;
        }
    }
    return std::any();
}
