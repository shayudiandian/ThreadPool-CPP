#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <iostream>

#include <vector>
#include <queue>
#include <unordered_map>

#include <memory>

#include <functional>

#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <future>


const int TASK_MAX_THRESHHOLD = 2;                 // INT32_MAX
const int THREAD_MAX_THRESHHOLD = 1024;
const int THREAD_MAX_IDLE_TIME = 60;               // 单位 s


// 线程池支持的模式
enum class PoolMode {
	MODE_FIXED,    // 固定数量的线程
	MODE_CACHED,   // 线程数量可动态增长
};


/* 线程类 */ 
// 对 std::thread 的一个简单封装。主要负责创建并启动一个实际的 C++ 线程来执行给定的函数对象。
class Thread {
public:
	// 线程函数对象类型
    // std::function       存储一个可调用对象，调用形式为: 参数int，返回void
	using ThreadFunc = std::function<void(int)>;        

    // 线程构造
	Thread(ThreadFunc func) : func_(func), threadId_(generateId_++) {}

	// 线程析构
	~Thread() = default;

	// 启动线程
	void start() {
	    // 创建一个新线程对象t，并在新线程中执行线程函数func_，该函数以threadId_作为参数
	    std::thread t(func_, threadId_);
	    t.detach();
        // 线程对象t出作用域就被销毁，⽽此时线程函数仍在运⾏。所以在使⽤ std::thread 类时，必须保证线程函数运⾏期间其线程对象有效。
	    // std::thread 类提供了⼀个 detach ⽅法，通过这个方法可以让线程对象与线程函数脱离关系，这样即使线程对象被销毁，也不影响线程函数的运⾏。 
    }

	// 获取线程id
	int getId() const {
        return threadId_;
    }

private:
    ThreadFunc func_;              // 线程函数对象
	static int generateId_;
	int threadId_;                 // 保存线程id
};

int Thread::generateId_ = 0;



/* 线程池类 */ 
// 管理线程的创建、任务的提交、执行以及线程资源的释放
class ThreadPool {
public:
	// 线程池构造
	ThreadPool() : 
        initThreadSize_(0), 
        taskSize_(0), 
	    idleThreadSize_(0), 
	    curThreadSize_(0), 
	    taskQueMaxThreshHold_(TASK_MAX_THRESHHOLD), 
	    threadSizeThreshHold_(THREAD_MAX_THRESHHOLD), 
	    poolMode_(PoolMode::MODE_FIXED),      // 默认固定数量的线程模式
	    isPoolRunning_(false)
    {}

	// 线程池析构
	~ThreadPool() {
        isPoolRunning_ = false;
		std::unique_lock<std::mutex> lock(taskQueMtx_);
		// 唤醒所有仍然等待任务（阻塞）的线程，让它们开始退出
		// 当线程被唤醒后，会检查 isPoolRunning_ 的状态，看到 false 时，它们会开始执行退出逻辑并销毁自己。
		notEmpty_.notify_all();
		// 线程池在析构时，会等待所有线程退出，而不是立刻结束。
		// wait会阻塞当前线程，直到条件满足———所有线程都已从threads_容器中删除。
		// 线程池中的线程是异步运行的，即使我们设置了 isPoolRunning_ 为 false 并通知线程退出，但这些线程可能还在处理当前的任务，或者正在退出过程中。
		// 因此，使用 exitCond_ 来等待所有线程彻底退出，避免在线程还未完全销毁时析构 ThreadPool 对象，防止资源泄露或访问非法内存。
		//（困难之处）
		exitCond_.wait(lock, [&]()->bool { return threads_.size() == 0; });
    }

	// 设置线程池的工作模式
	void setMode(PoolMode mode) {
		if (checkRunningState()) return;
		poolMode_ = mode;
    }

	// 设置task任务队列上线阈值
	void setTaskQueMaxThreshHold(int threshhold) {
		if (checkRunningState()) return;
		taskQueMaxThreshHold_ = threshhold;
    }

	// 设置线程池在cached模式下的线程阈值
	void setThreadSizeThreshHold(int threshhold) {
        if (checkRunningState()) return;
		if (poolMode_ == PoolMode::MODE_CACHED) {
			threadSizeThreshHold_ = threshhold;
		}
    }

