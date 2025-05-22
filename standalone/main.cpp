/*
 *
 *
 Copyright (c) 2007 Michael Haupt, Tobias Pape, Arne Bergmann
 Software Architecture Group, Hasso Plattner Institute, Potsdam, Germany
 http://www.hpi.uni-potsdam.de/swa/

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
*/

#include "common_utils.h"
#include "runtime_utils.h"
#include "som_compile_file.h"
#include "deegen_enter_vm_from_c.h"

#define DSOM_VERSION_MAJOR_NUMBER 0
#define DSOM_VERSION_MINOR_NUMBER 0
#define DSOM_VERSION_PATCH_NUMBER 1

extern const char* x_git_commit_hash;
constexpr const char* x_build_flavor_version_output = x_isTestBuild ? (x_isDebugBuild ? "**DEBUG** build" : "**TESTREL** build") : "release build";

static void NO_RETURN PrintUsageAndExit(const char* executable)
{
    fprintf(stderr, "DSOM v" PP_STRINGIFY(DSOM_VERSION_MAJOR_NUMBER) "."
            PP_STRINGIFY(DSOM_VERSION_MINOR_NUMBER) "." PP_STRINGIFY(DSOM_VERSION_PATCH_NUMBER) " (%s, %s)\n",
            x_git_commit_hash, x_build_flavor_version_output);
    fprintf(stderr, "Copyright (C) 2025 Haoran Xu.\n\n");
    fprintf(stderr, "Usage: %s [-options] [args...]\n\n", executable);
    fprintf(stderr, "where options include:\n");
    fprintf(stderr, "    -cp <directories separated by :>\n");
    fprintf(stderr, "        set search path for application classes\n");
    fprintf(stderr, "    -d  ignored\n");
    fprintf(stderr, "    -g  ignored\n");
    fprintf(stderr, "    -H  ignored\n");
    fprintf(stderr, "    -h  show this help\n");
    std::exit(0);
}

static void SetupClassPath(const std::string& cp)
{
    std::stringstream ss(cp);
    std::string token;

    while (getline(ss, token, ':'))
    {
        g_classLoadPaths.push_back(token);
    }
}

static bool GetClassPathExt(std::vector<std::string>& tokens, const std::string& arg)
{
#define EXT_TOKENS 2
    bool result = true;
    size_t fpIndex = arg.find_last_of('/');
    size_t ssepIndex = arg.find(".som");

    if (fpIndex == std::string::npos) {  // no new path
        // different from CSOM (see also HandleArguments):
        // we still want to strip the suffix from the filename, so
        // we set the start to -1, in order to start the substring
        // from character 0. npos is -1 too, but this is to make sure
        fpIndex = static_cast<size_t>(-1);
        // instead of returning here directly, we have to remember that
        // there is no new class path and return it later
        result = false;
    } else {
        tokens[0] = arg.substr(0, fpIndex);
    }

    // adding filename (minus ".som" if present) to second slot
    ssepIndex = ((ssepIndex != std::string::npos) && (ssepIndex > fpIndex))
        ? (ssepIndex - 1)
        : arg.length();
    tokens[1] = arg.substr(fpIndex + 1, ssepIndex - (fpIndex));
    return result;
}

std::vector<std::string> HandleArguments(int32_t argc, char** argv) {
    std::vector<std::string> vmArgs = std::vector<std::string>();

    for (int32_t i = 1; i < argc; ++i)
    {
        if (strncmp(argv[i], "-cp", 3) == 0)
        {
            if ((argc == i + 1) || !g_classLoadPaths.empty())
            {
                PrintUsageAndExit(argv[0]);
            }
            SetupClassPath(std::string(argv[++i]));
        }
        else if (strncmp(argv[i], "-d", 2) == 0)
        {
            /*ignored*/
        }
        else if (strncmp(argv[i], "-g", 2) == 0)
        {
            /*ignored*/
        }
        else if (strncmp(argv[i], "-H", 2) == 0)
        {
            /*ignored*/
        }
        else if ((strncmp(argv[i], "-h", 2) == 0) || (strncmp(argv[i], "--help", 6) == 0))
        {
            PrintUsageAndExit(argv[0]);
        }
        else
        {
            std::vector<std::string> extPathTokens = std::vector<std::string>(2);
            std::string const tmpString = std::string(argv[i]);
            if (GetClassPathExt(extPathTokens, tmpString))
            {
                g_classLoadPaths.push_back(extPathTokens[0]);
            }
            // Different from CSOM!!!:
            // In CSOM there is an else, where the original filename is pushed
            // into the vm_args. But unlike the class name in extPathTokens
            // (extPathTokens[1]) that could still have the .som suffix though.
            // So in SOM++ getClassPathExt will strip the suffix and add it to
            // extPathTokens even if there is no new class path present. So we
            // can in any case do the following:
            vmArgs.push_back(extPathTokens[1]);
        }
    }
    g_classLoadPaths.push_back(std::string("."));

    return vmArgs;
}

void DoWork(int argc, char** argv)
{
    std::vector<std::string> args = HandleArguments(argc, argv);

    if (args.empty())
    {
        // Interactive shell not supported
        //
        PrintUsageAndExit(argv[0]);
    }

    VM* vm = VM::Create();

    if (x_allow_interpreter_tier_up_to_baseline_jit)
    {
        vm->SetEngineStartingTier(VM::EngineStartingTier::BaselineJIT);
    }

    SOMInitializationResult r =  SOMBootstrapClassHierarchy();

    HeapPtr<FunctionObject> runFn = SOMGetMethodFromClass(r.m_systemClass, "initialize:");
    TestAssert(runFn != nullptr);

    CoroutineRuntimeContext* rc = vm->GetRootCoroutine();

    SOMObject* argsArr = SOMObject::AllocateArray(args.size());
    for (size_t i = 0; i < args.size(); i++)
    {
        SOMObject* str = SOMObject::AllocateString(args[i]);
        argsArr->m_data[i + 1] = TValue::Create<tObject>(TranslateToHeapPtr(str));
    }

    TValue aa[2];
    aa[0] = TValue::Create<tObject>(TranslateToHeapPtr(r.m_systemInstance));
    aa[1] = TValue::Create<tObject>(TranslateToHeapPtr(argsArr));

    DeegenEnterVMFromC(rc, runFn, rc->m_stackBegin, aa, 2 /*numArgs*/);

#ifdef ENABLE_SOM_PROFILE_FREQUENCY
    vm->PrintSOMFunctionFrequencyProfile();
#endif
}

int main(int argc, char** argv)
{
    DoWork(argc, argv);
    return 0;
}
