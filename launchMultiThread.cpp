#include <sched.h>
#include <pthread.h>
#include <cstring>
#include <iostream>
#include <fstream>
#include <thread>
#include <atomic>
#include <memory>
#include <functional>
#include <vector>
#include <chrono>
#include <mutex>

#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>
#include <asm/unistd.h>
#include <x86intrin.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>


class ClockCounter{
private:
    static int perf_event_open(struct perf_event_attr *hw_event,
                            pid_t pid,
                            int cpu,
                            int group_fd,
                            unsigned long flags ){
        int ret = syscall( __NR_perf_event_open, hw_event, pid, cpu, group_fd, flags );
        return ret;
    }

    static ClockCounter* instance;
    ClockCounter(){
        struct perf_event_attr attr;
        int perf_fd;


        memset(&attr, 0, sizeof(attr));
        attr.type = PERF_TYPE_HARDWARE;
        attr.size = sizeof(attr);
        attr.config = PERF_COUNT_HW_CPU_CYCLES;

        this->perf_fd = perf_event_open(&attr, 0, -1, -1, 0);
        if (this->perf_fd == -1) {
            perror("perf_event_open");
            exit(1);
        }
    }

    int perf_fd;

public:
    static void createInstance(){
        instance = new ClockCounter();
    }

    static ClockCounter* getInstance(){
        if(instance == nullptr){
            createInstance();
        }
        return instance;
    }

    long long getCounter(){
        long long val;
        read(perf_fd, &val, sizeof(val));
        return val;
    }
};


ClockCounter* ClockCounter::instance = nullptr;


class ThreadWrapper{
private:
    std::thread thread;
    std::atomic_int change_cpu_affinity_counter;
    std::atomic<bool> is_finished;
    int cpu_affinity;
    const int total_thread;
    std::mutex native_handle_mtx;

    void setCPUAffinity(int cpu_id){

        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(cpu_id, &cpuset);

        int rc = pthread_setaffinity_np(this->thread.native_handle(),
                                        sizeof(cpu_set_t),
                                        &cpuset);
        if(rc != 0){
            std::cout << "Failed to set affinity " << std::strerror(errno) << std::endl; 
        }
        else{
            // std::cout << "change to cpu_id: " << cpu_id << std::endl;
            this->change_cpu_affinity_counter++;
        }
    }

public:

    ThreadWrapper(std::function<void(int)> task, int cpu_affinity = -1)
    : change_cpu_affinity_counter(0), total_thread(std::thread::hardware_concurrency()){
        this->is_finished.store(false);
        this->cpu_affinity = cpu_affinity;


        // this->thread wiil be detached long after this constructor to get native_handle
        this->thread = std::thread([task, cpu_affinity, this]{
            std::this_thread::sleep_for(std::chrono::seconds(1)); // wait for pinning to the specified cpu
            task(cpu_affinity);
            this->is_finished.store(true);
        });
        
        if(cpu_affinity >= 0){
            this->setCPUAffinity(cpu_affinity);
        }
    }

    bool isFinished(){
        return this->is_finished.load();
    }

    void changeCPUAffinity(){
        do{ // skip if cpu_id == 0 as it is not tick-less.
            this->cpu_affinity = (this->cpu_affinity+1) % this->total_thread;
        }while(this->cpu_affinity == 0);

        this->setCPUAffinity(this->cpu_affinity);
    }

    int getChangeCPUAffinityCounter() const{return this->change_cpu_affinity_counter.load();};

    void detach(){this->thread.detach();}
};


void writeOutResult(const std::vector<int64_t>& v){
    std::ofstream ofs("./output.txt");
    ofs << "time" << "\n";
    for(auto val : v) ofs << val << '\n';
    ofs.close();
}


void add(int check_cpu_affinity){

    int64_t counter = 50'000'000;
    int64_t ans = 0;
    std::vector<int64_t> elapsed_times;
    elapsed_times.reserve(counter);
    auto clk_counter = ClockCounter::getInstance();

    for(int idx = 0; idx < counter; idx++){

        auto st = clk_counter->getCounter();
        __sync_synchronize();
        ans += idx;
        auto te = clk_counter->getCounter();

        elapsed_times.push_back(te-st);
    }

    std::cout << "result = " << ans << std::endl;
    writeOutResult(elapsed_times);
    std::cout << ans << std::endl;
}

int main(){

    std::cout << "show hardware concurrency: " << std::thread::hardware_concurrency() << std::endl;

    int th_num = 1; std::vector<std::unique_ptr<ThreadWrapper>> threads(th_num);
    for(int th_idx = 0; th_idx < th_num; th_idx++){
        std::cout << "start #th: " << th_idx << std::endl;
        threads[th_idx] = std::make_unique<ThreadWrapper>(add, th_idx+1);
    }

    std::this_thread::sleep_for(std::chrono::seconds(2));

    while(true){
        bool ok = true;
        for(auto& th : threads) ok = ok && th->isFinished();
        if(ok){
            std::cout << "Finished" << std::endl;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        for(auto& th : threads) th->changeCPUAffinity();
    }

    for(int th_idx = 0; th_idx < th_num; th_idx++){
        std::cout << "Change cpu_id counter #th " << th_idx << ", " << threads[th_idx]->getChangeCPUAffinityCounter() << std::endl;
    }

    // threads are detached here
    for(auto& th : threads) th->detach();

    return 0;
}