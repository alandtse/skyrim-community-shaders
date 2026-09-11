import unittest

from test_scene_settings_policy import ROOT
import test_scene_settings_runtime as runtime


class GpuPassTests(unittest.TestCase):
    def test_dynamic_names_and_prefix_registration(self):
        header = (ROOT / "src/GpuPass.h").read_text(encoding="utf-8")
        implementation = (ROOT / "src/GpuPass.cpp").read_text(encoding="utf-8")
        source = r'''
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include "MACROS_HEADER"
namespace GpuPassCapabilities {
bool Register(std::string_view);
bool Contains(std::string_view);
}
REGISTRY
void require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
struct TemporaryName {
    static inline bool alive = false;
    TemporaryName() { alive = true; }
    ~TemporaryName() { alive = false; }
    operator std::string_view() const { return "Temporary::Pass"; }
};
struct ScopedGpuPass {
    static inline std::string lastName;
    explicit ScopedGpuPass(std::string_view name) {
        if (name == "Temporary::Pass")
            require(TemporaryName::alive, "Temporary pass names survive through scope construction");
        lastName = name;
    }
};
DYNAMIC_MACRO
void dynamicPass(std::string_view prefix, int suffix) {
    CS_GPU_PASS_DYNAMIC(std::string(prefix) + "::" + std::to_string(suffix));
    require(ScopedGpuPass::lastName == std::string(prefix) + "::" + std::to_string(suffix),
            "The current invocation's dynamic name reaches the profiler");
}
int main() {
    {
        CS_GPU_PASS_DYNAMIC(TemporaryName{});
        require(TemporaryName::alive, "A temporary name lives through the pass scope");
    }
    require(!TemporaryName::alive, "Temporary name is released after the scope");
    for (int i = 0; i < 100; ++i) {
        dynamicPass("First", i);
        dynamicPass("Second", i);
    }
    require(GpuPassCapabilities::Contains("First") && GpuPassCapabilities::Contains("Second"),
            "One dynamic call site registers changing feature prefixes");
    {
        std::scoped_lock lock(GetGpuPassCapabilityRegistry().mutex);
        require(GpuPassCapabilities::Register("First::Cached"), "Known prefixes do not lock the shared registry again");
    }
    require(!GpuPassCapabilities::Register("NoSeparator") && !GpuPassCapabilities::Register("::NoPrefix"),
            "Invalid capability names are ignored");
    std::thread worker([] {
        for (int i = 0; i < 100; ++i) {
            GpuPassCapabilities::Register("First::Worker");
            GpuPassCapabilities::Register("Worker::Pass");
        }
    });
    for (int i = 0; i < 100; ++i) GpuPassCapabilities::Register("First::Main");
    worker.join();
    require(GpuPassCapabilities::Contains("Worker"), "Thread-local registration publishes to the shared registry");
    require(GetGpuPassCapabilityRegistry().featurePrefixes.size() == 4, "Repeated prefixes stay deduplicated");
}
'''
        source = source.replace("MACROS_HEADER", (ROOT / "src/Utils/Macros.h").as_posix())
        source = source.replace("REGISTRY", implementation[implementation.index("namespace\n{"):implementation.index("#ifdef TRACY_ENABLE")])
        source = source.replace("DYNAMIC_MACRO", header[header.index("#define CS_GPU_PASS_DYNAMIC"):])
        runtime.SceneSettingsRuntimeTests.compile_and_run(self, source)


if __name__ == "__main__":
    unittest.main()
