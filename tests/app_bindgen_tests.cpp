#include "realscript/bindgen/BindGen.h"
#include "realscript/bindgen/HostApi.h"
#include "realscript/bindgen/RsBindParser.h"
#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/runtime/Runtime.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using namespace realscript;

[[noreturn]] void failCheck(const char* expr, int line) noexcept {
    std::cerr << "Check failed at app_bindgen_tests.cpp:" << line << ": " << expr << std::endl;
    std::exit(1);
}

#define RS_CHECK(...)                                                                              \
    do {                                                                                           \
        if (!(__VA_ARGS__)) {                                                                      \
            failCheck(#__VA_ARGS__, __LINE__);                                                     \
        }                                                                                          \
    } while (false)

struct NativeDamageSpec {
    std::int32_t baseDamage = 0;
    std::int32_t bonus = 0;
};

struct NativeWeaponBase {
    std::int32_t tier = 2;
    virtual ~NativeWeaponBase() = default;
    std::int32_t getTier() const { return tier; }
};

struct NativeWeapon final : public NativeWeaponBase {
    std::int32_t damage = 15;
    std::int32_t durability = 100;

    explicit NativeWeapon(std::int32_t initial = 15) : damage(initial) {}

    std::int32_t strike(std::int32_t mult) const {
        return damage * mult;
    }

    static std::int32_t bonusFactor() {
        return 3;
    }

    std::int32_t getDamage() const { return damage; }
    void setDamage(std::int32_t v) { damage = v; }
};

std::shared_ptr<const runtime::ProgramImage> compileAndLinkWithHostApi(
    const bindgen::HostApi& api,
    const std::vector<compiler::SourceFile>& userSources) {
    RS_CHECK(api.valid());
    compiler::Compilation compilation;
    for (auto source : api.generatedSources()) {
        compilation.addSource(std::move(source));
    }
    for (const auto& source : userSources) {
        compilation.addSource(source);
    }
    const auto build = compilation.build();
    if (build.diagnostics.hasErrors()) {
        for (const auto& d : build.diagnostics.items()) {
            std::cerr << "BindGen compile error [" << d.code << "]: " << d.message << "\n";
        }
    }
    RS_CHECK(!build.diagnostics.hasErrors());

    const auto nativeModules = api.nativeModules();
    const auto& nativeBindings = api.nativeBindingNames();
    bytecode::Lowerer lowerer;
    std::vector<bytecode::Module> modules;
    modules.reserve(build.modules.size());
    for (const auto& mirMod : build.modules) {
        auto mod = lowerer.lower(mirMod);
        if (nativeModules.find(mod.name) != nativeModules.end()) {
            mod.functions.erase(
                std::remove_if(
                    mod.functions.begin(),
                    mod.functions.end(),
                    [&](const bytecode::Function& fn) {
                        return nativeBindings.find(
                            mod.name + "::" + fn.name) != nativeBindings.end();
                    }),
                mod.functions.end());
        }
        modules.push_back(std::move(mod));
    }

    runtime::RuntimeError linkError;
    auto linked = runtime::ProgramImage::link(modules, linkError);
    if (!linked) {
        std::cerr << "BindGen link error: " << linkError.message << "\n";
    }
    RS_CHECK(linked.has_value());
    return std::make_shared<runtime::ProgramImage>(std::move(*linked));
}

void testHostApiTypedBindingsAndStubs() {
    bindgen::HostApi api;
    api.structType<NativeDamageSpec>("Host.Native", "DamageSpec")
        .field<&NativeDamageSpec::baseDamage>("baseDamage")
        .field<&NativeDamageSpec::bonus>("bonus");

    api.type<NativeWeaponBase>("Host.Native", "WeaponBase")
        .property<&NativeWeaponBase::getTier>("tier");

    api.type<NativeWeapon>("Host.Native", "Weapon")
        .base<NativeWeaponBase>()
        .constructor<std::int32_t>("Create")
        .method<&NativeWeapon::strike>("strike")
        .staticMethod<&NativeWeapon::bonusFactor>("bonusFactor")
        .property<&NativeWeapon::getDamage, &NativeWeapon::setDamage>("damage")
        .field<&NativeWeapon::durability>("durability");

    api.function("Host.Native", "scaleDamage", [](NativeDamageSpec spec, std::int32_t factor) {
        return (spec.baseDamage + spec.bonus) * factor;
    });
    api.function("Host.Native", "combineValues", static_cast<std::int32_t (*)(std::int32_t)>([](std::int32_t a) {
        return a + 10;
    }));
    api.function("Host.Native", "combineValues", static_cast<std::int32_t (*)(std::int32_t, std::int32_t)>([](std::int32_t a, std::int32_t b) {
        return a + b;
    }));
    api.function("Host.Native", "sumVector", [](const std::vector<std::int32_t>& items) -> std::int32_t {
        std::int32_t total = 0;
        for (const auto v : items) total += v;
        return total;
    });
    api.function("Host.Native", "stepVec2", [](bindgen::Vec2 v, std::int32_t dx) {
        return bindgen::Vec2{v.x + bindgen::Fixed::fromInt(dx), v.y};
    });

    RS_CHECK(api.valid());
    const auto stubs = api.exportDeclarationStubs();
    RS_CHECK(stubs.size() == 1);
    RS_CHECK(stubs[0].path == "Host_Native.d.rscript");
    RS_CHECK(stubs[0].content.find("struct DamageSpec") != std::string::npos);
    RS_CHECK(stubs[0].content.find("class Weapon : WeaponBase") != std::string::npos);

    const std::string projectJson = api.exportProjectConfigJson("BindGenDemo", {"app.rscript"});
    RS_CHECK(projectJson.find("\"projectName\": \"BindGenDemo\"") != std::string::npos);
    RS_CHECK(projectJson.find("\"Host.Native\"") != std::string::npos);
    RS_CHECK(projectJson.find("\"app.rscript\"") != std::string::npos);

    const auto image = compileAndLinkWithHostApi(api, {{
        "app.rscript",
        R"(module Host.App;
import Host.Native;
import RealScript.Math;

public class BindRunner
{
    public static int run(int baseVal)
    {
        var spec = new DamageSpec();
        spec.baseDamage = baseVal;
        spec.bonus = 5;
        int scaled = scaleDamage(spec, 2);

        var blade = Weapon.Create(20);
        blade.damage = blade.damage + scaled;
        blade.durability = blade.durability - 5;

        int[] nums = new int[3];
        nums[0] = 1;
        nums[1] = 2;
        nums[2] = 3;

        var shifted = stepVec2(new Vec2(3, 4), 2);
        return blade.strike(2) + Weapon.bonusFactor() + blade.tier + blade.durability
            + combineValues(5) + combineValues(1, 2) + sumVector(nums) + (shifted.x.raw / 256);
    }
}
)",
    }});

    runtime::Interpreter interp(image, api.heap());
    interp.setBindingRegistry(api.bindings());
    const auto res = interp.invoke("Host.App::BindRunner.run", {runtime::Value{std::int64_t{10}}});
    RS_CHECK(res.succeeded);
    // strike: (20 + (10+5)*2) * 2 = 100
    // bonusFactor: 3, tier: 2, durability: 95
    // combineValues(5): 15, combineValues(1,2): 3, sumVector: 6, shifted.x: 5
    // total = 100 + 3 + 2 + 95 + 15 + 3 + 6 + 5 = 229
    RS_CHECK(std::get<std::int64_t>(res.value) == 229);

    std::cout << "[PASS] HostApi typed bindings, overloads, vectors, math & .d.rscript stubs\n";
}

