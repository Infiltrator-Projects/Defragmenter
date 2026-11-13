// SPDX-License-Identifier: GPL-3.0-or-later
// Independent C++17 regression: production filesystem mutation must not
// delegate work to CLI filesystem tools or mount/unmount programs.
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>
namespace fs=std::filesystem;
static std::string read_lower(const fs::path& path){
    std::ifstream file(path,std::ios::binary);
    if(!file)throw std::runtime_error("Cannot read "+path.string());
    std::string source{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
    std::transform(source.begin(),source.end(),source.begin(),
                   [](unsigned char c){return static_cast<char>(std::tolower(c));});
    return source;
}
static bool word_char(char c){
    const unsigned char v=static_cast<unsigned char>(c);
    return (v>='a'&&v<='z')||(v>='0'&&v<='9')||v=='_';
}
static bool tool_token(const std::string& source,const std::string& needle){
    std::size_t start=0;
    while((start=source.find(needle,start))!=std::string::npos){
        const auto finish=start+needle.size();
        if((start==0||!word_char(source[start-1]))&&
           (finish==source.size()||!word_char(source[finish])))
            return true;
        ++start;
    }
    return false;
}
int main(int argc,char** argv){
    try{
        if(argc!=2)throw std::runtime_error("Usage: test-no-external-fs-tools SOURCE_ROOT");
        const fs::path root=argv[1];
        const std::vector<std::string> forbidden={
            "e2image","resize2fs","e2fsck","xfs_repair","xfs_db","xfs_fsr",
            "ntfsfix","ntfsresize","ntfsclone","ntfs-3g",
            "fsck.exfat","exfatfsck","btrfs filesystem defragment","losetup",
            "mkfs.fat","mkfs.exfat","mkfs.ext2","mkfs.ext3","mkfs.ext4",
            "mkfs.xfs","mkfs.btrfs","mkfs.hfsplus","mkfs.minix",
            "mkntfs","mkswap","hformat"
        };
        const std::vector<std::regex> patterns={
            std::regex(R"REGEX("umount"\s*,)REGEX"),
            std::regex(R"REGEX(["']mount["']\s*,)REGEX"),
            std::regex(R"REGEX(["'](zpool|zfs)["']\s*,\s*["'](create|import|mount|export)["'])REGEX"),
            std::regex(R"REGEX(["']btrfs["']\s*,\s*["']check["'])REGEX"),
            std::regex(R"REGEX(["']btrfs["']\s*,\s*["']filesystem["']\s*,\s*["']defragment["'])REGEX"),
            std::regex(R"REGEX(\bbtrfs\s+check\b[\s\S]*(subprocess|exec|spawn|system))REGEX")
        };
        const std::vector<std::string> pattern_labels={
            R"("umount"\s*,)",
            R"(["']mount["']\s*,)",
            R"(["'](?:zpool|zfs)["']\s*,\s*["'](?:create|import|mount|export)["'])",
            R"(["']btrfs["']\s*,\s*["']check["'])",
            R"(["']btrfs["']\s*,\s*["']filesystem["']\s*,\s*["']defragment["'])",
            R"(\bbtrfs\s+check\b.*(?:subprocess|exec|spawn|system))"
        };
        std::vector<fs::path> inputs;
        for(const auto& name:{"gui","src","packaging","test_media"}){
            const auto directory=root/name;
            if(!fs::exists(directory))continue;
            for(const auto& item:fs::recursive_directory_iterator(directory)){
                if(!item.is_regular_file())continue;
                const auto extension=item.path().extension().string();
                if(extension==".py"||extension==".c"||extension==".h"||extension==".sh")
                    inputs.push_back(item.path());
            }
        }
        inputs.push_back(root/"CMakeLists.txt");
        inputs.push_back(root/"install.sh");
        std::sort(inputs.begin(),inputs.end());
        std::vector<std::string> failures;
        for(const auto& path:inputs){
            const std::string source=read_lower(path);
            const auto filename=path.lexically_relative(root).generic_string();
            for(const auto& needle:forbidden)
                if(tool_token(source,needle))
                    failures.push_back(filename+": "+needle);
            for(std::size_t i=0;i<patterns.size();++i)
                if(std::regex_search(source,patterns[i]))
                    failures.push_back(filename+": "+pattern_labels[i]);
        }
        if(!failures.empty()){
            std::cerr<<"Production code must implement filesystems natively; "
                     <<"external filesystem tool orchestration found:\n";
            for(const auto& issue:failures)std::cerr<<"  - "<<issue<<'\n';
            return 1;
        }
        if(fs::exists(root/"tests/test_no_external_fs_tools.py"))
            throw std::runtime_error("Retired Python external-tools contract returned");
        std::cout<<"Production filesystem external-tool prohibition passed (native C++17)\n";
        return 0;
    }catch(const std::exception& e){
        std::cerr<<"Native filesystem dependency regression failed: "<<e.what()<<'\n';
        return 1;
    }
}
