#include <fstream>
#include <memory>
#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <unordered_map>
#include <iostream>

#include "llvm-c/Core.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/LinkAllPasses.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Analysis/InstructionSimplify.h"
#include "llvm/Analysis/CallGraph.h"
//#include "llvm/Analysis/AnalysisManager.h"

#include "llvm/IR/LLVMContext.h"

#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Pass.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Support/SourceMgr.h"
#include <memory>




using namespace llvm;
using namespace std;

static void DoInlining(Module *);

static void summarize(Module *M);

static void print_csv_file(std::string outputfile);

static cl::opt<std::string>
        InputFilename(cl::Positional, cl::desc("<input bitcode>"), cl::Required, cl::init("-"));

static cl::opt<std::string>
        OutputFilename(cl::Positional, cl::desc("<output bitcode>"), cl::Required, cl::init("out.bc"));

static cl::opt<bool>
        InlineHeuristic("inline-heuristic",
              cl::desc("Use student's inlining heuristic."),
              cl::init(false));

static cl::opt<bool>
        InlineConstArg("inline-require-const-arg",
              cl::desc("Require function call to have at least one constant argument."),
              cl::init(false));

static cl::opt<int>
        InlineFunctionSizeLimit("inline-function-size-limit",
              cl::desc("Biggest size of function to inline."),
              cl::init(1000000000));

static cl::opt<int>
        InlineGrowthFactor("inline-growth-factor",
              cl::desc("Largest allowed program size increase factor (e.g. 2x)."),
              cl::init(20));


static cl::opt<bool>
        NoInline("no-inline",
              cl::desc("Do not perform inlining."),
              cl::init(false));


static cl::opt<bool>
        NoPreOpt("no-preopt",
              cl::desc("Do not perform pre-inlining optimizations."),
              cl::init(false));

static cl::opt<bool>
        NoPostOpt("no-postopt",
              cl::desc("Do not perform post-inlining optimizations."),
              cl::init(false));

static cl::opt<bool>
        Verbose("verbose",
                    cl::desc("Verbose stats."),
                    cl::init(false));

static cl::opt<bool>
        NoCheck("no",
                cl::desc("Do not check for valid IR."),
                cl::init(false));


static llvm::Statistic nInstrBeforeOpt = {"", "nInstrBeforeOpt", "number of instructions"};
static llvm::Statistic nInstrBeforeInline = {"", "nInstrPreInline", "number of instructions"};
static llvm::Statistic nInstrAfterInline = {"", "nInstrAfterInline", "number of instructions"};
static llvm::Statistic nInstrPostOpt = {"", "nInstrPostOpt", "number of instructions"};


static void countInstructions(Module *M, llvm::Statistic &nInstr) {
  for (auto i = M->begin(); i != M->end(); i++) {
    for (auto j = i->begin(); j != i->end(); j++) {
      for (auto k = j->begin(); k != j->end(); k++) {
	nInstr++;
      }
    }
  }
}


