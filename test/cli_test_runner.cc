#include "doctest.h"
#include <cxxopts.hpp>
// has to go first as it violates our requirements

// has to go first, as it violates poisons
#include "core/proto/proto.h"

#include "absl/strings/match.h"
#include "absl/strings/str_split.h"
#include "ast/ast.h"
#include "ast/desugar/Desugar.h"
#include "ast/treemap/treemap.h"
#include "cfg/CFG.h"
#include "cfg/builder/builder.h"
#include "class_flatten/class_flatten.h"
#include "common/FileOps.h"
#include "common/common.h"
#include "common/formatting.h"
#include "common/sort.h"
#include "common/web_tracer_framework/tracing.h"
#include "core/Error.h"
#include "core/ErrorQueue.h"
#include "core/Unfreeze.h"
#include "core/serialize/serialize.h"
#include "definition_validator/validator.h"
#include "infer/infer.h"
#include "local_vars/local_vars.h"
#include "main/autogen/autogen.h"
#include "namer/namer.h"
#include "parser/parser.h"
#include "payload/binary/binary.h"
#include "resolver/resolver.h"
#include "rewriter/rewriter.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/spdlog.h"
#include "test/helpers/expectations.h"
#include "test/helpers/position_assertions.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <sys/types.h>
#include <vector>

namespace sorbet::test {
namespace spd = spdlog;
using namespace std;

string singleTest;

Expectations getInput(string singleTest);

class CFGCollectorAndTyper {
public:
    vector<unique_ptr<cfg::CFG>> cfgs;
    unique_ptr<ast::MethodDef> preTransformMethodDef(core::Context ctx, unique_ptr<ast::MethodDef> m) {
        if (m->symbol.data(ctx)->isOverloaded()) {
            return m;
        }
        auto cfg = cfg::CFGBuilder::buildFor(ctx.withOwner(m->symbol), *m);
        auto symbol = cfg->symbol;
        cfg = infer::Inference::run(ctx.withOwner(symbol), move(cfg));
        if (cfg) {
            for (auto &extension : ctx.state.semanticExtensions) {
                extension->typecheck(ctx, *cfg, m);
            }
        }
        cfgs.push_back(move(cfg));
        return m;
    }
};

UnorderedSet<string> knownExpectations = {
    "parse-tree",       "parse-tree-json", "parse-tree-whitequark", "desugar-tree", "desugar-tree-raw", "rewrite-tree",
    "rewrite-tree-raw", "index-tree",      "index-tree-raw",        "symbol-table", "symbol-table-raw", "name-tree",
    "name-tree-raw",    "resolve-tree",    "resolve-tree-raw",      "flatten-tree", "flatten-tree-raw", "cfg",
    "cfg-raw",          "autogen",         "document-symbols"};

ast::ParsedFile testSerialize(core::GlobalState &gs, ast::ParsedFile expr) {
    auto &savedFile = expr.file.data(gs);
    auto saved = core::serialize::Serializer::storeFile(savedFile, expr);
    auto restored = core::serialize::Serializer::loadFile(gs, expr.file, saved.data());
    return {move(restored.tree), expr.file};
}

/** Converts a Sorbet Error object into an equivalent LSP Diagnostic object. */
unique_ptr<Diagnostic> errorToDiagnostic(const core::GlobalState &gs, const core::Error &error) {
    if (!error.loc.exists()) {
        return nullptr;
    }
    return make_unique<Diagnostic>(Range::fromLoc(gs, error.loc), error.header);
}

class ExpectationHandler {
    Expectations &test;
    shared_ptr<core::ErrorQueue> &errorQueue;

public:
    vector<unique_ptr<core::Error>> errors;
    UnorderedMap<string_view, string> got;

    ExpectationHandler(Expectations &test, shared_ptr<core::ErrorQueue> &errorQueue)
        : test(test), errorQueue(errorQueue){};

    void addObserved(string_view expectationType, std::function<string()> mkExp, bool addNewline = true) {
        if (test.expectations.contains(expectationType)) {
            got[expectationType].append(mkExp());
            if (addNewline) {
                got[expectationType].append("\n");
            }
            auto newErrors = errorQueue->drainAllErrors();
            errors.insert(errors.end(), make_move_iterator(newErrors.begin()), make_move_iterator(newErrors.end()));
        }
    }

