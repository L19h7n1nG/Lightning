#ifndef BRAINFUCK_OPS
#define BRAINFUCK_OPS

include "mlir/Interfaces/InferTypeOpInterface.td"
include "mlir/Interfaces/SideEffectInterfaces.td"

include "BrainfuckDialect.td"
include "BrainfuckTypes.td"


class Brainfuck_Op<string mnemonic, list<Trait> traits = []> :
        Op<Brainfuck_Dialect, mnemonic, traits>;


def Brainfuck_CellOp: Brainfuck_Op<"cell", []> {
        let summary = "Cell op";
        let arguments = (ins SI32Attr:$pos, SI32Attr:$val);
        let results = (outs Brainfuck_CellType:$output);
        let builders = [
                OpBuilder<(ins "int":$pos, "int":$val)>,
                OpBuilder<(ins "llvm::ArrayRef<int>":$input)>,
                OpBuilder<(ins "mlir::lightning::brainfuck::CellType":$input)>
        ];
}


def Brainfuck_EraseOp: Brainfuck_Op<"erase", []> {
        let summary = "Erase Op";
        let arguments = (ins Brainfuck_CellType:$input);
        let results = (outs Brainfuck_CellType:$output);
        let assemblyFormat = "$input attr-dict `:` type($input) `to` type($output)";

}


#endif //