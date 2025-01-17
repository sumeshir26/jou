#ifndef _WIN32
    // readlink() stuff
    #define _POSIX_C_SOURCE 200112L
    #include <unistd.h>
#endif // _WIN32

#include <assert.h>
#include <libgen.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "jou_compiler.h"
#include "util.h"
#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/Linker.h>
#include <llvm-c/Transforms/PassManagerBuilder.h>


static void optimize(LLVMModuleRef module, int level)
{
    assert(1 <= level && level <= 3);

    LLVMPassManagerRef pm = LLVMCreatePassManager();

    /*
    The default settings should be fine for Jou because they work well for
    C and C++, and Jou is quite similar to C.
    */
    LLVMPassManagerBuilderRef pmbuilder = LLVMPassManagerBuilderCreate();
    LLVMPassManagerBuilderSetOptLevel(pmbuilder, level);
    LLVMPassManagerBuilderPopulateModulePassManager(pmbuilder, pm);
    LLVMPassManagerBuilderDispose(pmbuilder);

    LLVMRunPassManager(pm, module);
    LLVMDisposePassManager(pm);
}

static const char usage_fmt[] = "Usage: %s [--help] [--verbose] [-O0|-O1|-O2|-O3] FILENAME\n";
static const char long_help[] =
    "  --help           display this message\n"
    "  --verbose        display a lot of information about all compilation steps\n"
    "  -O0/-O1/-O2/-O3  set optimization level (0 = default, 3 = runs fastest)\n"
    ;

static void parse_arguments(int argc, char **argv, CommandLineFlags *flags, const char **filename)
{
    *flags = (CommandLineFlags){0};

    int i = 1;
    while (i < argc && argv[i][0] == '-') {
        if (!strcmp(argv[i], "--help")) {
            printf(usage_fmt, argv[0]);
            printf("%s", long_help);
            exit(0);
        } else if (!strcmp(argv[i], "--verbose")) {
            flags->verbose = true;
            i++;
        } else if (strlen(argv[i]) == 3
                && !strncmp(argv[i], "-O", 2)
                && argv[i][2] >= '0'
                && argv[i][2] <= '3')
        {
            flags->optlevel = argv[i][2] - '0';
            i++;
        } else {
            goto usage;
        }
    }

    if (i != argc-1)
        goto usage;
    *filename = argv[i];
    return;

usage:
    fprintf(stderr, usage_fmt, argv[0]);
    exit(2);
}


struct FileState {
    char *path;
    AstToplevelNode *ast;
    TypeContext typectx;
    LLVMModuleRef module;
};

struct ParseQueueItem {
    const char *filename;
    Location import_location;
};

struct CompileState {
    char *stdlib_path;
    CommandLineFlags flags;
    List(struct FileState) files;
    List(struct ParseQueueItem) parse_queue;
};

static void parse_file(struct CompileState *compst, const char *filename, const Location *import_location)
{
    for (struct FileState *fs = compst->files.ptr; fs < End(compst->files); fs++)
        if (!strcmp(fs->ast->location.filename, filename))
            return;  // already parsed this file

    struct FileState fs = { .path = strdup(filename) };

    FILE *f = fopen(fs.path, "rb");
    if (!f) {
        if (import_location)
            fail_with_error(*import_location, "cannot import from \"%s\": %s", filename, strerror(errno));
        else
            fail_with_error((Location){.filename=filename}, "cannot open file: %s", strerror(errno));
    }
    Token *tokens = tokenize(f, fs.path);
    fclose(f);
    if(compst->flags.verbose)
        print_tokens(tokens);

    fs.ast = parse(tokens, compst->stdlib_path);
    free_tokens(tokens);
    if(compst->flags.verbose)
        print_ast(fs.ast);

    for (AstToplevelNode *impnode = fs.ast; impnode->kind == AST_TOPLEVEL_IMPORT; impnode++) {
        Append(&compst->parse_queue, (struct ParseQueueItem){
            .filename = impnode->data.import.path,
            .import_location = impnode->location,
        });
    }

    Append(&compst->files, fs);
}

static void parse_all_pending_files(struct CompileState *compst)
{
    while (compst->parse_queue.len > 0) {
        struct ParseQueueItem it = Pop(&compst->parse_queue);
        parse_file(compst, it.filename, &it.import_location);
    }
    free(compst->parse_queue.ptr);
}

