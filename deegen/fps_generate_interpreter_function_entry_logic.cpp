#include "anonymous_file.h"
#include "fps_main.h"
#include "transactional_output_file.h"

#include "deegen_function_entry_logic_creator.h"
#include "llvm/Linker/Linker.h"

namespace {

using namespace dast;

// If the function takes <= threshold fixed parameters, it will use the specialized implementation
//
constexpr size_t x_specializeThresholdForNonVarargsFunction = x_use_som_call_semantics ? 0 : 6;
constexpr size_t x_specializeThresholdForVarargsFunction = x_use_som_call_semantics ? 0 : 6;

struct GeneratorContext
{
    GeneratorContext()
    {
        context = std::make_unique<llvm::LLVMContext>();
    }

    void LinkInModule(std::unique_ptr<llvm::Module> m)
    {
        using namespace llvm;
        if (module.get() == nullptr)
        {
            module = std::move(m);
        }
        else
        {
            Linker linker(*module.get());
            // linkInModule returns true on error
            //
            ReleaseAssert(linker.linkInModule(std::move(m)) == false);
        }
    }

    void GenerateImplementations()
    {
        for (size_t i = 0; i <= x_specializeThresholdForNonVarargsFunction; i++)
        {
            DeegenFunctionEntryLogicCreator ifi(*context.get(), DeegenEngineTier::Interpreter, false /*takesVarArgs*/, i /*specialziedFixedParams*/);
            novaNames.push_back(ifi.GetFunctionName());
            LinkInModule(ifi.GetInterpreterModule());
        }

        {
            DeegenFunctionEntryLogicCreator ifi(*context.get(), DeegenEngineTier::Interpreter, false /*takesVarArgs*/, static_cast<size_t>(-1) /*specialziedFixedParams*/);
            generalNovaName = ifi.GetFunctionName();
            LinkInModule(ifi.GetInterpreterModule());
        }

        for (size_t i = 0; i <= x_specializeThresholdForVarargsFunction; i++)
        {
            DeegenFunctionEntryLogicCreator ifi(*context.get(), DeegenEngineTier::Interpreter, true /*takesVarArgs*/, i /*specialziedFixedParams*/);
            vaNames.push_back(ifi.GetFunctionName());
            LinkInModule(ifi.GetInterpreterModule());
        }

        {
            DeegenFunctionEntryLogicCreator ifi(*context.get(), DeegenEngineTier::Interpreter, true /*takesVarArgs*/, static_cast<size_t>(-1) /*specialziedFixedParams*/);
            generalVaName = ifi.GetFunctionName();
            LinkInModule(ifi.GetInterpreterModule());
        }

        {
            std::unique_ptr<llvm::Module> tierUpImpl = DeegenFunctionEntryLogicCreator::GenerateInterpreterTierUpOrOsrEntryImplementation(*context.get(), true /*isTierUp*/);
            LinkInModule(std::move(tierUpImpl));
        }

        {
            std::unique_ptr<llvm::Module> tierUpImpl = DeegenFunctionEntryLogicCreator::GenerateInterpreterTierUpOrOsrEntryImplementation(*context.get(), false /*isTierUp*/);
            LinkInModule(std::move(tierUpImpl));
        }
    }

    std::string WARN_UNUSED GenerateHeaderFile()
    {
        AnonymousFile file;
        FILE* fp = file.GetFStream("w");
        FPS_EmitHeaderFileCommonHeader(fp);

        fprintf(fp, "\nnamespace generated {\n\n");
        fprintf(fp, "extern const std::array<void(*)(), %d> x_interpreterEntryFuncListNoVa;\n", static_cast<int>(novaNames.size() + 1));
        fprintf(fp, "extern const std::array<void(*)(), %d> x_interpreterEntryFuncListVa;\n", static_cast<int>(vaNames.size() + 1));

        fprintf(fp, "\nvoid* WARN_UNUSED GetGuestLanguageFunctionEntryPointForInterpreter(bool takeVarArgs, size_t numFixedParams) {\n");
        fprintf(fp, "    if (x_use_som_call_semantics) {\n");
        fprintf(fp, "        TestAssert(!takeVarArgs);\n");
        fprintf(fp, "        return reinterpret_cast<void*>(x_interpreterEntryFuncListNoVa[x_interpreterEntryFuncListNoVa.size() - 1]);\n");
        fprintf(fp, "    }\n");
        fprintf(fp, "    if (takeVarArgs) {\n");
        fprintf(fp, "        return reinterpret_cast<void*>(x_interpreterEntryFuncListVa[std::min(numFixedParams, x_interpreterEntryFuncListVa.size() - 1)]);\n");
        fprintf(fp, "    } else {\n");
        fprintf(fp, "        return reinterpret_cast<void*>(x_interpreterEntryFuncListNoVa[std::min(numFixedParams, x_interpreterEntryFuncListNoVa.size() - 1)]);\n");
        fprintf(fp, "    }\n");
        fprintf(fp, "}\n\n");

        fprintf(fp, "}  // namespace generated\n");

        fclose(fp);

        return file.GetFileContents();
    }

