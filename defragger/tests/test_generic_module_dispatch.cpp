// SPDX-License-Identifier: GPL-3.0-or-later
/* Native, read-only hosted Common V2 inventory and fail-closed dispatch test. */
#include "json.hpp"
#include "process.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

static void ensure(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}
static bool format_name(std::string_view name)
{
    if (name.empty() || name.size() > 15U) return false;
    for (const unsigned char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
    return true;
}
static bool module_name(std::string_view name)
{
    if (name.size() < 4U || name.size() > 62U ||
        name.substr(name.size()-3U) != ".so") return false;
    for (const unsigned char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
            return false;
    return true;
}
static defragger::CommandResult run(const std::string& engine,
                                   const std::vector<std::string>& args)
{
    std::vector<std::string> argv{engine};
    argv.insert(argv.end(), args.begin(), args.end());
    return defragger::run_capture(argv, 2U * 1024U * 1024U,
                                  std::chrono::seconds(20));
}
static void test_inventory(const std::string& engine)
{
    const auto result = run(engine, {"--list-installed-modules"});
    ensure(result.return_code == 0, "Common V2 inventory failed");
    const auto inventory = defragger::Json::parse(result.standard_output);
    ensure(inventory.at("schema").unsigned_value() == 1U, "wrong schema");
    const bool available = inventory.at("authority_available").boolean();
    const auto& modules = inventory.at("modules").array();
    ensure(available || modules.empty(), "missing authority lists modules");
    for (const auto& module : modules) {
        ensure(format_name(module.at("format_id").string()), "bad format ID");
        ensure(module_name(module.at("filename").string()), "bad module name");
        ensure(module.at("abi").unsigned_value() == 2U, "not Common V2");
        const auto mask = module.at("group_mask").unsigned_value();
        const auto capabilities = module.at("capabilities").unsigned_value();
        const bool defrag = module.at("defrag_group").boolean();
        ensure(defrag == ((mask & 8U) != 0U), "wrong DEFRAG group flag");
        ensure(defrag || capabilities == 0U,
               "CORE-only provider advertised DEFRAG capabilities");
    }
}
static void test_no_untrusted_dispatch(const std::string& engine)
{
    char file[] = "/tmp/defragger-v2-native-XXXXXX";
    int fd = mkstemp(file);
    ensure(fd >= 0, "mkstemp failed");
    constexpr char original[] = "UNMODIFIED";
    const auto written = write(fd, original, sizeof(original)-1U);
    const int closed = close(fd);
    if (written != static_cast<ssize_t>(sizeof(original)-1U) || closed != 0) {
        (void)unlink(file);
        throw std::runtime_error("cannot create temporary image");
    }
    try {
        const auto absent = run(engine,
            {"defrag", file, "--filesystem", "zzdefragtest"});
        ensure(absent.return_code != 0, "missing V2 SO was accepted");
        ensure(absent.standard_error.find(
            "no installed trusted Common V2 SO qualifies for zzdefragtest") !=
            std::string::npos, "wrong missing-module diagnostic");
        std::ifstream image(file, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(image)),
                                std::istreambuf_iterator<char>());
        ensure(bytes == original, "refused operation altered image");
    } catch (...) { (void)unlink(file); throw; }
    ensure(unlink(file) == 0, "cannot remove test image");
    const auto invalid = run(engine,
        {"defrag", "/dev/null", "--filesystem", "bad/path"});
    ensure(invalid.return_code == 2, "invalid module identifier accepted");
    ensure(invalid.standard_error.find("invalid generic filesystem identifier") !=
           std::string::npos, "wrong invalid-identifier diagnostic");
}
int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s ENGINE\n", argv[0]);
        return 2;
    }
    try {
        test_inventory(argv[1]);
        test_no_untrusted_dispatch(argv[1]);
        std::puts("native Common V2 module inventory and refusal passed");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Common V2 inventory failed: %s\n", e.what());
        return 1;
    }
}