static void compile_ast_to_llvm(struct CompileState *compst, struct FileState *fs)
{
    if (compst->flags.verbose)
        printf("AST to LLVM IR: %s\n", fs->path);

    for (AstToplevelNode *impnode = fs->ast; impnode->kind == AST_TOPLEVEL_IMPORT; impnode++) {
        if (compst->flags.verbose)
            printf("  Add imported symbol: %s\n", impnode->data.import.symbol);

        struct FileState *src = NULL;
        for (struct FileState *p = compst->files.ptr; p < fs; p++) {
            if (!strcmp(p->path, impnode->data.import.path)) {
                src = p;
                break;
            }
        }
        assert(src);

        const Signature *sig = NULL;
        for (Signature *p = src->typectx.exports.ptr; p < End(src->typectx.exports); p++) {
            if (!strcmp(p->funcname, impnode->data.import.symbol)) {
                sig = p;
                break;
            }
        }
        if (!sig) {
            fail_with_error(
                impnode->location, "file \"%s\" does not contain a function named '%s'",
                impnode->data.import.path, impnode->data.import.symbol);
        }

        Append(&fs->typectx.function_signatures, copy_signature(sig));
    }

    CfGraphFile cfgfile = build_control_flow_graphs(fs->ast, &fs->typectx);
    free_ast(fs->ast);
    fs->ast = NULL;

    if(compst->flags.verbose)
        print_control_flow_graphs(&cfgfile);

    simplify_control_flow_graphs(&cfgfile);
    if(compst->flags.verbose)
        print_control_flow_graphs(&cfgfile);

    fs->module = codegen(&cfgfile, &fs->typectx);
    free_control_flow_graphs(&cfgfile);

    if(compst->flags.verbose)
        print_llvm_ir(fs->module, false);

    /*
    If this fails, it is not just users writing dumb code, it is a bug in this compiler.
    This compiler should always fail with an error elsewhere, or generate valid LLVM IR.
    */
    LLVMVerifyModule(fs->module, LLVMAbortProcessAction, NULL);

    if (compst->flags.optlevel) {
        if (compst->flags.verbose)
            printf("\n*** Optimizing %s... (level %d)\n\n\n", fs->path, compst->flags.optlevel);
        optimize(fs->module, compst->flags.optlevel);
        if(compst->flags.verbose)
            print_llvm_ir(fs->module, true);
    }
}

// argv[0] doesn't work as expected when Jou is ran through PATH.
char *find_this_executable(void)
{
    char *result;
    const char *err;

#ifdef _WIN32
    extern char *_pgmptr;  // A documented global variable in Windows. Full path to executable.
    result = strdup(_pgmptr);
    simplify_path(result);
    err = NULL;
#else
    int n = 10000;
    result = calloc(1, n);
    ssize_t ret = readlink("/proc/self/exe", result, n);

    if (ret < 0)
        err = strerror(errno);
    else if (ret == n) {
        static char s[100];
        sprintf(s, "path is more than %d bytes long", n);
        err=s;
    } else {
        assert(0<ret && ret<n);
        err = NULL;
    }
#endif

    if(err) {
        fprintf(stderr, "error finding current executable, needed to find the Jou standard library: %s\n", err);
        exit(1);
    }
    return result;
}

static char *find_stdlib()
{
    char *exe = find_this_executable();
    const char *exedir = dirname(exe);

    char *path = malloc(strlen(exedir) + 10);
    strcpy(path, exedir);
    strcat(path, "/stdlib");
    free(exe);

    char *iojou = malloc(strlen(path) + 10);
    sprintf(iojou, "%s/io.jou", path);
    struct stat st;
    if (stat(iojou, &st) != 0) {
        fprintf(stderr, "error: cannot find the Jou standard library in %s\n", path);
        exit(1);
    }
    free(iojou);

    return path;
}

int main(int argc, char **argv)
{
    init_types();

    struct CompileState compst = { .stdlib_path = find_stdlib() };
    const char *filename;
    parse_arguments(argc, argv, &compst.flags, &filename);

    parse_file(&compst, filename, NULL);
    parse_all_pending_files(&compst);

    // Reverse files, so that if foo imports bar, we compile bar first.
    // So far we have followed the imports.
    for (struct FileState *a = compst.files.ptr, *b = End(compst.files)-1; a<b; a++,b--)
    {
        struct FileState tmp = *a;
        *a = *b;
        *b = tmp;
    }

    for (struct FileState *fs = compst.files.ptr; fs < End(compst.files); fs++)
        compile_ast_to_llvm(&compst, fs);

    LLVMModuleRef main_module = LLVMModuleCreateWithName("main");
    for (struct FileState *fs = compst.files.ptr; fs < End(compst.files); fs++) {
        if (compst.flags.verbose)
            printf("Link %s\n", fs->path);
        if (LLVMLinkModules2(main_module, fs->module)) {
            fprintf(stderr, "error: LLVMLinkModules2() failed\n");
            return 1;
        }
        fs->module = NULL;  // consumed in linking
    }

    for (struct FileState *fs = compst.files.ptr; fs < End(compst.files); fs++) {
        free(fs->path);
        free_type_context(&fs->typectx);
    }
    free(compst.files.ptr);
    free(compst.stdlib_path);

    return run_program(main_module, &compst.flags);
}
