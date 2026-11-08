// SPDX-License-Identifier: GPL-3.0-or-later
// Native C++17 source-ownership regression for system-managed APT updates.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
namespace fs=std::filesystem;
static void require(bool value,const std::string& reason){
    if(!value)throw std::runtime_error(reason);
}
static std::string read(const fs::path& path){
    std::ifstream stream(path,std::ios::binary);
    require(bool(stream),"Cannot read "+path.string());
    return {std::istreambuf_iterator<char>(stream),std::istreambuf_iterator<char>()};
}
static void includes(const std::string& src,const std::string& token,const std::string& file){
    require(src.find(token)!=std::string::npos,file+" must contain: "+token);
}
static void excludes(const std::string& src,const std::string& token,const std::string& file){
    require(src.find(token)==std::string::npos,file+" still contains: "+token);
}
int main(int argc,char**argv){
    try{
        require(argc==2,"Usage: test-update-service SOURCE_ROOT");
        const fs::path root=argv[1];
        const std::string desktop=read(root/"packaging/io.github.defragmenter.desktop");
        const std::string media=read(root/"packaging/io.github.defragmenter.TestMedia.desktop");
        const std::string cmake=read(root/"cmake/test_media.cmake");
        const std::string package=read(root/"cmake/project.cmake");
        const std::string runner=read(root/"packaging/local-run-header.sh.in");
        const std::string gui=read(root/"native/desktop.cpp");
        includes(desktop,"Exec=/Programs/defragmenter","Desktop launcher");
        includes(desktop,"TryExec=/Programs/defragmenter","Desktop launcher");
        includes(desktop,"Icon=/System/Resources/Icons/defragmenter.png","Desktop launcher");
        includes(media,"Icon=/System/Resources/Icons/defragmenter.png","Test Media launcher");
        excludes(desktop,"CheckUpdates","Desktop launcher");
        excludes(desktop,"--check-updates","Desktop launcher");
        includes(cmake,"OUTPUT_NAME defragmenter","Test Media build");
        includes(cmake,"install(TARGETS defragmenter-desktop RUNTIME DESTINATION /Programs)","Test Media build");
        excludes(package,"install(PROGRAMS packaging/defragmenter","Package builder");
        require(!fs::exists(root/"packaging/defragmenter"),
                "Retired packaging update launcher has returned");
        excludes(desktop,"/usr/bin/defragmenter","Desktop launcher");
        excludes(gui,"auth_timer_","Desktop GUI");
        includes(gui,"else start_helper();","Explicit privileged action");
        includes(gui,"void start_helper()","Privileged helper entry point");
        excludes(package,"gui/update_helper.py","Package builder");
        for(const auto* retired:{"gui/update_helper.py","gui/ui/update_launcher.py",
                                 "gui/ui/update_service.py","tests/test_update_service.py"})
            require(!fs::exists(root/retired),std::string("Retired Python component exists: ")+retired);
        includes(runner,"normal system updater","Local native runner");
        excludes(runner,"detected by Defragmenter itself","Local native runner");
        std::cout<<"Defragmenter system-update contract passed (C++17)\n";
        return 0;
    }catch(const std::exception& error){
        std::cerr<<"Defragmenter system-update contract failed: "<<error.what()<<'\n';
        return 1;
    }
}