int main(int argc, char **argv) {
    // Parse command line arguments
    cl::ParseCommandLineOptions(argc, argv, "llvm system compiler\n");

    // Handle creating output files and shutting down properly
    llvm_shutdown_obj Y;  // Call llvm_shutdown() on exit.
    LLVMContext Context;

    // LLVM idiom for constructing output file.
    std::unique_ptr<ToolOutputFile> Out;
    std::string ErrorInfo;
    std::error_code EC;
    Out.reset(new ToolOutputFile(OutputFilename.c_str(), EC,
                                 sys::fs::OF_None));

    EnableStatistics();

    // Read in module
    SMDiagnostic Err;
    std::unique_ptr<Module> M;
    M = parseIRFile(InputFilename, Err, Context);

    // If errors, fail
    if (M.get() == 0)
    {
        Err.print(argv[0], errs());
        return 1;
    }

    countInstructions(M.get(),nInstrBeforeOpt);
    
    if (!NoPreOpt) {
      legacy::PassManager Passes;
      Passes.add(createPromoteMemoryToRegisterPass());    
      Passes.add(createEarlyCSEPass());
      Passes.add(createSCCPPass());
      Passes.add(createAggressiveDCEPass());
      Passes.add(createVerifierPass());
      Passes.run(*M);  
    }

    countInstructions(M.get(),nInstrBeforeInline);    

    if (!NoInline) {
        DoInlining(M.get());
    }

    countInstructions(M.get(),nInstrAfterInline);
    
    if (!NoPostOpt) {
      legacy::PassManager Passes;
      Passes.add(createPromoteMemoryToRegisterPass());    
      Passes.add(createEarlyCSEPass());
      Passes.add(createSCCPPass());
      Passes.add(createAggressiveDCEPass());
      Passes.add(createVerifierPass());
      Passes.run(*M);  
    }

    countInstructions(M.get(),nInstrPostOpt);
    
    // Collect statistics on Module
    summarize(M.get());
    print_csv_file(OutputFilename);

    if (Verbose)
        PrintStatistics(errs());

    // Verify integrity of Module, do this by default
    if (!NoCheck)
    {
        legacy::PassManager Passes;
        Passes.add(createVerifierPass());
        Passes.run(*M.get());
    }

    // Write final bitcode
    WriteBitcodeToFile(*M.get(), Out->os());
    Out->keep();

    return 0;
}

static llvm::Statistic nFunctions = {"", "Functions", "number of functions"};
static llvm::Statistic nInstructions = {"", "Instructions", "number of instructions"};
static llvm::Statistic nLoads = {"", "Loads", "number of loads"};
static llvm::Statistic nStores = {"", "Stores", "number of stores"};

static void summarize(Module *M) {
    for (auto i = M->begin(); i != M->end(); i++) {
        if (i->begin() != i->end()) {
            nFunctions++;
        }

        for (auto j = i->begin(); j != i->end(); j++) {
            for (auto k = j->begin(); k != j->end(); k++) {
                Instruction &I = *k;
                nInstructions++;
                if (isa<LoadInst>(&I)) {
                    nLoads++;
                } else if (isa<StoreInst>(&I)) {
                    nStores++;
                }
            }
        }
    }
}

static void print_csv_file(std::string outputfile)
{
    std::ofstream stats(outputfile + ".stats");
    auto a = GetStatistics();
    for (auto p : a) {
        stats << p.first.str() << "," << p.second << std::endl;
    }
    stats.close();
}

static llvm::Statistic Inlined = {"", "Inlined", "Inlined a call."};
static llvm::Statistic ConstArg = {"", "ConstArg", "Call has a constant argument."};
static llvm::Statistic SizeReq = {"", "SizeReq", "Call has a constant argument."};


#include "llvm/Transforms/Utils/Cloning.h"

int beginningInstCount = 0;
int currentInstCount = 0;


static int totalInstCount(Module *M) {
	int count = 0;
  for (auto i = M->begin(); i != M->end(); i++) {
    for (auto j = i->begin(); j != i->end(); j++) {
      for (auto k = j->begin(); k != j->end(); k++) {
		  count++;

      }
    }
  }
  return count;
}

