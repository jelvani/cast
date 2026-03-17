#include "castLower.hpp"
#include <iostream>
#include <mlir/IR/Builders.h>
#include <mlir/IR/BuiltinOps.h>
#include <mlir/IR/Location.h>
#include <mlir/IR/OwningOpRef.h>

using namespace std;

// this is a test example visitor for emitting mlir
std::any LowerVisitor::visitDecl_machine(castParser::Decl_machineContext *antlrCtx)
{

    cout << "Visiting machine: " << antlrCtx->func_name()->getText() << endl;

    mlir::Location loc = mlir::UnknownLoc::get(this->ctx);

    mlir::OwningOpRef<mlir::ModuleOp> module = mlir::ModuleOp::create(loc);

    mlir::OpBuilder builder(this->ctx);

    builder.setInsertionPointToEnd(module->getBody());

    std::any result = visitChildren(antlrCtx);

    cout << "\n--- Generated MLIR Module ---" << endl;
    module->dump();

    return result;
}