void testRsBindParserPreprocessorTemplateEcsAndParity() {
    const std::string header = R"(
#pragma once

RS_EXPORT_ENUM("Host.SdkBind")
enum class CombatMode {
    Melee = 0,
    Ranged = 1,
};

RS_EXPORT_ECS_COMPONENT("Host.SdkBind")
struct TransformComp {
    RS_EXPORT_FIELD int entityId;
    RS_EXPORT_FIELD double weight;
};

#ifdef ENABLE_EXPERIMENTAL_ABILITY
RS_EXPORT_CLASS("Host.SdkBind")
class ExperimentalAbility {
public:
    RS_EXPORT_CONSTRUCTOR() ExperimentalAbility();
    RS_EXPORT_METHOD int power() const;
};
#else
RS_EXPORT_CLASS("Host.SdkBind")
class StandardAbility {
public:
    RS_EXPORT_CONSTRUCTOR() StandardAbility();
    RS_EXPORT_METHOD int power() const;
};
#endif

#if 0
RS_EXPORT_FUNCTION("Host.SdkBind")
int deadBranchHelper(int x);
#endif

RS_EXPORT_FUNCTION("Host.SdkBind")
int liveBranchHelper(int x);
)";

    const auto offMacro = bindgen::HostApi::generateBindingsFromCppHeader(header, "Host.SdkBind", {});
    const auto offParity = bindgen::rsbind_detail::generateFromParsedHeader(header, "Host.SdkBind", {});
    RS_CHECK(offMacro.scriptDeclarationStub == offParity.scriptDeclarationStub);
    RS_CHECK(offMacro.cppRegistrationSource == offParity.cppRegistrationSource);
    RS_CHECK(offMacro.enumsExported == 1);
    RS_CHECK(offMacro.structsExported == 1);
    RS_CHECK(offMacro.classesExported == 1);
    RS_CHECK(offMacro.functionsExported == 1);
    RS_CHECK(offMacro.scriptDeclarationStub.find("StandardAbility") != std::string::npos);
    RS_CHECK(offMacro.scriptDeclarationStub.find("ExperimentalAbility") == std::string::npos);
    RS_CHECK(offMacro.scriptDeclarationStub.find("deadBranchHelper") == std::string::npos);
    RS_CHECK(offMacro.scriptDeclarationStub.find("liveBranchHelper") != std::string::npos);
    RS_CHECK(offMacro.cppRegistrationSource.find("#include \"realscript/bindgen/BindGen.h\"") != std::string::npos);
    RS_CHECK(offMacro.cppRegistrationSource.find("registerGeneratedHeaderBindings(realscript::bindgen::HostApi& api)") != std::string::npos);
    RS_CHECK(offMacro.cppRegistrationSource.find("template <typename Runtime>") != std::string::npos);
    RS_CHECK(offMacro.cppRegistrationSource.find("bindEcsColumn_TransformComp") != std::string::npos);
    RS_CHECK(offMacro.cppRegistrationSource.find("realscript::game") == std::string::npos);
    RS_CHECK(offMacro.cppRegistrationSource.find("GameScripting.h") == std::string::npos);

    const std::unordered_map<std::string, std::string> macros{{"ENABLE_EXPERIMENTAL_ABILITY", "1"}};
    const auto onMacro = bindgen::HostApi::generateBindingsFromCppHeader(header, "Host.SdkBind", macros);
    RS_CHECK(onMacro.classesExported == 1);
    RS_CHECK(onMacro.scriptDeclarationStub.find("ExperimentalAbility") != std::string::npos);
    RS_CHECK(onMacro.scriptDeclarationStub.find("StandardAbility") == std::string::npos);

    std::cout << "[PASS] RsBindParser L1 BindGen preprocessor, templated ECS helpers & parity\n";
}

} // namespace

int main() {
    testHostApiTypedBindingsAndStubs();
    testRsBindParserPreprocessorTemplateEcsAndParity();
    return 0;
}
