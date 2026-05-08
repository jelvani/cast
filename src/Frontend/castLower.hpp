#pragma once
#include "antlr4-runtime.h"
#include "castBaseVisitor.h"
#include <mlir/IR/Builders.h>
#include <mlir/IR/BuiltinOps.h>
#include <mlir/IR/Location.h>
#include <mlir/IR/MLIRContext.h>
#include <circt/Dialect/HW/HWOps.h>
#include <circt/Dialect/SV/SVOps.h>
#include <circt/Dialect/FSM/FSMOps.h>
#include "circt/Dialect/FSM/FSMDialect.h"
#include <circt/Dialect/Comb/CombDialect.h>
#include <circt/Dialect/Comb/CombOps.h>
#include <circt/Dialect/ESI/ESIDialect.h>
#include <circt/Dialect/ESI/ESIOps.h>
#include <circt/Dialect/Seq/SeqDialect.h>
#include <circt/Dialect/Seq/SeqOps.h>
#include <circt/Dialect/Seq/SeqTypes.h>

class LowerVisitor : public castBaseVisitor
{
public:
    mlir::ModuleOp topModule;

    LowerVisitor() : builder(&ctx)
    {
        mlir::Location loc = mlir::UnknownLoc::get(&ctx);
        topModule = mlir::ModuleOp::create(loc);
        builder.setInsertionPointToStart(topModule.getBody());
        this->ctx.getOrLoadDialect<circt::hw::HWDialect>();
        this->ctx.getOrLoadDialect<circt::fsm::FSMDialect>();
        this->ctx.getOrLoadDialect<circt::seq::SeqDialect>();
        this->ctx.getOrLoadDialect<circt::sv::SVDialect>();
        this->ctx.getOrLoadDialect<circt::comb::CombDialect>();
        this->ctx.getOrLoadDialect<circt::esi::ESIDialect>();
        this->currentClock = nullptr;
    }

    std::any visitDecl_machine(castParser::Decl_machineContext *antlrCtx) override;
    std::any visitDecl_states(castParser::Decl_statesContext *antlrCtx) override;
    std::any visitDecl_state(castParser::Decl_stateContext *antlrCtx) override;
    std::any visitDecl_interface(castParser::Decl_interfaceContext *antlrCtx) override;
    std::any visitDecl_shared(castParser::Decl_sharedContext *antlrCtx) override;
    std::any visitDecl_instantiate(castParser::Decl_instantiateContext *antlrCtx) override;
    std::any visitInst_module(castParser::Inst_moduleContext *antlrCtx) override;
    std::any visitIdent_field(castParser::Ident_fieldContext *antlrCtx) override;
    std::any visitStmt_binary(castParser::Stmt_binaryContext *antlrCtx) override;
    std::any visitAssignment_op(castParser::Assignment_opContext *antlrCtx) override;
    std::any visitIdent(castParser::IdentContext *antlrCtx) override;
    std::any visitType_lit(castParser::Type_litContext *antlrCtx) override;
    std::any visitNumber_literal(castParser::Number_literalContext *antlrCtx) override;
    std::any visitStmt(castParser::StmtContext *antlrCtx) override;
    std::any visitExpr(castParser::ExprContext *antlrCtx) override;
    std::any visitStmt_if(castParser::Stmt_ifContext *antlrCtx) override;
    std::any visitStmt_nextstate(castParser::Stmt_nextstateContext *antlrCtx) override;
    std::any visitExpr_func_call(castParser::Expr_func_callContext *antlrCtx) override;
    std::any visitDecl_var(castParser::Decl_varContext *antlrCtx) override;
    std::any visitDecl_enum(castParser::Decl_enumContext *antlrCtx) override;
    std::any visitDecl_func(castParser::Decl_funcContext *antlrCtx) override;
    std::any visitDecl_type(castParser::Decl_typeContext *antlrCtx) override;
    std::any visitDecl_memory(castParser::Decl_memoryContext *antlrCtx) override;
    std::any visitDecl_exception(castParser::Decl_exceptionContext *antlrCtx) override;
    std::any visitDecl_assertions(castParser::Decl_assertionsContext *antlrCtx) override;
    std::any visitStmt_for(castParser::Stmt_forContext *antlrCtx) override;
    std::any visitStmt_switch(castParser::Stmt_switchContext *antlrCtx) override;
    std::any visitStmt_return(castParser::Stmt_returnContext *antlrCtx) override;

