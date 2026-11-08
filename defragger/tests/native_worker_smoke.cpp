// SPDX-License-Identifier: GPL-3.0-or-later
// Test-only native filesystem worker smoke probes. No Python subprocesses,
// no privileged operations and no production block devices are involved.
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
namespace fs=std::filesystem;

static void require(bool yes,const std::string& explanation){
    if(!yes)throw std::runtime_error(explanation);
}
static int invoke(const std::vector<std::string>& cmd,bool from_root,
                  std::chrono::seconds limit=std::chrono::seconds(20)){
    require(!cmd.empty(),"missing native worker command");
    const pid_t child=fork();
    require(child>=0,"could not fork native worker");
    if(child==0){
        const int nullfd=open("/dev/null",O_RDWR|O_CLOEXEC);
        if(nullfd<0)_exit(127);
        if(dup2(nullfd,STDOUT_FILENO)<0||dup2(nullfd,STDERR_FILENO)<0)_exit(127);
        if(nullfd>STDERR_FILENO)close(nullfd);
        if(from_root&&chdir("/")!=0)_exit(127);
        std::vector<char*> argv;
        for(const auto& a:cmd)argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execv(argv[0],argv.data());
        _exit(127);
    }
    int status=0;
    const auto deadline=std::chrono::steady_clock::now()+limit;
    while(true){
        const pid_t done=waitpid(child,&status,WNOHANG);
        if(done==child)break;
        if(done<0){
            if(errno==EINTR)continue;
            kill(child,SIGKILL);
            while(waitpid(child,&status,0)<0&&errno==EINTR){}
            throw std::runtime_error("could not wait for native worker");
        }
        if(std::chrono::steady_clock::now()>=deadline){
            kill(child,SIGKILL);
            while(waitpid(child,&status,0)<0&&errno==EINTR){}
            throw std::runtime_error("native worker timed out: "+cmd[0]);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return WIFEXITED(status)?WEXITSTATUS(status):128+WTERMSIG(status);
}
static fs::path build_root(){
    const char* override=std::getenv("DEFRAGMENTER_BUILD_DIR");
    return override&&*override?fs::absolute(override):fs::absolute("build");
}
struct TemporaryDirectory{
    fs::path path;
    TemporaryDirectory(){
        auto pattern=(fs::temp_directory_path()/"defragmenter-native-smoke-XXXXXX").string();
        std::vector<char> bytes(pattern.begin(),pattern.end());bytes.push_back('\0');
        char* result=mkdtemp(bytes.data());
        require(result!=nullptr,"cannot allocate private scratch directory");
        path=result;
    }
    ~TemporaryDirectory(){std::error_code ec;if(!path.empty())fs::remove_all(path,ec);}
    TemporaryDirectory(const TemporaryDirectory&)=delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&)=delete;
};
static void malformed_ext_input(){
    const auto worker=build_root()/"defragmenter-ext-worker";
    require(fs::is_regular_file(worker),"EXT worker is not built: "+worker.string());
    TemporaryDirectory temp;
    const auto image=temp.path/"garbage.img";
    const int randomfd=open("/dev/urandom",O_RDONLY|O_CLOEXEC);
    require(randomfd>=0,"cannot read operating system entropy");
    std::ofstream output(image,std::ios::binary|std::ios::trunc);
    if(!output){close(randomfd);throw std::runtime_error("cannot write malformed EXT fixture");}
    std::vector<char> buffer(65536);
    for(int n=0;n<16;n++){
        std::size_t filled=0;
        while(filled<buffer.size()){
            const ssize_t received=read(randomfd,buffer.data()+filled,buffer.size()-filled);
            if(received<0&&errno==EINTR)continue;
            require(received>0,"cannot fill malformed EXT fixture");
            filled+=std::size_t(received);
        }
        output.write(buffer.data(),std::streamsize(buffer.size()));
        require(bool(output),"failed writing malformed EXT fixture");
    }
    close(randomfd);
    output.close();
    require(bool(output),"failed closing malformed EXT fixture");
    const int status=invoke({worker.string(),"analyse-json",image.string()},false);
    require(status!=0,"native EXT worker accepted malformed non-EXT input");
    std::cout<<"native EXT malformed-input fail-closed test passed\n";
}
static void worker_versions(){
    const std::vector<std::pair<std::string,std::string>> workers={
        {"defragmenter-ext-worker","DEFRAGMENTER_EXT_WORKER"},
        {"defragmenter-ntfs-worker","DEFRAGMENTER_NTFS_WORKER"},
        {"defragmenter-exfat-worker","DEFRAGMENTER_EXFAT_WORKER"},
        {"defragmenter-xfs-worker","DEFRAGMENTER_XFS_WORKER"},
        {"defragmenter-fat-worker","DEFRAGMENTER_FAT_WORKER"},
        {"defragmenter-affs-worker","DEFRAGMENTER_AFFS_WORKER"},
        {"defragmenter-hfsplus-worker","DEFRAGMENTER_HFSPLUS_WORKER"},
    };
    for(const auto& [name,env]:workers){
        const char* alternative=std::getenv(env.c_str());
        const fs::path worker=fs::absolute(alternative&&*alternative?
                                            fs::path(alternative):build_root()/name);
        require(fs::is_regular_file(worker),"missing native worker: "+worker.string());
        const int status=invoke({worker.string(),"--version"},true);
        require(status==0,"native worker version entry point failed: "+name);
    }
    std::cout<<"native worker entry-point/version tests passed\n";
}
int main(int argc,char**argv){
    try{
        require(argc==2,"Usage: native-worker-smoke {ext-catalog-fuzz|worker-entrypoints}");
        const std::string mode=argv[1];
        if(mode=="ext-catalog-fuzz")malformed_ext_input();
        else if(mode=="worker-entrypoints")worker_versions();
        else throw std::runtime_error("unknown worker test mode");
        return 0;
    }catch(const std::exception&error){
        std::cerr<<"Native worker smoke test failed: "<<error.what()<<'\n';
        return 1;
    }
}
