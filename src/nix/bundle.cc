#include "command.hh"
#include "common-args.hh"
#include "shared.hh"
#include "store-api.hh"
#include "local-fs-store.hh"
#include "fs-accessor.hh"

using namespace nix;

struct CmdBundle : InstallableCommand
{
    std::string bundler = "github:matthewbauer/nix-bundle";
    std::optional<Path> outLink;

    CmdBundle()
    {
        addFlag({
            .longName = "bundler",
            .description = fmt("Use a custom bundler instead of the default (`%s`).", bundler),
            .labels = {"flake-url"},
            .handler = {&bundler},
            .completer = {[&](size_t, std::string_view prefix) {
                completeFlakeRef(getStore(), prefix);
            }}
        });

        addFlag({
            .longName = "out-link",
            .shortName = 'o',
            .description = "Override the name of the symlink to the build result. It defaults to the base name of the app.",
            .labels = {"path"},
            .handler = {&outLink},
            .completer = completePath
        });

    }

    std::string description() override
    {
        return "bundle an application so that it works outside of the Nix store";
    }

    std::string doc() override
    {
        return
          #include "bundle.md"
          ;
    }

    Category category() override { return catSecondary; }

    Strings getDefaultFlakeAttrPaths() override
    {
        Strings res{"defaultApp." + settings.thisSystem.get()};
        for (auto & s : SourceExprCommand::getDefaultFlakeAttrPaths())
            res.push_back(s);
        return res;
    }

    Strings getDefaultFlakeAttrPathPrefixes() override
    {
        Strings res{"apps." + settings.thisSystem.get() + "."};
        for (auto & s : SourceExprCommand::getDefaultFlakeAttrPathPrefixes())
            res.push_back(s);
        return res;
    }

    void run(ref<Store> store) override
    {
        auto evalState = getEvalState();

        auto app = installable->toApp(*evalState).resolve(getEvalStore(), store);

        auto [bundlerFlakeRef, bundlerName] = parseFlakeRefWithFragment(bundler, absPath("."));
        const flake::LockFlags lockFlags{ .writeLockFile = false };
        auto bundler = InstallableFlake(this,
            evalState, std::move(bundlerFlakeRef),
            Strings{bundlerName == "" ? "defaultBundler" : bundlerName},
            Strings({"bundlers."}), lockFlags);

        auto attrs = evalState->buildBindings(2);

        PathSet context;
        for (auto & i : app.context)
            context.insert("=" + store->printStorePath(i.path));
        attrs.alloc("program").mkString(app.program, context);

        attrs.alloc("system").mkString(settings.thisSystem.get());

        auto vRes = evalState->allocValue();
        evalState->callFunction(
            *bundler.toValue(*evalState).first,
            evalState->allocValue()->mkAttrs(attrs),
            *vRes, noPos);

        if (!evalState->isDerivation(*vRes))
            throw Error("the bundler '%s' does not produce a derivation", bundler.what());

        auto attr1 = vRes->attrs->get(evalState->sDrvPath);
        if (!attr1)
            throw Error("the bundler '%s' does not produce a derivation", bundler.what());

        PathSet context2;
        StorePath drvPath = store->parseStorePath(evalState->coerceToPath(*attr1->pos, *attr1->value, context2));

        auto attr2 = vRes->attrs->get(evalState->sOutPath);
        if (!attr2)
            throw Error("the bundler '%s' does not produce a derivation", bundler.what());

        StorePath outPath = store->parseStorePath(evalState->coerceToPath(*attr2->pos, *attr2->value, context2));

        store->buildPaths({ DerivedPath::Built { drvPath } });

        auto outPathS = store->printStorePath(outPath);

        if (!outLink)
            outLink = baseNameOf(app.program);

        store.dynamic_pointer_cast<LocalFSStore>()->addPermRoot(outPath, absPath(*outLink));
    }
};

static auto r2 = registerCommand<CmdBundle>("bundle");
