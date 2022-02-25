#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>
#include <utility>

#include "llvm/ADT/Optional.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Alignment.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/GVN.h"


std::ostream & operator<<(std::ostream & os, const std::exception & e) {
    return os << e.what();
}

enum class Instruction: uint8_t {
    moveRight,
    moveLeft,
    increment,
    decrement,
    output,
    input,
    startLoop,
    endLoop
};

class ParseError: public llvm::ErrorInfo<ParseError> {
public:
    enum class Kind: uint8_t {
        noLoopEnd,
        noLoopStart
    };

    static char ID;

private:
    Kind _kind;

public:
    ParseError() = delete;

    explicit ParseError(Kind kind): _kind(kind) {}

    Kind getKind() const { return _kind; }

    void log(llvm::raw_ostream & os) const override {
        switch (_kind) {
            case Kind::noLoopEnd: os << "Loop was not ended!"; break;
            case Kind::noLoopStart: os << "Loop was not started!"; break;
        }
    }

    std::error_code convertToErrorCode() const override {
        return llvm::inconvertibleErrorCode();
    }
};

char ParseError::ID = 0;


std::vector<Instruction> instructions;
std::vector<Instruction>::iterator currentInstruction;


llvm::LLVMContext context;
llvm::IRBuilder<> builder(context);
std::unique_ptr<llvm::Module> module;
std::unique_ptr<llvm::legacy::FunctionPassManager> passManager;

llvm::GlobalVariable * emptyString;
llvm::GlobalVariable * moveLeftErrorString;

llvm::StructType * fileStruct;

llvm::GlobalVariable * stdinP;
llvm::GlobalVariable * stderrP;

llvm::Function * mallocFunction;
llvm::Function * reallocFunction;
llvm::Function * freeFunction;
llvm::Function * strlenFunction;
llvm::Function * getlineFunction;
llvm::Function * fputsFunction;
llvm::Function * putcharFunction;

llvm::Function * moveRightFunction;
llvm::Function * inputFunction;
llvm::Function * mainFunction;

llvm::BasicBlock * errorBlock;


llvm::AllocaInst * cellsAlloca;
llvm::AllocaInst * cellsLengthAlloca;
llvm::AllocaInst * currentCellAlloca;
llvm::AllocaInst * currentLineAlloca;
llvm::AllocaInst * lengthAlloca;
llvm::AllocaInst * currentPositionAlloca;


llvm::cl::OptionCategory compilerCategory("Compiler Options", "Options for controlling the compilation process.");

llvm::cl::opt<std::string> outputFileNameOption("o",
                                                llvm::cl::desc("Write output to <file>"),
                                                llvm::cl::value_desc("file"),
                                                llvm::cl::cat(compilerCategory));

llvm::cl::alias outputFileNameAlias("output-file",
                                    llvm::cl::desc("Alias for -o"),
                                    llvm::cl::value_desc("file"),
                                    llvm::cl::aliasopt(outputFileNameOption),
                                    llvm::cl::cat(compilerCategory));

llvm::cl::opt<std::string> inputFileNameOption(llvm::cl::Positional,
                                               llvm::cl::desc("<input file>"),
                                               llvm::cl::Required,
                                               llvm::cl::cat(compilerCategory));