    std::string WARN_UNUSED GenerateCppFile()
    {
        AnonymousFile file;
        FILE* fp = file.GetFStream("w");
        FPS_EmitCPPFileCommonHeader(fp);

        for (auto& name : novaNames)
        {
            EmitDeclaration(fp, name);
        }
        EmitDeclaration(fp, generalNovaName);

        for (auto& name : vaNames)
        {
            EmitDeclaration(fp, name);
        }
        EmitDeclaration(fp, generalVaName);

        fprintf(fp, "\nnamespace generated {\n\n");

        fprintf(fp, "extern const std::array<void(*)(), %d> x_interpreterEntryFuncListNoVa;\n", static_cast<int>(novaNames.size() + 1));
        fprintf(fp, "constexpr std::array<void(*)(), %d> x_interpreterEntryFuncListNoVa {\n", static_cast<int>(novaNames.size() + 1));
        for (auto& name : novaNames)
        {
            fprintf(fp, "    %s,\n", name.c_str());
        }
        fprintf(fp, "    %s };\n\n", generalNovaName.c_str());

        fprintf(fp, "extern const std::array<void(*)(), %d> x_interpreterEntryFuncListVa;\n", static_cast<int>(vaNames.size() + 1));
        fprintf(fp, "constexpr std::array<void(*)(), %d> x_interpreterEntryFuncListVa {\n", static_cast<int>(vaNames.size() + 1));
        for (auto& name : vaNames)
        {
            fprintf(fp, "    %s,\n", name.c_str());
        }
        fprintf(fp, "    %s };\n\n", generalVaName.c_str());

        fprintf(fp, "}  // namespace generated\n");

        fclose(fp);

        return file.GetFileContents();
    }

    void EmitDeclaration(FILE* fp, const std::string& name)
    {
        // The actual function prototype doesn't matter here: it's sufficient to let the compiler know it's a function.
        // We always cast it to void* to store, and never directly call it from C++
        //
        fprintf(fp, "extern \"C\" void %s();\n", name.c_str());
    }

    std::unique_ptr<llvm::LLVMContext> context;
    std::unique_ptr<llvm::Module> module;

    std::vector<std::string> novaNames;
    std::string generalNovaName;
    std::vector<std::string> vaNames;
    std::string generalVaName;
};

}   // anonymous namespace

void FPS_GenerateInterpreterFunctionEntryLogic()
{
    GeneratorContext ctx;
    ctx.GenerateImplementations();

    std::string hdrFileContents = ctx.GenerateHeaderFile();
    TransactionalOutputFile hdrFile(cl_headerOutputFilename);
    hdrFile.write(hdrFileContents);

    std::string cppFileContents = ctx.GenerateCppFile();
    TransactionalOutputFile cppFile(cl_cppOutputFilename);
    cppFile.write(cppFileContents);

    std::string asmFileContents = CompileLLVMModuleToAssemblyFile(ctx.module.get(), llvm::Reloc::Static, llvm::CodeModel::Small);
    TransactionalOutputFile asmFile(cl_assemblyOutputFilename);
    asmFile.write(asmFileContents);

    std::string auditFilePath = FPS_GetAuditFilePath("interpreter_func_entry.s");
    TransactionalOutputFile auditFile(auditFilePath);
    auditFile.write(asmFileContents);

    auditFile.Commit();
    hdrFile.Commit();
    cppFile.Commit();
    asmFile.Commit();
}