    void checkExpectations(string prefix = "") {
        for (auto &gotPhase : got) {
            auto expectation = test.expectations.find(gotPhase.first);
            REQUIRE_MESSAGE(expectation != test.expectations.end(),
                            prefix << "missing expectation for " << gotPhase.first);
            REQUIRE_MESSAGE(expectation->second.size() == 1,
                            prefix << "found unexpected multiple expectations of type " << gotPhase.first);

            auto checker = test.folder + expectation->second.begin()->second;
            auto expect = FileOps::read(checker);

            {
                INFO(prefix << "Mismatch on: " << checker);
                CHECK_EQ(expect, gotPhase.second);
            }
            if (expect == gotPhase.second) {
                MESSAGE(gotPhase.first << " OK");
            }
        }
    }

    void drainErrors() {
        auto newErrors = errorQueue->drainAllErrors();
        errors.insert(errors.end(), make_move_iterator(newErrors.begin()), make_move_iterator(newErrors.end()));
    }

    void clear() {
        got.clear();
        errorQueue->drainAllErrors();
    }
};

TEST_CASE("PerPhaseTest") { // NOLINT
    Expectations test = getInput(singleTest);

    auto inputPath = test.folder + test.basename;
    auto rbName = test.basename + ".rb";

    for (auto &exp : test.expectations) {
        if (!knownExpectations.contains(exp.first)) {
            FAIL_CHECK("Unknown pass: " << exp.first);
        }
    }

    auto logger = spd::stderr_color_mt("fixtures: " + inputPath);
    auto errorQueue = make_shared<core::ErrorQueue>(*logger, *logger);
    auto gs = make_unique<core::GlobalState>(errorQueue);

    for (auto provider : sorbet::pipeline::semantic_extension::SemanticExtensionProvider::getProviders()) {
        gs->semanticExtensions.emplace_back(provider->defaultInstance());
    }

    gs->censorForSnapshotTests = true;
    auto workers = WorkerPool::create(0, gs->tracer());

    auto assertions = RangeAssertion::parseAssertions(test.sourceFileContents);
    if (BooleanPropertyAssertion::getValue("no-stdlib", assertions).value_or(false)) {
        gs->initEmpty();
    } else {
        core::serialize::Serializer::loadGlobalState(*gs, getNameTablePayload);
    }
    // Parser
    vector<core::FileRef> files;
    constexpr string_view whitelistedTypedNoneTest = "missing_typed_sigil.rb"sv;
    {
        core::UnfreezeFileTable fileTableAccess(*gs);

        for (auto &sourceFile : test.sourceFiles) {
            auto fref = gs->enterFile(test.sourceFileContents[test.folder + sourceFile]);
            if (FileOps::getFileName(sourceFile) == whitelistedTypedNoneTest) {
                fref.data(*gs).strictLevel = core::StrictLevel::False;
            }
            files.emplace_back(fref);
        }
    }
    vector<ast::ParsedFile> trees;
    ExpectationHandler handler(test, errorQueue);

    for (auto file : files) {
        if (FileOps::getFileName(file.data(*gs).path()) != whitelistedTypedNoneTest &&
            file.data(*gs).source().find("# typed:") == string::npos) {
            ADD_FAIL_CHECK_AT(file.data(*gs).path().data(), 1, "Add a `# typed: strict` line to the top of this file");
        }
        unique_ptr<parser::Node> nodes;
        {
            core::UnfreezeNameTable nameTableAccess(*gs); // enters original strings

            nodes = parser::Parser::run(*gs, file);
        }

        handler.drainErrors();
        handler.addObserved("parse-tree", [&]() { return nodes->toString(*gs); });
        handler.addObserved("parse-tree-whitequark", [&]() { return nodes->toWhitequark(*gs); });
        handler.addObserved("parse-tree-json", [&]() { return nodes->toJSON(*gs); });

        // Desugarer
        ast::ParsedFile desugared;
        {
            core::UnfreezeNameTable nameTableAccess(*gs); // enters original strings

            core::MutableContext ctx(*gs, core::Symbols::root(), file);
            desugared = testSerialize(*gs, ast::ParsedFile{ast::desugar::node2Tree(ctx, move(nodes)), file});
        }

        handler.addObserved("desugar-tree", [&]() { return desugared.tree->toString(*gs); });
        handler.addObserved("desugar-tree-raw", [&]() { return desugared.tree->showRaw(*gs); });

        ast::ParsedFile rewriten;
        ast::ParsedFile localNamed;

        if (!test.expectations.contains("autogen")) {
            // Rewriter
            {
                core::UnfreezeNameTable nameTableAccess(*gs); // enters original strings

                core::MutableContext ctx(*gs, core::Symbols::root(), desugared.file);
                rewriten = testSerialize(
                    *gs, ast::ParsedFile{rewriter::Rewriter::run(ctx, move(desugared.tree)), desugared.file});
            }

            handler.addObserved("rewrite-tree", [&]() { return rewriten.tree->toString(*gs); });
            handler.addObserved("rewrite-tree-raw", [&]() { return rewriten.tree->showRaw(*gs); });

            core::MutableContext ctx(*gs, core::Symbols::root(), desugared.file);
            localNamed = testSerialize(*gs, local_vars::LocalVars::run(ctx, move(rewriten)));

            handler.addObserved("index-tree", [&]() { return localNamed.tree->toString(*gs); });
            handler.addObserved("index-tree-raw", [&]() { return localNamed.tree->showRaw(*gs); });
        } else {
            core::MutableContext ctx(*gs, core::Symbols::root(), desugared.file);
            localNamed = testSerialize(*gs, local_vars::LocalVars::run(ctx, move(desugared)));
            if (test.expectations.contains("rewrite-tree-raw") || test.expectations.contains("rewrite-tree")) {
                FAIL_CHECK("Running Rewriter passes with autogen isn't supported");
            }
        }

        // Namer
        ast::ParsedFile namedTree;
        {
            core::UnfreezeNameTable nameTableAccess(*gs);     // creates singletons and class names
            core::UnfreezeSymbolTable symbolTableAccess(*gs); // enters symbols
            vector<ast::ParsedFile> vTmp;
            vTmp.emplace_back(move(localNamed));
            vTmp = namer::Namer::run(*gs, move(vTmp));
            namedTree = testSerialize(*gs, move(vTmp[0]));
        }

        handler.addObserved("name-tree", [&]() { return namedTree.tree->toString(*gs); });
        handler.addObserved("name-tree-raw", [&]() { return namedTree.tree->showRaw(*gs); });

        trees.emplace_back(move(namedTree));
    }

    if (test.expectations.contains("autogen")) {
        {
            core::UnfreezeNameTable nameTableAccess(*gs);
            core::UnfreezeSymbolTable symbolAccess(*gs);

            trees = resolver::Resolver::runConstantResolution(*gs, move(trees), *workers);
        }
        handler.addObserved(
            "autogen",
            [&]() {
                stringstream payload;
                for (auto &tree : trees) {
                    core::Context ctx(*gs, core::Symbols::root(), tree.file);
                    auto pf = autogen::Autogen::generate(ctx, move(tree));
                    tree = move(pf.tree);
                    payload << pf.toString(ctx);
                }
                return payload.str();
            },
            false);
        // Autogen forces you to to put --stop-after=namer so lets not run
        // anything else
        return;
    } else {
        core::UnfreezeNameTable nameTableAccess(*gs);     // Resolver::defineAttr
        core::UnfreezeSymbolTable symbolTableAccess(*gs); // enters stubs
        trees = move(resolver::Resolver::run(*gs, move(trees), *workers).result());
        handler.drainErrors();
    }

    handler.addObserved("symbol-table", [&]() { return gs->toString(); });
    handler.addObserved("symbol-table-raw", [&]() { return gs->showRaw(); });

    for (auto &resolvedTree : trees) {
        handler.addObserved("resolve-tree", [&]() { return resolvedTree.tree->toString(*gs); });
        handler.addObserved("resolve-tree-raw", [&]() { return resolvedTree.tree->showRaw(*gs); });
    }

    // Simulate what pipeline.cc does: We want to start typeckecking big files first because it helps with better work
    // distribution
    fast_sort(trees, [&](const auto &lhs, const auto &rhs) -> bool {
        return lhs.file.data(*gs).source().size() > rhs.file.data(*gs).source().size();
    });

    for (auto &resolvedTree : trees) {
        auto file = resolvedTree.file;

        core::Context ctx(*gs, core::Symbols::root(), file);
        resolvedTree = definition_validator::runOne(ctx, move(resolvedTree));
        handler.drainErrors();

        resolvedTree = class_flatten::runOne(ctx, move(resolvedTree));

        handler.addObserved("flatten-tree", [&]() { return resolvedTree.tree->toString(*gs); });
        handler.addObserved("flatten-tree-raw", [&]() { return resolvedTree.tree->showRaw(*gs); });

        auto checkTree = [&]() {
            if (resolvedTree.tree == nullptr) {
                auto path = file.data(*gs).path();
                ADD_FAIL_CHECK_AT(path.begin(), 1, "Already used tree. You can only have 1 CFG-ish .exp file");
            }
        };
        auto checkPragma = [&](string ext) {
            if (file.data(*gs).strictLevel < core::StrictLevel::True) {
                auto path = file.data(*gs).path();
                ADD_FAIL_CHECK_AT(path.begin(), 1,
                                  "Missing `# typed:` pragma. Sources with ." << ext
                                                                              << ".exp files must specify # typed:");
            }
        };

        // CFG
        if (test.expectations.contains("cfg") || test.expectations.contains("cfg-raw")) {
            checkTree();
            checkPragma("cfg");
            CFGCollectorAndTyper collector;
            core::Context ctx(*gs, core::Symbols::root(), resolvedTree.file);
            auto cfg = ast::TreeMap::apply(ctx, collector, move(resolvedTree.tree));
            for (auto &extension : ctx.state.semanticExtensions) {
                extension->finishTypecheckFile(ctx, file);
            }
            resolvedTree.tree.reset();

            handler.addObserved("cfg", [&]() {
                stringstream dot;
                dot << "digraph \"" << rbName << "\" {" << '\n';
                for (auto &cfg : collector.cfgs) {
                    dot << cfg->toString(ctx) << '\n' << '\n';
                }
                dot << "}" << '\n';
                return dot.str();
            });

            handler.addObserved("cfg-raw", [&]() {
                stringstream dot;
                dot << "digraph \"" << rbName << "\" {" << '\n';
                dot << "  graph [fontname = \"Courier\"];\n";
                dot << "  node [fontname = \"Courier\"];\n";
                dot << "  edge [fontname = \"Courier\"];\n";
                for (auto &cfg : collector.cfgs) {
                    dot << cfg->showRaw(ctx) << '\n' << '\n';
                }
                dot << "}" << '\n';
                return dot.str();
            });
        }

        // If there is a tree left with a typed: pragma, run the inferencer
        if (resolvedTree.tree != nullptr && file.data(*gs).originalSigil >= core::StrictLevel::True) {
            checkTree();
            CFGCollectorAndTyper collector;
            core::Context ctx(*gs, core::Symbols::root(), resolvedTree.file);
            ast::TreeMap::apply(ctx, collector, move(resolvedTree.tree));
            for (auto &extension : ctx.state.semanticExtensions) {
                extension->finishTypecheckFile(ctx, file);
            }
            resolvedTree.tree.reset();
            handler.drainErrors();
        }
    }

    for (auto &extension : gs->semanticExtensions) {
        extension->finishTypecheck(*gs);
    }

    handler.checkExpectations();

    if (test.expectations.contains("symbol-table")) {
        string table = gs->toString() + '\n';
        INFO("symbol-table should not be mutated by CFG+inference");
        CHECK_EQ(handler.got["symbol-table"], table);
    }

    if (test.expectations.contains("symbol-table-raw")) {
        string table = gs->showRaw() + '\n';
        INFO("symbol-table-raw should not be mutated by CFG+inference");
        CHECK_EQ(handler.got["symbol-table-raw"], table);
    }

    // Check warnings and errors
    {
        map<string, vector<unique_ptr<Diagnostic>>> diagnostics;
        for (auto &error : handler.errors) {
            if (error->isSilenced) {
                continue;
            }
            auto diag = errorToDiagnostic(*gs, *error);
            if (diag == nullptr) {
                continue;
            }
            auto path = error->loc.file().data(*gs).path();
            diagnostics[string(path.begin(), path.end())].push_back(std::move(diag));
        }
        ErrorAssertion::checkAll(test.sourceFileContents, RangeAssertion::getErrorAssertions(assertions), diagnostics);
    }

    // Allow later phases to have errors that we didn't test for
    errorQueue->drainAllErrors();

    // now we test the incremental resolver

    auto disableStressIncremental =
        BooleanPropertyAssertion::getValue("disable-stress-incremental", assertions).value_or(false);
    if (disableStressIncremental) {
        MESSAGE("errors OK");
        return;
    }

    handler.clear();
    auto symbolsBefore = gs->symbolsUsed();

    vector<ast::ParsedFile> newTrees;
    for (auto &f : trees) {
        const int prohibitedLines = f.file.data(*gs).source().size();
        auto newSource = absl::StrCat(string(prohibitedLines + 1, '\n'), f.file.data(*gs).source());
        auto newFile =
            make_shared<core::File>((string)f.file.data(*gs).path(), move(newSource), f.file.data(*gs).sourceType);
        gs = core::GlobalState::replaceFile(move(gs), f.file, move(newFile));

        // this replicates the logic of pipeline::indexOne
        auto nodes = parser::Parser::run(*gs, f.file);
        handler.addObserved("parse-tree", [&]() { return nodes->toString(*gs); });
        handler.addObserved("parse-tree-json", [&]() { return nodes->toJSON(*gs); });

        core::MutableContext ctx(*gs, core::Symbols::root(), f.file);
        ast::ParsedFile file = testSerialize(*gs, ast::ParsedFile{ast::desugar::node2Tree(ctx, move(nodes)), f.file});
        handler.addObserved("desguar-tree", [&]() { return file.tree->toString(*gs); });
        handler.addObserved("desugar-tree-raw", [&]() { return file.tree->showRaw(*gs); });

        // Rewriter pass
        file = testSerialize(*gs, ast::ParsedFile{rewriter::Rewriter::run(ctx, move(file.tree)), file.file});
        handler.addObserved("rewrite-tree", [&]() { return file.tree->toString(*gs); });
        handler.addObserved("rewrite-tree-raw", [&]() { return file.tree->showRaw(*gs); });

        // local vars
        file = testSerialize(*gs, local_vars::LocalVars::run(ctx, move(file)));
        handler.addObserved("index-tree", [&]() { return file.tree->toString(*gs); });
        handler.addObserved("index-tree-raw", [&]() { return file.tree->showRaw(*gs); });

        // namer
        {
            core::UnfreezeSymbolTable symbolTableAccess(*gs);
            vector<ast::ParsedFile> vTmp;
            vTmp.emplace_back(move(file));
            vTmp = namer::Namer::run(*gs, move(vTmp));
            file = testSerialize(*gs, move(vTmp[0]));
        }

        handler.addObserved("name-tree", [&]() { return file.tree->toString(*gs); });
        handler.addObserved("name-tree-raw", [&]() { return file.tree->showRaw(*gs); });
        newTrees.emplace_back(move(file));
    }

    // resolver
    trees = resolver::Resolver::runTreePasses(*gs, move(newTrees));

    for (auto &resolvedTree : trees) {
        handler.addObserved("resolve-tree", [&]() { return resolvedTree.tree->toString(*gs); });
        handler.addObserved("resolve-tree-raw", [&]() { return resolvedTree.tree->showRaw(*gs); });
    }

    handler.checkExpectations("[stress-incremental] ");

    // and drain all the remaining errors
    errorQueue->drainAllErrors();

    {
        INFO("the incremental resolver should not add new symbols");
        CHECK_EQ(symbolsBefore, gs->symbolsUsed());
    }
} // namespace sorbet::test

// namespace sorbet::test

bool compareNames(string_view left, string_view right) {
    auto lsplit = left.find("__");
    if (lsplit == string::npos) {
        lsplit = left.find(".");
    }
    auto rsplit = right.find("__");
    if (rsplit == string::npos) {
        rsplit = right.find(".");
    }
    string_view lbase(left.data(), lsplit == string::npos ? left.size() : lsplit);
    string_view rbase(right.data(), rsplit == string::npos ? right.size() : rsplit);
    if (lbase != rbase) {
        return left < right;
    }

    // If the base names match, make files with the ".rb" extension come before all others.
    // The remaining files will be sorted by reverse order on extension.
    auto lext = FileOps::getExtension(left);
    auto rext = FileOps::getExtension(right);
    if (lext != rext) {
        if (lext == "rb") {
            return true;
        } else if (rext == "rb") {
            return false;
        } else {
            return rext < lext;
        }
    }

    // Sort multi-part tests
    return left < right;
}

string rbFile2BaseTestName(string rbFileName) {
    auto basename = rbFileName;
    auto lastDirSeparator = basename.find_last_of("/");
    if (lastDirSeparator != string::npos) {
        basename = basename.substr(lastDirSeparator + 1);
    }
    auto split = basename.rfind(".");
    if (split != string::npos) {
        basename = basename.substr(0, split);
    }
    split = basename.find("__");
    if (split != string::npos) {
        basename = basename.substr(0, split);
    }
    string testName = basename;
    if (lastDirSeparator != string::npos) {
        testName = rbFileName.substr(0, lastDirSeparator + 1 + testName.length());
    }
    return testName;
}

vector<Expectations> listDir(const char *name) {
    vector<Expectations> result;

    vector<string> names = sorbet::FileOps::listFilesInDir(name, {".rb", ".rbi", ".rbupdate", ".exp"}, false, {}, {});
    const int prefixLen = strnlen(name, 1024) + 1;
    // Trim off the input directory from the name.
    transform(names.begin(), names.end(), names.begin(),
              [&prefixLen](auto &name) -> string { return name.substr(prefixLen); });
    fast_sort(names, compareNames);

    Expectations current;
    for (auto &s : names) {
        if (absl::EndsWith(s, ".rb") || absl::EndsWith(s, ".rbi")) {
            auto basename = rbFile2BaseTestName(s);
            if (basename != s) {
                if (basename == current.basename) {
                    current.sourceFiles.emplace_back(s);
                    continue;
                }
            }

            if (!current.basename.empty()) {
                result.emplace_back(current);
                current = Expectations();
            }
            current.basename = basename;
            current.sourceFiles.emplace_back(s);
            current.folder = name;
            current.folder += "/";
            current.testName = current.folder + current.basename;
        } else if (absl::EndsWith(s, ".exp")) {
            if (absl::StartsWith(s, current.basename)) {
                auto kind_start = s.rfind(".", s.size() - strlen(".exp") - 1);
                string kind = s.substr(kind_start + 1, s.size() - kind_start - strlen(".exp") - 1);
                string source_file_path = string(name) + "/" + s.substr(0, kind_start);
                current.expectations[kind][source_file_path] = s;
            }
        } else if (absl::EndsWith(s, ".rbupdate")) {
            if (absl::StartsWith(s, current.basename)) {
                // Should be `.[number].rbupdate`
                auto pos = s.rfind('.', s.length() - 10);
                if (pos != string::npos) {
                    int version = stoi(s.substr(pos + 1, s.length() - 9));
                    current.sourceLSPFileUpdates[version].emplace_back(absl::StrCat(s.substr(0, pos), ".rb"), s);
                } else {
                    cout << "Ignoring " << s << ": No version number provided (expected .[number].rbupdate).\n";
                }
            }
        }
    }
    if (!current.basename.empty()) {
        result.emplace_back(current);
        current = Expectations();
    }

    return result;
}

Expectations getInput(string singleTest) {
    vector<Expectations> result;
    if (singleTest.empty()) {
        Exception::raise("No test specified. Pass one with --single_test=<test_path>");
    }

    string parentDir;
    {
        auto lastDirSeparator = singleTest.find_last_of("/");
        if (lastDirSeparator == string::npos) {
            parentDir = ".";
        } else {
            parentDir = singleTest.substr(0, lastDirSeparator);
        }
    }
    auto scan = listDir(parentDir.c_str());
    auto lookingFor = rbFile2BaseTestName(singleTest);
    for (Expectations &f : scan) {
        if (f.testName == lookingFor) {
            for (auto &file : f.sourceFiles) {
                string filename = f.folder + file;
                string fileContents = FileOps::read(filename);
                f.sourceFileContents[filename] =
                    make_shared<core::File>(move(filename), move(fileContents), core::File::Type::Normal);
            }
            result.emplace_back(f);
        }
    }

    if (result.size() != 1) {
        Exception::raise("Expected exactly one test, found {}", result.size());
    }

    return result.front();
}
} // namespace sorbet::test

int main(int argc, char *argv[]) {
    cxxopts::Options options("test_corpus", "Test corpus for Sorbet typechecker");
    options.allow_unrecognised_options().add_options()("single_test", "run over single test.",
                                                       cxxopts::value<std::string>()->default_value(""), "testpath");
    auto res = options.parse(argc, argv);

    if (res.count("single_test") != 1) {
        printf("--single_test=<filename> argument expected\n");
        return 1;
    }

    sorbet::test::singleTest = res["single_test"].as<std::string>();

    doctest::Context context;
    return context.run();
}