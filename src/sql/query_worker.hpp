#pragma once
#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>

namespace minivectordb::sql {
// One lazy, sleeping worker per Database. The executor joins each request's
// task before any captured query state or immutable snapshot can change.
class QueryWorker {
  std::mutex mutex_;
  std::condition_variable ready_,finished_;
  std::thread thread_;
  std::function<void()> task_;
  std::exception_ptr error_;
  bool stopping_=false,pending_=false,done_=true;
  void loop() noexcept {
    std::unique_lock lock(mutex_);
    for(;;) {
      ready_.wait(lock,[&]{return stopping_||pending_;});
      if(stopping_)return;
      auto task=std::move(task_);pending_=false;
      lock.unlock();std::exception_ptr error;
      try{task();}catch(...){error=std::current_exception();}
      lock.lock();error_=error;done_=true;finished_.notify_one();
    }
  }
public:
  QueryWorker()=default;
  QueryWorker(const QueryWorker&)=delete;
  QueryWorker& operator=(const QueryWorker&)=delete;
  ~QueryWorker() {
    if(thread_.joinable()){{std::lock_guard lock(mutex_);stopping_=true;}ready_.notify_one();thread_.join();}
  }
  template<class A,class B> void run(A&& first,B&& second) {
    // Construct the potentially allocating closure before creating/dispatching
    // any work. Failure cannot leave a task using expired caller references.
    std::function<void()> task(std::forward<B>(second));
    if(!thread_.joinable())thread_=std::thread([this]{loop();});
    {
      std::lock_guard lock(mutex_);task_=std::move(task);error_=nullptr;done_=false;pending_=true;
    }
    ready_.notify_one();std::exception_ptr first_error;
    try{first();}catch(...){first_error=std::current_exception();}
    std::unique_lock lock(mutex_);finished_.wait(lock,[&]{return done_;});
    auto second_error=error_;lock.unlock();
    if(first_error)std::rethrow_exception(first_error);
    if(second_error)std::rethrow_exception(second_error);
  }
};
}