map<Function *, int> FuncFreq;
static void DoInlining(Module *M) {
  // Implement a function to perform function inlining

  /*
    CallInst *CI = ....; // a call instruction
    InlineFunctionInfo IFI;
    InlineFunction(*CI, IFI);
	
  */
	std::set<CallInst*> worklist;  //create a worklist to put the function calls into
	
	for (auto f = M->begin(); f != M->end(); f++) {
        for(auto bb= f->begin(); bb!=f->end(); bb++){
  	  for(auto i = bb->begin(); i != bb->end(); i++)
  	    {
			Instruction * I = &*i;
			
			if(isa<CallInst>(I)){ // run through instructions and if the instruction is a call, check to make sure it is viable
				
				CallInst * instructionCall = dyn_cast<CallInst>(I); // cast instruction to function call
				Function *calledFunction = instructionCall->getCalledFunction(); //get called function
				
				FuncFreq[calledFunction] = FuncFreq[calledFunction] + 1;
				
				if((calledFunction == nullptr || calledFunction->isDeclaration() || calledFunction->begin() == calledFunction->end() ) ){
					continue; // if the instruction is a declaration, continue 
				}
				
				
				InlineResult temp = isInlineViable(*calledFunction);
				if(temp.isSuccess()){
					worklist.insert(instructionCall); // is the instruction is a viable inline, add it to worklist
				}
				
				
				
				
			}
			else{
				continue;
			}
			
		}
	
	}
  	  
	}
	
	beginningInstCount = totalInstCount(M); //find the original size of the module
	 int maxInstCountWithGF = beginningInstCount * InlineGrowthFactor; // the max amount of instructions will be the original size times the GF

	int currentInstCount = beginningInstCount; // create a current inst count variable
	
	map<Function *, int> FuncSize; // create a map to hold the function sizes for quicker compiling 
	
	
	for(auto &func : *M){
		FuncSize[&func] = func.getInstructionCount(); //map function sizes to map
	}
	
	
	
	double totalWorklistSize = worklist.size();
	
	//----------------------worklist inlining------------------------------
    while(!worklist.empty()) 
      {
		  double currentWorklistSize = worklist.size();
		  
		  int percent =  int ((1 - (currentWorklistSize/totalWorklistSize)) * 100);
		  
		  if(percent % 5 == 0){
			  cout << percent << "%" << endl; // create a quick percentage counter for compiling
		  }
		  
		 
        // Get the first item 
  	CallInst *newCall = *(worklist.begin());
    Function *calledFunction = newCall->getCalledFunction();//get called function
	
	if(InlineHeuristic){
		/*
		int allocaCount = 0;
		for(auto bb= calledFunction->begin(); bb!=calledFunction->end(); bb++){
			for(auto i = bb->begin(); i != bb->end(); i++){
				Instruction * I = &*i;
				if(isa<AllocaInst>(I)){
					allocaCount++;
					cout << "alloca count" << endl;
				}
				}
			}
		*/
		
			
			if(FuncFreq[calledFunction] > 2){
				InlineFunctionInfo IFI;
				InlineFunction(*newCall, IFI);
				worklist.erase(worklist.begin());
				Inlined++;
				cout << "inlined" << endl;
				
			}else{
				worklist.erase(worklist.begin());
			}
	}
	else{
	
	bool funcSizeLimit = false;
	bool growthFactor = false;
	bool reqConstantArg = true; // set flags to default
	
	if(FuncSize[calledFunction] < InlineFunctionSizeLimit){
		funcSizeLimit = true; // if the size of the function is less than the limit, set to high
	}
	
	
	if((maxInstCountWithGF) > (currentInstCount + FuncSize[calledFunction])){
		growthFactor = true; // if the instruction count of the current function will exceed growth factor, return and stop inlining, else, inline function
	}else{
		return;
	}
	
	if(InlineConstArg){
		if(calledFunction->getNumOperands() == 0){
			//do nothing if no operands
		}else{
			for (auto &arg : calledFunction->args()) { // if arg is not a constant, turn flag to false
        		if (!(isa<Constant>(arg))) {
          		  reqConstantArg = false;
          		break;	}
			}
		}
	}
	
	//for growth factor, I need to recount the # of instructions in the module every time (can create a function for this) and then compare the number of instructions added
	// to orignial size times the growth factor, if the function added exceeds the growth factor, stop inlining

	if(funcSizeLimit && growthFactor && reqConstantArg){ // if all conditions are met, inline the function and erase from worklist
		currentInstCount = currentInstCount + FuncSize[calledFunction];
		InlineFunctionInfo IFI;
		InlineFunction(*newCall, IFI);
		worklist.erase(worklist.begin());
		Inlined++;
		ConstArg++;
		SizeReq++;

	}else{
		worklist.erase(worklist.begin()); // erase from worklist if conditions are not met
	
	}
	}
	
	  }
	
	  //---------------------------------------------
	
	 
}