static void createSTDIO() {
    llvm::Type * i8 = llvm::Type::getInt8Ty(context);
    llvm::Type * i8Ptr = llvm::Type::getInt8PtrTy(context);
    llvm::Type * i16 = llvm::Type::getInt16Ty(context);
    llvm::Type * i32 = llvm::Type::getInt32Ty(context);
    llvm::Type * i64 = llvm::Type::getInt64Ty(context);

    llvm::Type * f1 = llvm::FunctionType::get(i32, {i8Ptr}, false)->getPointerTo();
    llvm::Type * f2 = llvm::FunctionType::get(i32, {i8Ptr, i8Ptr, i32}, false)->getPointerTo();
    llvm::Type * f3 = llvm::FunctionType::get(i32, {i8Ptr, i64, i32}, false)->getPointerTo();

    llvm::ArrayType * a1 = llvm::ArrayType::get(i8, 3);
    llvm::ArrayType * a2 = llvm::ArrayType::get(i8, 1);

    llvm::StructType * buf = llvm::StructType::create("__sbuf", i8Ptr, i32);
    llvm::StructType * fileX = llvm::StructType::create(context, "__sFILEX");

    fileStruct = llvm::StructType::create("__sFILE", i8Ptr, i32, i32, i16, i16, buf, i32, i8Ptr, f1, f2, f3, f2, buf, fileX->getPointerTo(), i32, a1, a2, buf, i32, i64);

    stdinP = new llvm::GlobalVariable(fileStruct->getPointerTo(), false, llvm::GlobalValue::ExternalLinkage,
                                      nullptr, "__stdinp");
    stdinP->setAlignment(llvm::Align(8));
    module->getGlobalList().push_back(stdinP);

    stderrP = new llvm::GlobalVariable(fileStruct->getPointerTo(), false, llvm::GlobalValue::ExternalLinkage,
                                      nullptr, "__stderrp");
    stderrP->setAlignment(llvm::Align(8));
    module->getGlobalList().push_back(stderrP);
}

static llvm::Function * createFunction(llvm::Type * returnType, const std::vector<llvm::Type *> & params, bool isVarArg, llvm::StringRef name) {
    llvm::FunctionType * type = llvm::FunctionType::get(returnType, params, isVarArg);
    return llvm::Function::Create(type, llvm::Function::ExternalLinkage, name, module.get());
}

static void createMoveRightFunction() {
    llvm::Type * i8Ptr = llvm::Type::getInt8PtrTy(context);
    llvm::Type * i8PtrPtr = i8Ptr->getPointerTo();
    llvm::Type * i64 = llvm::Type::getInt64Ty(context);
    llvm::Type * i64Ptr = llvm::Type::getInt64PtrTy(context);

    moveRightFunction = createFunction(llvm::Type::getVoidTy(context), {i8PtrPtr, i64Ptr, i64Ptr}, false, "moveRight");
    llvm::BasicBlock * entryBlock = llvm::BasicBlock::Create(context, "entry", moveRightFunction);
    builder.SetInsertPoint(entryBlock);

    llvm::AllocaInst * cellsPointerAlloca = builder.CreateAlloca(i8PtrPtr, nullptr, "cells");
    llvm::AllocaInst * cellsLengthPointerAlloca = builder.CreateAlloca(i64Ptr, nullptr, "cellsLength");
    llvm::AllocaInst * currentCellPointerAlloca = builder.CreateAlloca(i64Ptr, nullptr, "currentCell");

    auto args = moveRightFunction->args();
    llvm::Argument * cellsPointerArgument = args.begin();
    llvm::Argument * cellsLengthPointerArgument = args.begin() + 1;
    llvm::Argument * currentCellPointerArgument = args.begin() + 2;

    cellsPointerArgument->setName("cells");
    cellsLengthPointerArgument->setName("cellsLength");
    currentCellPointerArgument->setName("currentCell");

    builder.CreateStore(cellsPointerArgument, cellsPointerAlloca);
    builder.CreateStore(cellsLengthPointerArgument, cellsLengthPointerAlloca);
    builder.CreateStore(currentCellPointerArgument, currentCellPointerAlloca);

    llvm::Value * currentCellPointer = builder.CreateLoad(i64Ptr, currentCellPointerAlloca);
    llvm::Value * currentCell = builder.CreateLoad(i64, currentCellPointer);

    currentCell = builder.CreateAdd(builder.getInt64(1), currentCell, "incrementedCurrentCell");

    builder.CreateStore(currentCell, currentCellPointer);

    llvm::Value * cellsLengthPointer = builder.CreateLoad(i64Ptr, cellsLengthPointerAlloca);
    llvm::Value * cellsLength = builder.CreateLoad(i64, cellsLengthPointer);

    llvm::Value * resizeCells = builder.CreateICmpEQ(currentCell, cellsLength, "resizeCells");

    llvm::BasicBlock * thenBlock = llvm::BasicBlock::Create(context, "then", moveRightFunction);
    llvm::BasicBlock * mergeBlock = llvm::BasicBlock::Create(context, "merge");

    builder.CreateCondBr(resizeCells, thenBlock, mergeBlock);
    builder.SetInsertPoint(thenBlock);

    cellsLength = builder.CreateLoad(i64, cellsLengthPointer);

    cellsLength = builder.CreateMul(builder.getInt64(2), cellsLength, "doubledCellsLength");

    builder.CreateStore(cellsLength, cellsLengthPointer);

    llvm::Value * cellsPointer = builder.CreateLoad(i8PtrPtr, cellsPointerAlloca);
    llvm::Value * cells = builder.CreateLoad(i8Ptr, cellsPointer);

    cellsLength = builder.CreateLoad(i64, cellsLengthPointer);

    cells = builder.CreateCall(reallocFunction, {cells, cellsLength}, "reallocatedCells");

    builder.CreateStore(cells, cellsPointer);

    builder.CreateBr(mergeBlock);
    moveRightFunction->getBasicBlockList().push_back(mergeBlock);
    builder.SetInsertPoint(mergeBlock);

    builder.CreateRetVoid();

    llvm::verifyFunction(*moveRightFunction, &llvm::errs());

    passManager->run(*moveRightFunction);
}

