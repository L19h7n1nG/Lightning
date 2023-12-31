#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <stack>
#include <string>
#include <string_view>
#include <vector>

#include "brainfuck/BrainfuckTypeInterfaces.h"
#include "brainfuck/BrainfuckTypes.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorOr.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/ToolOutputFile.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/Block.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinDialect.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Dialect.h"
#include "mlir/IR/Location.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/TypeRange.h"
#include "mlir/IR/Types.h"
#include "mlir/IR/Value.h"
#include "mlir/IR/ValueRange.h"
#include "mlir/IR/Verifier.h"
#include "mlir/InitAllDialects.h"
#include "mlir/InitAllPasses.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Support/FileUtilities.h"
#include "mlir/Tools/mlir-opt/MlirOptMain.h"

#include "brainfuck/BrainfuckDialect.h"
#include "brainfuck/BrainfuckOps.h"

template <typename... Args>
void regisrtyDialects(mlir::DialectRegistry& registry) {
    registry.insert<Args...>();
}

struct Loop {
    mlir::Block *Entry, *Body, *Exit;
};
using namespace mlir::lightning::brainfuck;

mlir::Location getLoc(mlir::OpBuilder& builder) {
    return builder.getUnknownLoc();
}

// while( lhs.getVal() != 0 ) { ... }
void while_op_start(
        mlir::OpBuilder& builder,
        Loop* loop,
        CellOp& lhs,
        mlir::arith::ConstantIntOp Zero) {
    loop->Entry = builder.getInsertionBlock();

    auto loc = getLoc(builder);

    auto UnknownType = CellType::get(
            builder.getContext(),
            CellLikeType::kDynamic,
            CellLikeType::kDynamic);

    auto erased = builder.create<EraseOp>(loc, UnknownType, lhs);

    lhs = builder.create<CellOp>(builder.getUnknownLoc(), erased.getType());

    auto ResTy = CellType::get(
            builder.getContext(),
            CellLikeType::kDynamic,
            CellLikeType::kDynamic);

    llvm::SmallVector<mlir::Type, 2> Types{ResTy, Zero.getType()};
    llvm::SmallVector<mlir::Value, 2> Operands{lhs, Zero};

    auto whileOp = builder.create<mlir::scf::WhileOp>(loc, Types, Operands);

    auto Before =
            builder.createBlock(&whileOp.getBefore(), {}, Types, {loc, loc});

    auto After =
            builder.createBlock(&whileOp.getAfter(), {}, Types, {loc, loc});

    builder.setInsertionPointToStart(&whileOp.getBefore().front());

    auto NE_ZERO = builder.create<mlir::arith::CmpIOp>(
            getLoc(builder),
            mlir::arith::CmpIPredicate::ne,
            builder.create<mlir::arith::ConstantIntOp>(loc, lhs.getVal(), 32),
            Zero);

    builder.create<mlir::scf::ConditionOp>(
            loc, NE_ZERO, Before->getArguments());

    builder.setInsertionPointToStart(After);
}

void while_op_end(
        mlir::OpBuilder& builder,
        Loop* loop,
        CellOp& lhs,
        mlir::arith::ConstantIntOp Zero) {
    auto Entry = loop->Entry;
    auto loc = getLoc(builder);
    builder.create<mlir::scf::YieldOp>(loc, mlir::ValueRange{lhs, Zero});
    builder.setInsertionPointToEnd(Entry);
}

auto getCellAttrs(CellOp op, std::function<llvm::StringRef()> f) {
    auto sattr = f();
    if (sattr.equals("unknown")) {
        return CellLikeType::kDynamic;
    } else {
        int res = CellLikeType::kDynamic;
        sattr.getAsInteger(10, res);
        return res;
    }
}

int update(int original_val, int sum) {
    if (original_val == CellLikeType::kDynamic) {
        return CellLikeType::kDynamic;
    } else
        return original_val + sum;
}

mlir::OwningOpRef<mlir::ModuleOp> parse_code(
        mlir::MLIRContext& context,
        std::string_view code) {
    mlir::OpBuilder builder(&context);

    auto theModule = mlir::ModuleOp::create(builder.getUnknownLoc());

    auto mainFunc = builder.create<mlir::func::FuncOp>(
            builder.getUnknownLoc(),
            "main",
            builder.getFunctionType(std::nullopt, std::nullopt));
    theModule.push_back(mainFunc);
    auto entry = mainFunc.addEntryBlock();

    builder.setInsertionPointToStart(entry);

    auto cur_cell = builder.create<CellOp>(builder.getUnknownLoc(), 0, 0);

    auto const_zero = builder.create<mlir::arith::ConstantIntOp>(
            builder.getUnknownLoc(), 0, 32);

    std::stack<Loop, llvm::SmallVector<Loop>> loops;
    Loop ThisLoop;

    for (auto&& ch : code) {
        if (ch == '+') {
            cur_cell = builder.create<CellOp>(
                    builder.getUnknownLoc(),
                    cur_cell.getPos(),
                    update(cur_cell.getVal(), 1));

        } else if (ch == '-') {
            cur_cell = builder.create<CellOp>(
                    builder.getUnknownLoc(),
                    cur_cell.getPos(),
                    update(cur_cell.getVal(), -1));

        } else if (ch == '>') {
            cur_cell = builder.create<CellOp>(
                    builder.getUnknownLoc(),
                    update(cur_cell.getPos(), 1),
                    cur_cell.getVal());
        } else if (ch == '<') {
            cur_cell = builder.create<CellOp>(
                    builder.getUnknownLoc(),
                    update(cur_cell.getPos(), -1),
                    cur_cell.getVal());
        } else if (ch == '[') {
            while_op_start(builder, &ThisLoop, cur_cell, const_zero);
            loops.push(ThisLoop);
        } else if (ch == ']') {
            if (loops.empty()) {
                llvm::errs() << "Error\n";
                exit(0);
            }

            ThisLoop = loops.top();
            loops.pop();
            while_op_end(builder, &ThisLoop, cur_cell, const_zero);
        }
    }

    builder.setInsertionPointToEnd(entry);
    builder.create<mlir::func::ReturnOp>(builder.getUnknownLoc());

    if (failed(mlir::verify(theModule))) {
        theModule.emitError("module verification error");
        return nullptr;
    }
    return theModule;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        return 0;
    }
    auto filepath = argv[1];
    std::ifstream ifs(filepath);
    std::stringstream buffer;
    buffer << ifs.rdbuf();

    mlir::MLIRContext context;
    context.loadDialect<
            mlir::func::FuncDialect,
            mlir::lightning::brainfuck::BrainfuckDialect,
            mlir::arith::ArithDialect,
            mlir::scf::SCFDialect>();

    mlir::OwningOpRef<mlir::ModuleOp> module =
            parse_code(context, buffer.str());

    if (module) module->dump();

    return 0;
}
