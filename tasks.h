#pragma once
#include <coroutine>
#include <stdint.h>
#include <iostream> // only for debug

struct promise; // ffdecl.

struct coroutine : std::coroutine_handle<promise> {
	using promise_type = ::promise;
};

struct scheduled_task {
	// if await_ready is false, then await_suspend will be called
	bool await_ready() noexcept {
		return false;
	};

	void await_suspend( std::coroutine_handle<::promise> h ) noexcept;
	void await_resume() noexcept {};
};

struct finalize_task {
	// if await_ready is false, then await_suspend will be called
	bool await_ready() noexcept {
		return false;
	};

	void await_suspend( std::coroutine_handle<::promise> h ) noexcept;
	void await_resume() noexcept {};
};

struct channel {
	// add channel where we can block until there is something to read
	~channel() {
		std::cout << "deleting channel." << std::endl
		          << std::flush;
	}
};

class scheduler_impl;              // ffdecl, pimpl
struct task_list_o;                // ffdecl
using task_list_t = task_list_o**; // helper

class scheduler_o {
	scheduler_impl* p_impl = nullptr;

	scheduler_o( int32_t num_worker_threads );

	scheduler_o( const scheduler_o& )            = delete;
	scheduler_o( scheduler_o&& )                 = delete; // move constructor
	scheduler_o& operator=( const scheduler_o& ) = delete;
	scheduler_o& operator=( scheduler_o&& )      = delete; // move assignment

  public:
	// create a new task list object
	task_list_o** new_task_list();

	// add new objects to a task list object
	void add_to_task_list( task_list_o** p_t, coroutine c );

	// execute all tasks in the task list, then free the task list object
	void wait_for_task_list( task_list_o** p_t );

	static scheduler_o* create( int32_t num_worker_threads = 0 );

	~scheduler_o();
};

struct promise {
	coroutine get_return_object() {
		return { coroutine::from_promise( *this ) };
	}
	scheduled_task initial_suspend() noexcept {
		return {};
	}
	finalize_task final_suspend() noexcept {
		return {};
	}
	void            return_void(){};
	void            unhandled_exception(){};
	scheduler_impl* scheduler   = nullptr; // owned by scheduler
	task_list_t     p_task_list = nullptr; // owned by scheduler
};