static void createInputFunction() {
    llvm::Type * i8 = llvm::Type::getInt8Ty(context);
    llvm::Type * i8Ptr = llvm::Type::getInt8PtrTy(context);
    llvm::Type * i8PtrPtr = i8Ptr->getPointerTo();
    llvm::Type * i64 = llvm::Type::getInt64Ty(context);
    llvm::Type * i64Ptr = llvm::Type::getInt64PtrTy(context);

    inputFunction = createFunction(llvm::Type::getVoidTy(context), {i8Ptr, i64, i8PtrPtr, i64Ptr, i8PtrPtr}, false, "input");
    llvm::BasicBlock * entryBlock = llvm::BasicBlock::Create(context, "entry", inputFunction);
    builder.SetInsertPoint(entryBlock);

    llvm::AllocaInst * innerCellsAlloca = builder.CreateAlloca(i8Ptr, nullptr, "cells");
    llvm::AllocaInst * innerCurrentCellAlloca = builder.CreateAlloca(i64, nullptr, "currentCell");
    llvm::AllocaInst * currentLinePointerAlloca = builder.CreateAlloca(i8PtrPtr, nullptr, "currentLine");
    llvm::AllocaInst * lengthPointerAlloca = builder.CreateAlloca(i64Ptr, nullptr, "length");
    llvm::AllocaInst * currentPositionPointerAlloca = builder.CreateAlloca(i8PtrPtr, nullptr, "currentPosition");

    auto args = inputFunction->args();
    llvm::Argument * cellsArgument = args.begin();
    llvm::Argument * currentCellArgument = args.begin() + 1;
    llvm::Argument * currentLinePointerArgument = args.begin() + 2;
    llvm::Argument * lengthPointerArgument = args.begin() + 3;
    llvm::Argument * currentPositionPointerArgument = args.begin() + 4;

    cellsArgument->setName("cells");
    currentCellArgument->setName("currentCell");
    currentLinePointerArgument->setName("currentLine");
    lengthPointerArgument->setName("length");
    currentPositionPointerArgument->setName("currentPosition");

    builder.CreateStore(cellsArgument, innerCellsAlloca);
    builder.CreateStore(currentCellArgument, innerCurrentCellAlloca);
    builder.CreateStore(currentLinePointerArgument, currentLinePointerAlloca);
    builder.CreateStore(lengthPointerArgument, lengthPointerAlloca);
    builder.CreateStore(currentPositionPointerArgument, currentPositionPointerAlloca);

    llvm::Value * currentPositionPointer = builder.CreateLoad(i8PtrPtr, currentPositionPointerAlloca);
    llvm::Value * currentPosition = builder.CreateLoad(i8Ptr, currentPositionPointer);

    llvm::Value * currentLength = builder.CreateCall(strlenFunction, {currentPosition}, "currentLength");

    llvm::Value * getNewLine = builder.CreateICmpEQ(currentLength, builder.getInt64(0), "getNewLine");

    llvm::BasicBlock * thenBlock = llvm::BasicBlock::Create(context, "then", inputFunction);
    llvm::BasicBlock * mergeBlock = llvm::BasicBlock::Create(context, "merge");

    builder.CreateCondBr(getNewLine, thenBlock, mergeBlock);
    builder.SetInsertPoint(thenBlock);

    llvm::Value * currentLinePointer = builder.CreateLoad(i8PtrPtr, currentLinePointerAlloca);
    llvm::Value * length = builder.CreateLoad(i64Ptr, lengthPointerAlloca);
    llvm::Value * stdinV = builder.CreateLoad(fileStruct->getPointerTo(), stdinP);

    builder.CreateCall(getlineFunction, {currentLinePointer, length, stdinV});

    llvm::Value * currentLine = builder.CreateLoad(i8Ptr, currentLinePointer);

    builder.CreateStore(currentLine, currentPositionPointer);

    builder.CreateBr(mergeBlock);
    inputFunction->getBasicBlockList().push_back(mergeBlock);
    builder.SetInsertPoint(mergeBlock);

    currentPosition = builder.CreateLoad(i8Ptr, currentPositionPointer);
    llvm::Value * newCurrentPosition = builder.CreateGEP(i8, currentPosition, builder.getInt32(1));

    builder.CreateStore(newCurrentPosition, currentPositionPointer);

    currentPosition = builder.CreateLoad(i8Ptr, currentPositionPointer);
    llvm::Value * currentCharacter = builder.CreateLoad(i8, currentPosition);
    llvm::Value * cells = builder.CreateLoad(i8Ptr, innerCellsAlloca);
    llvm::Value * currentCell = builder.CreateLoad(i64, innerCurrentCellAlloca);

    llvm::Value * cellsGEP = builder.CreateGEP(i8, cells, currentCell);

    builder.CreateStore(currentCharacter, cellsGEP);

    builder.CreateRetVoid();

    llvm::verifyFunction(*inputFunction, &llvm::errs());

    passManager->run(*inputFunction);
}