	// 向线程池提交任务，并通过线程池执行任务，同时返回一个 std::future 对象供调用者获取任务结果
	// 使用可变参数模板，让 submitTask 可以接收任意任务函数和任意数量的参数
    template<typename Func, typename... Args>
	// auto submitTask(Func&& func, Args&&... args) -> std::future<decltype(func(args...))>
	// 无须返回类型后置 C++14，推导返回类型 auto 为 std::future<decltype(func(args...))>
    auto submitTask(Func&& func, Args&&... args) {
        // 打包任务，放入任务队列里面

		using RType = decltype(func(args...));        // 推导任务函数返回类型  （困难之处）

		// 使用std::packaged_task来包装任务，以便任务能够异步执行，并能通过std::future来获取任务的结果
		// 任务用std::bind绑定器，注意使用std::forward完美转发
		// 注意，std::bind已经绑定了1个参数，所以bind返回的可调用对象是RType()类型的，即不需要参数了
		// task 是一个智能指针，指向 std::packaged_task<RType()> 对象
		// （困难之处）
		auto task = std::make_shared<std::packaged_task<RType()>>(std::bind(std::forward<Func>(func), std::forward<Args>(args)...));
		std::future<RType> result = task->get_future();

		// 这是用户调用的接口submitTask，不能把用户的线程阻塞住
        // 用户提交任务，如果任务队列满了，等待，最长不能阻塞（等待）超过 1s，否则wait_for返回false，表示提交任务失败 
		//（困难之处）
		std::unique_lock<std::mutex> lock(taskQueMtx_);       // 确保在任务队列上的所有操作都是线程安全的
		if (!notFull_.wait_for(lock, std::chrono::seconds(1), [&]()->bool { return taskQue_.size() < (size_t)taskQueMaxThreshHold_; })) {
			std::cerr << "task queue is full, submit task fail." << std::endl;
			// 即使任务提交失败了，但为了保证接口的一致性（submitTask返回的是auto），还是需要返回一个 std::future
			// 为此，创建了一个默认的空任务对象 std::packaged_task<RType()>，并执行，以便能返回这个空任务的 std::future 对象
			//（困难之处）
			auto task = std::make_shared<std::packaged_task<RType()>>([]()->RType { return RType(); });
			(*task)();
			return task->get_future();       // 返回 std::future<RType> 类型

			// 注意任务提交失败了，这个任务就不会被线程池执行了，抛出异常并处理比较好，但调用者还要编写代码 try-catch 来捕获异常并处理
		}

		// 如果任务队列在1秒内有了空位，即任务队列有空余，则条件满足，wait_for返回true，允许继续向队列提交任务，把任务放入任务队列中
		// 将lambda表达式——即函数对象放入任务队列
		// 当线程池中的某个线程从任务队列中取出这个任务并执行时，它将调用这个lambda表达式，即执行(*task)()，即实际的用户任务
		taskQue_.emplace([task]() { (*task)(); });
		taskSize_++;

		// 因为新放了任务，任务队列肯定不空了，在notEmpty_上进行通知，赶快分配线程执行任务
		notEmpty_.notify_all();


		// 在cached模式下，当任务数量超过了空闲线程的数量并且当前线程数量还没有达到上限时，增加线程池中的线程
		// cached模式场景: 适合任务处理比较紧急 小而快的任务 
		if (poolMode_ == PoolMode::MODE_CACHED && taskSize_ > idleThreadSize_ && curThreadSize_ < threadSizeThreshHold_) {
			std::cout << ">>> create new thread..." << std::endl;
			// 创建新的线程对象Thread
            // make_unique<T>(...) 参数是传递给类型T的构造函数的参数，这里Thread类的构造函数的参数是可调用对象类型——ThreadFunc类型（可调用类型）std::function<void(int)>
            // 创建Thread线程对象的时候，把线程函数给到Thread线程对象

			// 注意，void ThreadPool::threadFunc(int threadId)，还有一个隐含的参数——指向 ThreadPool 类实例的 this 指针
			// 所以有个this参数，作用是让 std::bind 知道要调用哪个对象的成员函数，并保持对该对象的访问权
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			int threadId = ptr->getId();
            // 不能直接用ptr，unique_ptr不允许拷贝赋值，要用 std::move 转移
            // （困难之处）
			threads_.emplace(threadId, std::move(ptr));

			// 启动新的线程
			// 这里才是真的创建一个线程 thread，再执行线程函数
			threads_[threadId]->start();

			curThreadSize_++;
			idleThreadSize_++;
		}
		// 返回任务的Result对象
		return result;
    }

	// 开启线程池（初始线程数量为CPU核心数量）
	void start(int initThreadSize = std::thread::hardware_concurrency()) {
        // 设置线程池的运行状态
		isPoolRunning_ = true;

		// 记录初始线程个数
		initThreadSize_ = initThreadSize;
		curThreadSize_ = initThreadSize;

		// 创建线程对象Thread
		for (int i = 0; i < initThreadSize_; i++) {
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			int threadId = ptr->getId();
			threads_.emplace(threadId, std::move(ptr));
		}

		// 启动所有线程
		for (int i = 0; i < initThreadSize_; i++) {
			threads_[i]->start();
			idleThreadSize_++;
		}
    }

