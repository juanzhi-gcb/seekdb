#pragma once
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>
#include <unistd.h>
namespace minivectordb::detail {
// One bounded helper; only pure row encoding runs here. Storage/index/root
// publication stays on the caller. Each submission is joined before returning.
class BatchWorker {
  const pid_t owner_=::getpid();
  std::mutex mutex_;
  std::condition_variable wake_,finished_;
  void* argument_=nullptr;
  void (*function_)(void*)=nullptr;
  std::exception_ptr failure_;
  bool ready_=false,done_=true,stop_=false;
  std::thread thread_;
  void loop() noexcept {
    std::unique_lock lock(mutex_);
    for(;;){
      wake_.wait(lock,[&]{return ready_||stop_;});if(stop_)return;
      auto fn=function_;auto arg=argument_;ready_=false;lock.unlock();
      std::exception_ptr failure;try{fn(arg);}catch(...){failure=std::current_exception();}
      lock.lock();failure_=failure;done_=true;finished_.notify_one();
    }
  }
public:
  BatchWorker():thread_([this]{loop();}){}
  BatchWorker(const BatchWorker&)=delete;
  pid_t owner() const noexcept{return owner_;}
  ~BatchWorker(){
    {std::lock_guard lock(mutex_);stop_=true;}wake_.notify_one();thread_.join();
  }
  template<class F> void start(F& function){
    std::lock_guard lock(mutex_);
    argument_=&function;function_=[](void* p){(*static_cast<F*>(p))();};
    failure_={};done_=false;ready_=true;wake_.notify_one();
  }
  void finish(){
    std::unique_lock lock(mutex_);finished_.wait(lock,[&]{return done_;});
    if(failure_)std::rethrow_exception(failure_);
  }
};
}