static llvm::Error generateIR(std::vector<Instruction>::iterator loopStart = instructions.end()) {
    llvm::Type * i8 = llvm::Type::getInt8Ty(context);
    llvm::Type * i8Ptr = llvm::Type::getInt8PtrTy(context);
    llvm::Type * i64 = llvm::Type::getInt64Ty(context);

    llvm::BasicBlock * loopBlock = nullptr;
    llvm::BasicBlock * mergeBlock = nullptr;

    while (currentInstruction != instructions.end()) {
        switch (*currentInstruction) {
            case Instruction::moveRight: {
                builder.CreateCall(moveRightFunction, {cellsAlloca, cellsLengthAlloca, currentCellAlloca});

                break;
            }
            case Instruction::moveLeft: {
                llvm::Value * currentCell = builder.CreateLoad(i64, currentCellAlloca);

                llvm::Value * returnWithError = builder.CreateICmpEQ(currentCell, builder.getInt64(0), "returnWithError");

                llvm::BasicBlock * moveLeftBlock = llvm::BasicBlock::Create(context, "moveLeft", mainFunction);

                builder.CreateCondBr(returnWithError, errorBlock, moveLeftBlock);

                builder.SetInsertPoint(moveLeftBlock);

                currentCell = builder.CreateLoad(i64, currentCellAlloca);

                currentCell = builder.CreateSub(currentCell, builder.getInt64(1), "decrementedCurrentCell");

                builder.CreateStore(currentCell, currentCellAlloca);

                break;
            }
            case Instruction::increment: {
                uint8_t increment = 1;
                while ((currentInstruction + 1) != instructions.end() && *(currentInstruction + 1) == Instruction::increment) {
                    ++increment;
                    ++currentInstruction;
                }

                llvm::Value * cells = builder.CreateLoad(i8Ptr, cellsAlloca);
                llvm::Value * currentCell = builder.CreateLoad(i64, currentCellAlloca);

                llvm::Value * cellsGEP = builder.CreateGEP(i8, cells, currentCell);

                llvm::Value * currentCellValue = builder.CreateLoad(i8, cellsGEP);

                currentCellValue = builder.CreateAdd(currentCellValue, builder.getInt8(increment), "incrementedCurrentCellValue");

                builder.CreateStore(currentCellValue, cellsGEP);

                break;
            }
            case Instruction::decrement: {
                uint8_t decrement = 1;
                while ((currentInstruction + 1) != instructions.end() && *(currentInstruction + 1) == Instruction::decrement) {
                    ++decrement;
                    ++currentInstruction;
                }

                llvm::Value * cells = builder.CreateLoad(i8Ptr, cellsAlloca);
                llvm::Value * currentCell = builder.CreateLoad(i64, currentCellAlloca);

                llvm::Value * cellsGEP = builder.CreateGEP(i8, cells, currentCell);

                llvm::Value * currentCellValue = builder.CreateLoad(i8, cellsGEP);

                currentCellValue = builder.CreateSub(currentCellValue, builder.getInt8(decrement), "decrementedCurrentCellValue");

                builder.CreateStore(currentCellValue, cellsGEP);

                break;
            }
            case Instruction::output: {
                llvm::Value * cells = builder.CreateLoad(i8Ptr, cellsAlloca);
                llvm::Value * currentCell = builder.CreateLoad(i64, currentCellAlloca);

                llvm::Value * cellsGEP = builder.CreateGEP(i8, cells, currentCell);

                llvm::Value * currentCellValue = builder.CreateLoad(i8, cellsGEP);

                currentCellValue = builder.CreateZExt(currentCellValue, llvm::Type::getInt32Ty(context));

                builder.CreateCall(putcharFunction, {currentCellValue});

                break;
            }
            case Instruction::input: {
                llvm::Value * cells = builder.CreateLoad(i8Ptr, cellsAlloca);
                llvm::Value * currentCell = builder.CreateLoad(i64, currentCellAlloca);
                builder.CreateCall(inputFunction, {cells, currentCell, currentLineAlloca, lengthAlloca, currentPositionAlloca});

                break;
            }
            case Instruction::startLoop: {
                if (loopStart == currentInstruction) {
                    loopBlock = llvm::BasicBlock::Create(context, "loop", mainFunction);
                    llvm::BasicBlock * thenBlock = llvm::BasicBlock::Create(context, "then");
                    mergeBlock = llvm::BasicBlock::Create(context, "merge");

                    builder.CreateBr(loopBlock);

                    builder.SetInsertPoint(loopBlock);

                    llvm::Value * cells = builder.CreateLoad(i8Ptr, cellsAlloca);
                    llvm::Value * currentCell = builder.CreateLoad(i64, currentCellAlloca);

                    llvm::Value * cellsGEP = builder.CreateGEP(i8, cells, currentCell);

                    llvm::Value * currentCellValue = builder.CreateLoad(i8, cellsGEP);

                    llvm::Value * continueLoop = builder.CreateICmpNE(currentCellValue, builder.getInt8(0), "breakLoop");

                    builder.CreateCondBr(continueLoop, thenBlock, mergeBlock);

                    loopBlock = builder.GetInsertBlock();

                    mainFunction->getBasicBlockList().push_back(thenBlock);
                    builder.SetInsertPoint(thenBlock);
                } else {
                    if (auto error = generateIR(currentInstruction)) return error;
                }

                break;
            }
            case Instruction::endLoop: {
                if (!loopBlock) return llvm::make_error<ParseError>(ParseError::Kind::noLoopStart);

                builder.CreateBr(loopBlock);

                mainFunction->getBasicBlockList().push_back(mergeBlock);
                builder.SetInsertPoint(mergeBlock);

                return llvm::Error::success();
            }
        }

        ++currentInstruction;
    }

    return loopBlock ? llvm::make_error<ParseError>(ParseError::Kind::noLoopEnd) : llvm::Error::success();
}


