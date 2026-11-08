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
        const auto loader=read(root/"native/fat_module_loader.c");
        for(const char* token:{
            "infiltrator_filesystem_defrag_v1",
            "FAT12.so","FAT16.so","FAT32.so","O_NOFOLLOW","trusted_module"
        })contains(loader,token,"Trusted filesystem module loader");
        const auto runtime=read(root/"native/runtime.cpp");
        for(const char* token:{
            "for (const int bits : {12, 16, 32})",
            "write, \"exact\", \"fat-native\", MapAdapter::Fat",
            "standard_write_ops(\"fat-native\")"
        })contains(runtime,token,"Versioned native FAT operations");
        check(!fs::exists(root/"tests/test_fat_relayout_architecture.py"),
              "Retired Python FAT ownership checker still exists");
        std::cout<<"FAT operations use a versioned Filesystems module\n";
        return 0;
    }catch(const std::exception& error){
        std::cerr<<"FAT ownership contract failed: "<<error.what()<<'\n';
        return 1;
    }
}