    std::optional<mlir::Type> getMlirType(castParser::Ident_typedContext *typedIdentCtx);

private:
    mlir::MLIRContext ctx;
    mlir::OpBuilder builder;
    llvm::SmallVector<circt::hw::PortInfo, 4> currentPorts;
    std::string currentModuleName;

    // Type info per module
    std::unordered_map<std::string, std::unordered_map<std::string, mlir::Type>> variables;
    std::unordered_map<std::string, mlir::Type> channels;
    std::unordered_map<std::string, mlir::Type> states;
    std::unordered_map<std::string, circt::hw::HWModuleOp> modules;
    std::unordered_map<std::string, circt::hw::InstanceOp> instances;
    // FIFO feed registers created in visitInst_module, keyed by [instanceName][portName].
    // Patched when the instantiate block contains `m.port <- value`.
    std::unordered_map<std::string,
        std::unordered_map<std::string,
            std::pair<circt::seq::CompRegOp, circt::seq::CompRegOp>>> instanceFifoRegs;
    mlir::Value currentClock;

    // Live mlir::Value bindings per module
    // varRegs[mod][name] : the CompRegOp backing a shared variable
    std::unordered_map<std::string, std::unordered_map<std::string, circt::seq::CompRegOp>> varRegs;
    // varNext[mod][name] : the running mux chain that becomes the reg's input
    //                     Each conditional update appends a `mux(cond, newVal, prev)` layer.
    std::unordered_map<std::string, std::unordered_map<std::string, mlir::Value>> varNext;
    // portValues[mod][name] : the block-argument mlir::Value for an interface port
    std::unordered_map<std::string, std::unordered_map<std::string, mlir::Value>> portValues;
    // portDirs[mod][name] : direction of each port (input/output)
    std::unordered_map<std::string, std::unordered_map<std::string, circt::hw::ModulePort::Direction>> portDirs;

    // State machine, per module
    // stateRegs[mod] : the CompRegOp holding the active state ID
    std::unordered_map<std::string, circt::seq::CompRegOp> stateRegs;
    // stateIds[mod][name] : the integer encoding of each state
    std::unordered_map<std::string, std::unordered_map<std::string, uint32_t>> stateIds;
    // stateActive[mod][name] : combinational i1 — true when this state is current
    std::unordered_map<std::string, std::unordered_map<std::string, mlir::Value>> stateActive;
    // stateNext[mod] : the chain of goto-driven mux layers feeding the state register
    std::unordered_map<std::string, mlir::Value> stateNext;

    // Output channel drivers, per module
    // outputDataNext / outputValidNext : chains feeding wrap_valid_ready when the module finishes
    std::unordered_map<std::string, std::unordered_map<std::string, mlir::Value>> outputDataNext;
    std::unordered_map<std::string, std::unordered_map<std::string, mlir::Value>> outputValidNext;

    // Enum value table — name → (encoding, width)
    std::unordered_map<std::string, std::pair<uint64_t, unsigned>> enumValues;

    // Per-statement context (mutable while walking a state body)
    mlir::Value currentFire;        // i1: when high, the current statement should "fire"
    std::string currentStateName;   // name of the state currently being lowered
    bool inStateBody = false;       // true while inside a decl_state body
    bool inStateHeader = false;     // true while visiting state header receives
    mlir::Type currentExprType;     // hint for typing literals from context
    mlir::Value currentReset;       // i1: synchronous reset signal (0 in single-clock modules)
    // Within a state body, header receives bind their variable here so reads
    // see the just-received data rather than the previous register value.
    std::unordered_map<std::string, mlir::Value> localBindings;

    bool insideInsantiate = false;

    // Helpers
    mlir::Value readVar(const std::string &name);
    void writeVar(const std::string &name, mlir::Value newVal, mlir::Value cond);
    mlir::Value coerce(mlir::Value v, mlir::Type t, mlir::Location loc);
};