llvm::Optional<std::string> getSDKPath() {
    llvm::SmallString<128> tempFilePath;
    if (auto error = llvm::sys::fs::createTemporaryFile("sdkpath", "", tempFilePath)) {
        llvm::errs() << "Could not create temporary file: " << error.message();
        return llvm::None;
    }


    auto xcrunPath = llvm::sys::findProgramByName("xcrun");
    if (std::error_code errorCode = xcrunPath.getError()) {
        llvm::errs() << "Could not find xcrun: " << errorCode.message();
        return llvm::None;
    }

    llvm::StringRef xcrunArgs[] = {
        *xcrunPath,
        "--sdk",
        "macosx",
        "--show-sdk-path"
    };

    int xcrunReturn = llvm::sys::ExecuteAndWait(*xcrunPath, xcrunArgs, llvm::None, {llvm::None, {tempFilePath}, llvm::None});
    if (xcrunReturn) {
        llvm::errs() << "xcrun command failed.";
        return llvm::None;
    }


    std::ifstream tempFile((std::string)tempFilePath);
    std::stringstream buffer;

    char c;
    while (tempFile.get(c) && c != '\n')
        buffer << c;

    return buffer.str();
}


int link(llvm::StringRef tempFilePath, llvm::StringRef outputFilePath) {
    auto sdkPath = getSDKPath();
    if (!sdkPath) {
        llvm::errs() << "Could not find path of MacOSK.sdk";
        return 1;
    }


    auto ldPath = llvm::sys::findProgramByName("ld");
    if (std::error_code errorCode = ldPath.getError()) {
        llvm::errs() << "Could not find ld: " << errorCode.message();
        return 1;
    }

    llvm::StringRef ldArgs[] = {
        *ldPath,
        "-syslibroot",
        sdkPath.getValue(),
        "-lSystem",
        tempFilePath,
        "-o",
        outputFilePath
    };

    int ldReturn = llvm::sys::ExecuteAndWait(*ldPath, ldArgs);
    if (ldReturn) {
        llvm::errs() << "ld command failed.";
        return 1;
    }

    llvm::outs() << "Generated " << outputFilePath << "\n";

    return 0;
}


