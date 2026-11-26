// SPDX-License-Identifier: GPL-3.0-or-later
// Native ownership contract: FAT defragmentation is provided by authoritative
// filesystem shared modules, not another private GUI FAT implementation.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
namespace fs=std::filesystem;
static void check(bool yes,const std::string& why) {
    if(!yes)throw std::runtime_error(why);
}
static std::string read(const fs::path& path){
    std::ifstream f(path,std::ios::binary);
    check(bool(f),"cannot read "+path.string());
    return {std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};
}
static void contains(const std::string& source,const char* token,const char* role) {
    check(source.find(token)!=std::string::npos,
          std::string(role)+" is missing: "+token);
}
int main(int argc,char** argv) {
    try {
        check(argc==2,"Usage: test-fat-relayout-architecture SOURCE_ROOT");
        const fs::path root=argv[1];
        check(!fs::exists(root/"gui/filesystems/fat/native"),
              "Private FAT engine must not return to Defragmenter GUI");
        // Test media must also delegate all filesystem operations to Common V2.
        for(const char* retired:{"native_fat_fixture.cpp","fat12_small_workspace_fixture.c","make_fat16_dependency_blocker_image.py","make_fat16_workspace_image.py","make_fragmented_directory_image.py","make_gapped_contiguous_image.py","make_growth_defrag_image.py","verify_defragged_image.py","verify_directory_defrag.py","verify_fat16_dependency_blocker_image.py","verify_fat16_workspace_image.py","verify_gapped_contiguous.py","verify_growth_defrag.py","verify_growth_fat12_16.py","verify_fragment_map.py","test_fat_module_recovery.sh","test_fat_module_safety.sh","test_fat_relayout.c","test_fat_directory.c","test_fat_volume.c","test_fat_journal.c"})
            check(!fs::exists(root/"tests"/retired),
                  std::string("Private or retired FAT implementation reintroduced: ")+retired);
        check(!fs::exists(root/"test_media/test_media_fat12_common.c"),
              "Dedicated FAT Test Media adapter must not return");
        const auto fixture=read(root/"test_media/test_media_common_v2.c");
        for(const char* token:{"IFS_FS_COMMON_EXPORT_V2","volume->format",
                               "core->create","core->write","core->read",
                               "volume->check","map->map","profile_geometry_valid"})
            contains(fixture,token,"Unified Common V2 test-media consumer");
        const auto loader=read(root/"native/fat_module_loader.c");
        for(const char* token:{
            "ifs_filesystem_module_find_readonly_v2",
            "ifs_filesystem_module_run",
            "probe-only","fat12","fat16","fat32"
        })contains(loader,token,"Trusted Common V2 FAT dispatcher");
        check(loader.find("infiltrator_filesystem_defrag_v1")==std::string::npos,
              "Retired FAT defrag V1 loader reintroduced");
        check(!fs::exists(root/"native/fat_defrag_abi.h"),
              "Retired FAT defrag V1 host interface still present");
        const auto runtime=read(root/"native/runtime.cpp");
        contains(runtime,"ifs_filesystem_module_visit_v2","Installed-provider inventory");
        check(runtime.find("MapAdapter::Fat")==std::string::npos &&
              runtime.find("for (const int bits")==std::string::npos,
              "Host must not carry a FAT-specific registry or map adapter");
        const auto host=read(root/"native/filesystem_module_host.c");
        const auto mapper=read(root/"native/map.cpp");
        check(host.find("v2_fat_")==std::string::npos &&
              mapper.find("map_fat(")==std::string::npos,
              "Host must not carry FAT-specific mapping");
        const auto engine=read(root/"native/operation_engine.cpp");
        contains(engine,"ifs_filesystem_module_detect_installed_v2","Generic installed provider probing");
        contains(engine,"ifs_filesystem_module_run","Common V2 operation dispatch");
        check(!fs::exists(root/"tests/test_fat_relayout_architecture.py"),
              "Retired Python FAT ownership checker still exists");
        std::cout<<"FAT operations use only independently trusted Common V2 SO modules\n";
        return 0;
    }catch(const std::exception& error){
        std::cerr<<"FAT ownership contract failed: "<<error.what()<<'\n';
        return 1;
    }
}
