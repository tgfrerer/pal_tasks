#pragma once
#include <coroutine>
#include <stdint.h>

struct TaskPromise; // ffdecl.
class scheduler_impl; // ffdecl, pimpl
class task_list_o;    // ffdecl
class TaskList;       // ffdecl

struct Task : std::coroutine_handle<TaskPromise> {
	using promise_type = ::TaskPromise;
};

struct suspend_task {
	// if await_ready is false, then await_suspend will be called
	constexpr bool await_ready() noexcept {
		return false;
	};
	void await_suspend( std::coroutine_handle<TaskPromise> h ) noexcept;
	void await_resume() noexcept {};
};

struct await_tasks {
	// if await_ready is false, then await_suspend will be called
	constexpr bool await_ready() noexcept {
		return false;
	};
	void await_suspend( std::coroutine_handle<TaskPromise> h ) noexcept;
	void await_resume() noexcept {};

	task_list_o* p_task_list; // owning: careful: don't access after await_suspend has been called
};

struct finalize_task {
	// if await_ready is false, then await_suspend will be called
	constexpr bool await_ready() noexcept {
		return false;
	};
	void await_suspend( std::coroutine_handle<TaskPromise> h ) noexcept;
	void await_resume() noexcept {};
};



class Scheduler {
	scheduler_impl* p_impl = nullptr;

	Scheduler( int32_t num_worker_threads );

	Scheduler( const Scheduler& )            = delete;
	Scheduler( Scheduler&& )                 = delete; // move constructor
	Scheduler& operator=( const Scheduler& ) = delete;
	Scheduler& operator=( Scheduler&& )      = delete; // move assignment

  public:
	// Execute all tasks in the task list, then free the task list object.
	// This takes possession of the task list object, and acts as if it was
	// a blocking call.
	//
	// Once this call returns, the TaskList that was given as a parameter
	// has been consumed, and you should not re-use it.
	void wait_for_task_list( TaskList& p_t );

	// Execute all tasks in task list, then resume the current task, if
	// it is a coroutine. This call can be awaited.
	//
	// Takes posession of task list object.
	await_tasks wait_for_task_list_inner( TaskList& p_t );

	// Create a scheduler with as many hardware threads as possible
	//  0 ... No worker threads, just one main thread
	//  n ... n number of worker threads
	// -1 ... As many worker threads as cpus, -1
	static Scheduler* create( int32_t num_worker_threads = 0 );

	~Scheduler();
};

class TaskList {

	task_list_o* p_impl; // owning

	TaskList( const TaskList& )            = delete;
	TaskList( TaskList&& )                 = delete; // move constructor
	TaskList& operator=( const TaskList& ) = delete;
	TaskList& operator=( TaskList&& )      = delete; // move assignment

  public:
	TaskList( uint32_t hint_capacity = 1 ); // default constructor

	~TaskList();

	void add_task( Task c );

	friend class scheduler_impl;
};

struct TaskPromise {
	Task get_return_object() {
		return { Task::from_promise( *this ) };
	}
	std::suspend_always initial_suspend() noexcept {
		return {};
	}
	finalize_task   final_suspend() noexcept { return {}; }
	void            return_void(){};
	void            unhandled_exception(){};
	scheduler_impl* scheduler       = nullptr; // weak: owned by scheduler
	task_list_o*    p_task_list     = nullptr; // weak: owned by scheduler
	task_list_o*    child_task_list = nullptr; // weak: the task list we are possibly waiting upon
};
