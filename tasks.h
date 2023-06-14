#pragma once
#include <coroutine>
#include <stdint.h>

struct TaskPromise; // ffdecl.

struct Task : std::coroutine_handle<TaskPromise> {
	using promise_type = ::TaskPromise;
};

struct defer_task {
	// if await_ready is false, then await_suspend will be called
	bool await_ready() noexcept { return false; };
	void await_suspend( std::coroutine_handle<::TaskPromise> h ) noexcept;
	void await_resume() noexcept {};
};

struct finalize_task {
	// if await_ready is false, then await_suspend will be called
	bool await_ready() noexcept { return false; };
	void await_suspend( std::coroutine_handle<::TaskPromise> h ) noexcept;
	void await_resume() noexcept {};
};


class scheduler_impl;              // ffdecl, pimpl
struct task_list_o;                // ffdecl
class TaskList;                    // ffdecl

class Scheduler {
	scheduler_impl* p_impl = nullptr;

	Scheduler( int32_t num_worker_threads );

	Scheduler( const Scheduler& )            = delete;
	Scheduler( Scheduler&& )                 = delete; // move constructor
	Scheduler& operator=( const Scheduler& ) = delete;
	Scheduler& operator=( Scheduler&& )      = delete; // move assignment

  public:
	// add coroutines to a task list object

	// execute all tasks in the task list, then free the task list object
	// this takes possession of the task list object.
	void wait_for_task_list( TaskList& p_t );

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
	defer_task      initial_suspend() noexcept {
        return {};
	}
	finalize_task   final_suspend() noexcept { return {}; }
	void            return_void(){};
	void            unhandled_exception(){};
	scheduler_impl* scheduler   = nullptr; // owned by scheduler
	task_list_o*    p_task_list = nullptr; // owned by scheduler
};