    // 禁止拷贝赋值
    // （注意点）
	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;


private:
	// 线程函数
	void threadFunc(int threadid) {
		auto lastTime = std::chrono::high_resolution_clock().now();
		// 所有任务必须执行完成，线程池才可以回收所有线程资源
		// 死循环确保线程池中的每个线程能够持续运行，不断地从任务队列中获取任务并执行，直到线程池结束
		for (;;) {
			Task task;
			{
				std::unique_lock<std::mutex> lock(taskQueMtx_);

				std::cout << "tid:" << std::this_thread::get_id() << "尝试获取任务..." << std::endl;

				// 每一秒中返回一次   怎么区分：超时返回？还是有任务待执行返回
				// 锁 + 双重判断

				// 循环检查任务队列是否为空。如果任务队列为空，线程会进入等待状态，直到有新的任务到来或线程池关闭。
				while (taskQue_.size() == 0) {
					// 如果线程池要结束，回收线程资源
					if (!isPoolRunning_) {
						threads_.erase(threadid);
						std::cout << "threadid:" << std::this_thread::get_id() << " exit!" << std::endl;
						exitCond_.notify_all();
						return;  // 通过 return 结束线程函数，线程结束
					}

					if (poolMode_ == PoolMode::MODE_CACHED) {
						// cached模式下，等待notEmpty条件，若在1s内没有新的任务到来，还会检查当前线程是否超时
						// cached模式下，可能已经创建了很多的线程（大于initThreadSize_数量的线程），若某线程空闲时间超过60s就应该回收
						if (std::cv_status::timeout == notEmpty_.wait_for(lock, std::chrono::seconds(1))) {
							auto now = std::chrono::high_resolution_clock().now();
							auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
							if (dur.count() >= THREAD_MAX_IDLE_TIME && curThreadSize_ > initThreadSize_) {
								// 开始回收当前线程对象，注意记录线程数量的相关变量的值修改
								// 把线程对象从线程列表容器中删除 原本是没有办法的 threadFunc线程函数对应的是哪一个Thread对象？
								// 为此，提供 threadid 映射 thread对象，即unordered_map以便删除
								threads_.erase(threadid);
								curThreadSize_--;
								idleThreadSize_--;

								std::cout << "threadid:" << std::this_thread::get_id() << " exit!" << std::endl;
								return;
							}
						}
					} 
					else {
						// fixed模式下，一直等待notEmpty条件，直到线程池关闭
						notEmpty_.wait(lock);
					}
				}

				idleThreadSize_--;
				std::cout << "tid:" << std::this_thread::get_id() << "获取任务成功..." << std::endl;

				// 从任务队列中取一个任务出来
				task = taskQue_.front();
				taskQue_.pop();
				taskSize_--;
				// 如果依然有剩余任务，继续通知其它的线程获取/执行任务
				if (taskQue_.size() > 0) {
					notEmpty_.notify_all();
				}

				// 取出一个任务，任务队列不满，通知可以继续提交任务了
				notFull_.notify_all();
			} 
			// 线程取出任务后就应该把锁释放，让其他线程也可以去取任务/或者能提交新的任务（操作任务队列），而不是等到任务执行完才释放锁
			// 为此，造了一个作用域->unique_lock
			// （困难之处）

			// 当前线程负责执行这个任务
			if (task != nullptr) {
				task();
			}
			// 任务执行完毕...
			idleThreadSize_++;
			lastTime = std::chrono::high_resolution_clock().now();
		}
    }

	// 检查pool的运行状态
	bool checkRunningState() const {
		return isPoolRunning_;
	}

private:
	std::unordered_map<int, std::unique_ptr<Thread>> threads_;      // 线程列表
	int initThreadSize_;                 // 初始的线程数量
	int threadSizeThreshHold_;           // 线程数量上限阈值（cached模式）
	std::atomic_int curThreadSize_;	     // 记录当前线程池里面线程的总数量
	std::atomic_int idleThreadSize_;     // 记录当前线程池里面空闲线程的数量


    using Task = std::function<void()>;  // 一个Task任务其实对应的就是一个函数对象
	std::queue<Task> taskQue_;           // 任务队列
	std::atomic_int taskSize_;           // 任务的数量
	int taskQueMaxThreshHold_;           // 任务队列数量上限阈值

	std::mutex taskQueMtx_;              // 保证任务队列的线程安全
	std::condition_variable notFull_;    // 表示任务队列不满（可以生产）
	std::condition_variable notEmpty_;   // 表示任务队列不空（可以消费）
	std::condition_variable exitCond_;   // 等待线程资源全部回收


	PoolMode poolMode_;                  // 当前线程池的工作模式
	std::atomic_bool isPoolRunning_;     // 表示当前线程池的启动状态
};



#endif