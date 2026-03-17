#pragma once
#include "antlr4-runtime.h"
#include "castBaseVisitor.h"
#include <mlir/IR/Builders.h>
#include <mlir/IR/BuiltinOps.h>
#include <mlir/IR/Location.h>
#include <mlir/IR/MLIRContext.h>

class LowerVisitor : public castBaseVisitor
{
public:
    LowerVisitor(mlir::MLIRContext *context) : ctx(context) {}
    std::any visitDecl_machine(castParser::Decl_machineContext *ctx) override;

private:
    mlir::MLIRContext *ctx;
};