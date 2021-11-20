#include "main/autogen/dsl_analysis.h"
#include "ast/Helpers.h"
#include "ast/ast.h"
#include "ast/treemap/treemap.h"
#include "common/formatting.h"
#include "main/autogen/crc_builder.h"

using namespace std;
namespace sorbet::autogen {

const std::vector<u4> KNOWN_PROP_METHODS = {
    core::Names::prop().rawId(),         core::Names::tokenProp().rawId(),
    core::Names::tokenProp().rawId(),    core::Names::timestampedTokenProp().rawId(),
    core::Names::createdProp().rawId(),  core::Names::updatedProp().rawId(),
    core::Names::merchantProp().rawId(), core::Names::merchantTokenProp().rawId(),
};

struct PropInfoInternal {
    core::NameRef name;
    ast::ExpressionPtr typeExp;
};

class DSLAnalysisWalk {
    UnorderedMap<vector<core::NameRef>, DSLInfo> dslInfo;
    vector<vector<core::NameRef>> nestingScopes;
    core::FileRef file;
    bool validScope;

    // Convert a symbol name into a fully qualified name
    vector<core::NameRef> symbolName(core::Context ctx, core::SymbolRef sym) {
        vector<core::NameRef> out;
        while (sym.exists() && sym != core::Symbols::root()) {
            out.emplace_back(sym.data(ctx)->name);
            sym = sym.data(ctx)->owner;
        }
        reverse(out.begin(), out.end());
        return out;
    }

    std::optional<PropInfoInternal> parseProp(core::Context ctx, ast::Send *send) {
        switch (send->fun.rawId()) {
            case core::Names::tokenProp().rawId():
            case core::Names::timestampedTokenProp().rawId():
                return PropInfoInternal{
                    core::Names::token(),
                    ast::MK::Constant(send->loc, core::Symbols::String()),
                };
            case core::Names::createdProp().rawId():
                return PropInfoInternal{core::Names::created(), ast::MK::Constant(send->loc, core::Symbols::Float())};
            case core::Names::updatedProp().rawId():
                return PropInfoInternal{core::Names::updated(), ast::MK::Constant(send->loc, core::Symbols::Float())};
            case core::Names::merchantProp().rawId():
                return PropInfoInternal{
                    core::Names::merchant(),
                    ast::MK::Constant(send->loc, core::Symbols::String()),
                };
            case core::Names::merchantTokenProp().rawId():
                return PropInfoInternal{
                    core::Names::merchant(),
                    ast::MK::UnresolvedConstant(
                        send->loc,
                        ast::MK::UnresolvedConstant(
                            send->loc,
                            ast::MK::UnresolvedConstant(send->loc,
                                                        ast::MK::UnresolvedConstant(send->loc, ast::MK::EmptyTree(),
                                                                                    core::Names::Constants::Opus()),
                                                        core::Names::Constants::Autogen()),
                            core::Names::Constants::Tokens()),
                        core::Names::Constants::AccountModelMerchantToken()),
                };
            default:
                return std::nullopt;
        }

        return std::nullopt;
    }

public:
    DSLAnalysisWalk(core::FileRef a_file) {
        validScope = true;
        file = a_file;
    }

    ast::ExpressionPtr preTransformClassDef(core::Context ctx, ast::ExpressionPtr tree) {
        auto &original = ast::cast_tree_nonnull<ast::ClassDef>(tree);
        if (original.symbol.data(ctx)->owner == core::Symbols::PackageRegistry()) {
            // this is a package, so do not enter a definition for it
            return tree;
        }

        vector<vector<core::NameRef>> ancestors;
        for (auto &ancst : original.ancestors) {
            auto *cnst = ast::cast_tree<ast::ConstantLit>(ancst);
            if (cnst == nullptr || cnst->original == nullptr) {
                // ignore them if they're not statically-known ancestors (i.e. not constants)
                continue;
            }

            ancestors.emplace_back(symbolName(ctx, cnst->symbol));
        }

        const vector<core::NameRef> className = symbolName(ctx, original.symbol);
        nestingScopes.emplace_back(className);
        dslInfo.emplace(className, DSLInfo{{}, ancestors, file, {}, {}});

        return tree;
    }

    ast::ExpressionPtr postTransformClassDef(core::Context ctx, ast::ExpressionPtr tree) {
        if (nestingScopes.size() == 0 || !validScope) {
            // Not in any scope
            return tree;
        }

        nestingScopes.pop_back();

        return tree;
    }

    ast::ExpressionPtr preTransformSend(core::Context ctx, ast::ExpressionPtr tree) {
        if (nestingScopes.size() == 0) {
            // Not in any scope
            return tree;
        }

        auto *original = ast::cast_tree<ast::Send>(tree);
        auto &curScope = nestingScopes.back();

        u4 funId = original->fun.rawId();
        bool isProp = absl::c_any_of(KNOWN_PROP_METHODS, [&](const auto &nrid) -> bool { return nrid == funId; });
        if (isProp) {
            if (!validScope) {
                dslInfo[curScope].problemLocs.emplace_back(original->loc);
                return tree;
            }

            if (funId == core::Names::prop().rawId()) {
                auto *lit = ast::cast_tree<ast::Literal>(original->args.front());
                if (lit && lit->isSymbol(ctx)) {
                    if (original->args.size() > 1) {
                        dslInfo[curScope].props.emplace_back(
                            PropInfo{lit->asSymbol(ctx), original->args[1].toString(ctx)});
                    } else {
                        dslInfo[curScope].props.emplace_back(PropInfo{lit->asSymbol(ctx), std::nullopt});
                    }
                } else {
                    dslInfo[curScope].problemLocs.emplace_back(original->loc);
                }

                return tree;
            }

            const auto propInfo = parseProp(ctx, original);
            if (propInfo.has_value()) {
                dslInfo[curScope].props.emplace_back(
                    PropInfo{std::move((*propInfo).name), std::move((*propInfo).typeExp).toString(ctx)});
            } else {
                dslInfo[curScope].problemLocs.emplace_back(original->loc);
            }

            return tree;
        }

        if (original->fun == core::Names::modelDsl()) {
            auto *cnst = ast::cast_tree<ast::ConstantLit>(original->args.front());
            if (!validScope || cnst == nullptr || cnst->original == nullptr) {
                return tree;
            }

            dslInfo[curScope].model = symbolName(ctx, cnst->symbol);
        }

        return tree;
    }

    ast::ExpressionPtr preTransformMethodDef(core::Context ctx, ast::ExpressionPtr tree) {
        if (nestingScopes.size() == 0 || !validScope) {
            // Not already in a valid scope
            return tree;
        }

        validScope = false;
        return tree;
    }

    ast::ExpressionPtr postTransformMethodDef(core::Context ctx, ast::ExpressionPtr tree) {
        if (nestingScopes.size() == 0 || validScope) {
            // Already in a valid scope, or never in a scope
            return tree;
        }

        validScope = true;
        return tree;
    }

    DSLAnalysisFile dslAnalysisFile() {
        DSLAnalysisFile out;
        out.dslInfo = std::move(dslInfo);
        out.file = std::move(file);
        return out;
    }
};

DSLAnalysisFile DSLAnalysis::generate(core::Context ctx, ast::ParsedFile tree, const CRCBuilder &crcBuilder) {
    DSLAnalysisWalk walk(tree.file);
    ast::TreeMap::apply(ctx, walk, move(tree.tree));
    auto daf = walk.dslAnalysisFile();
    auto src = tree.file.data(ctx).source();
    daf.cksum = crcBuilder.crc32(src);
    return daf;
}

} // namespace sorbet::autogen