int main(int argc, const char ** argv) {
    llvm::cl::HideUnrelatedOptions(compilerCategory);

    llvm::cl::SetVersionPrinter([](llvm::raw_ostream & os) {
        os << "BrainFuck compiler version 1.0.0\n";
    });

    llvm::cl::ParseCommandLineOptions(argc, argv, "Simple BrainFuck to executable compiler", nullptr, nullptr, true);


    std::string inputFileName = inputFileNameOption.getValue();

    std::ifstream file(inputFileName);
    llvm::SmallString<32> fileBaseName = llvm::sys::path::stem(inputFileName);
    llvm::SmallString<128> outputFilePath(outputFileNameOption.getValue());

    if (outputFilePath.empty()) {
        outputFilePath = inputFileName;
        llvm::sys::path::replace_extension(outputFilePath, "");
    }

    char c;
    while (file.get(c)) {
        switch (c) {
            case '>': instructions.push_back(Instruction::moveRight); break;
            case '<': instructions.push_back(Instruction::moveLeft); break;
            case '+': instructions.push_back(Instruction::increment); break;
            case '-': instructions.push_back(Instruction::decrement); break;
            case '.': instructions.push_back(Instruction::output); break;
            case ',': instructions.push_back(Instruction::input); break;
            case '[': instructions.push_back(Instruction::startLoop); break;
            case ']': instructions.push_back(Instruction::endLoop); break;
            default: break;
        }
    }

    currentInstruction = instructions.begin();

    module = std::make_unique<llvm::Module>(fileBaseName, context);
    module->setSourceFileName(argv[1]);

    passManager = std::make_unique<llvm::legacy::FunctionPassManager>(module.get());
    passManager->add(llvm::createInstructionCombiningPass());
    passManager->add(llvm::createReassociatePass());
    passManager->add(llvm::createGVNPass());
    passManager->add(llvm::createCFGSimplificationPass());

    passManager->doInitialization();

    createSTDIO();

    mallocFunction = createFunction(llvm::Type::getInt8PtrTy(context), {llvm::Type::getInt64Ty(context)}, false, "malloc");
    reallocFunction = createFunction(llvm::Type::getInt8PtrTy(context), {llvm::Type::getInt8PtrTy(context), llvm::Type::getInt64Ty(context)}, false, "realloc");
    freeFunction = createFunction(llvm::Type::getVoidTy(context), {llvm::Type::getInt8PtrTy(context)}, false, "free");
    strlenFunction = createFunction(llvm::Type::getInt64Ty(context), {llvm::Type::getInt8PtrTy(context)}, false, "strlen");
    getlineFunction = createFunction(llvm::Type::getInt64Ty(context), {llvm::Type::getInt8PtrTy(context)->getPointerTo(), llvm::Type::getInt64PtrTy(context), fileStruct->getPointerTo()}, false, "getline");
    fputsFunction = createFunction(llvm::Type::getInt32Ty(context), {llvm::Type::getInt8PtrTy(context), fileStruct->getPointerTo()}, false, "fputs");
    putcharFunction = createFunction(llvm::Type::getInt32Ty(context), {llvm::Type::getInt32Ty(context)}, false, "putchar");

    createMoveRightFunction();
    createInputFunction();

    mainFunction = createFunction(llvm::Type::getInt32Ty(context), {}, false, "main");
    llvm::BasicBlock * mainEntryBlock = llvm::BasicBlock::Create(context, "entry", mainFunction);
    errorBlock = llvm::BasicBlock::Create(context, "error");
    llvm::BasicBlock * returnBlock = llvm::BasicBlock::Create(context, "return");

    builder.SetInsertPoint(mainEntryBlock);

    emptyString = builder.CreateGlobalString("", "emptyString");
    moveLeftErrorString = builder.CreateGlobalString("Error: Cannot move pointer to negative cell!\n", "moveLeftErrorString");

    cellsAlloca = builder.CreateAlloca(llvm::Type::getInt8PtrTy(context), nullptr, "cells");
    cellsLengthAlloca = builder.CreateAlloca(llvm::Type::getInt64Ty(context), nullptr, "cellsLength");
    currentCellAlloca = builder.CreateAlloca(llvm::Type::getInt64Ty(context), nullptr, "currentCell");
    currentLineAlloca = builder.CreateAlloca(llvm::Type::getInt8PtrTy(context), nullptr, "currentLine");
    lengthAlloca = builder.CreateAlloca(llvm::Type::getInt64Ty(context), nullptr, "length");
    currentPositionAlloca = builder.CreateAlloca(llvm::Type::getInt8PtrTy(context), nullptr, "currentPosition");

    llvm::Value * cells = builder.CreateCall(mallocFunction, {builder.getInt64(4)});

    builder.CreateStore(cells, cellsAlloca);
    builder.CreateStore(builder.getInt64(4), cellsLengthAlloca);
    builder.CreateStore(builder.getInt64(0), currentCellAlloca);
    builder.CreateStore(llvm::ConstantPointerNull::get(llvm::Type::getInt8PtrTy(context)), currentLineAlloca);
    builder.CreateStore(builder.getInt64(0), lengthAlloca);

    llvm::Value * castedEmptyString = builder.CreateBitCast(emptyString, llvm::Type::getInt8PtrTy(context), "emptyString");
    builder.CreateStore(castedEmptyString, currentPositionAlloca);

    if (auto error = generateIR()) {
        mainFunction->eraseFromParent();

        llvm::errs() << "Parsing Error: " << error << "\n";

        return 1;
    }

    builder.CreateBr(returnBlock);

    llvm::BasicBlock * lastBlock = builder.GetInsertBlock();

    mainFunction->getBasicBlockList().push_back(errorBlock);
    builder.SetInsertPoint(errorBlock);

    llvm::Value * castedErrorString = builder.CreateBitCast(moveLeftErrorString, llvm::Type::getInt8PtrTy(context), "errorString");
    llvm::Value * stderrV = builder.CreateLoad(fileStruct->getPointerTo(), stderrP);
    builder.CreateCall(fputsFunction, {castedErrorString, stderrV});

    builder.CreateBr(returnBlock);

    errorBlock = builder.GetInsertBlock();

    mainFunction->getBasicBlockList().push_back(returnBlock);
    builder.SetInsertPoint(returnBlock);

    llvm::PHINode * phi = builder.CreatePHI(llvm::Type::getInt32Ty(context), 2, "returnValue");
    phi->addIncoming(builder.getInt32(0), lastBlock);
    phi->addIncoming(builder.getInt32(1), errorBlock);

    cells = builder.CreateLoad(llvm::Type::getInt8PtrTy(context), cellsAlloca);
    builder.CreateCall(freeFunction, {cells});

    llvm::Value * currentLine = builder.CreateLoad(llvm::Type::getInt8PtrTy(context), currentLineAlloca);
    builder.CreateCall(freeFunction, {currentLine});

    builder.CreateRet(phi);

    llvm::verifyFunction(*mainFunction, &llvm::errs());

    passManager->run(*mainFunction);

    llvm::verifyModule(*module, &llvm::errs());


    llvm::InitializeAllTargetInfos();
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmParsers();
    llvm::InitializeAllAsmPrinters();

    std::string targetTriple = llvm::sys::getDefaultTargetTriple();
    module->setTargetTriple(targetTriple);

    std::string errorString;
    const llvm::Target * target = llvm::TargetRegistry::lookupTarget(targetTriple, errorString);

    if (!target) {
        llvm::errs() << errorString;
        return 1;
    }

    llvm::TargetOptions options;
    auto relocationModel = llvm::Optional<llvm::Reloc::Model>();
    auto targetMachine = target->createTargetMachine(targetTriple, "generic", "", options, relocationModel);

    module->setDataLayout(targetMachine->createDataLayout());


    int tempFileDescriptor;
    llvm::SmallString<128> tempFilePath;
    if (auto error = llvm::sys::fs::createTemporaryFile(fileBaseName, "o", tempFileDescriptor, tempFilePath)) {
        llvm::errs() << "Could not create temporary file: " << error.message();
        return 1;
    }

    {
        llvm::raw_fd_ostream outputFileStream(tempFileDescriptor, true);

        llvm::legacy::PassManager outputPassManager;
        auto outputFileType = llvm::CGFT_ObjectFile;

        if (targetMachine->addPassesToEmitFile(outputPassManager, outputFileStream, nullptr, outputFileType)) {
            llvm::errs() << "Target machine cannot emit a file of this type";
            return 1;
        }

        outputPassManager.run(*module);
        outputFileStream.flush();
    }


    return link(tempFilePath, outputFilePath);
}
