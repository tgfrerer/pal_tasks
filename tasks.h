#pragma once
#include <coroutine>
#include <stdint.h>

struct promise; // ffdecl.

struct task : std::coroutine_handle<promise> {
	using promise_type = ::promise;
};

struct schedule_task {
	// if await_ready is false, then await_suspend will be called
	bool await_ready() noexcept { return false; };
	void await_suspend( std::coroutine_handle<::promise> h ) noexcept;
	void await_resume() noexcept {};
};

struct finalize_task {
	// if await_ready is false, then await_suspend will be called
	bool await_ready() noexcept { return false; };
	void await_suspend( std::coroutine_handle<::promise> h ) noexcept;
	void await_resume() noexcept {};
};


class scheduler_impl;              // ffdecl, pimpl
struct task_list_o;                // ffdecl
class task_list_t;                 // ffdecl

class scheduler_o {
	scheduler_impl* p_impl = nullptr;

	scheduler_o( int32_t num_worker_threads );

	scheduler_o( const scheduler_o& )            = delete;
	scheduler_o( scheduler_o&& )                 = delete; // move constructor
	scheduler_o& operator=( const scheduler_o& ) = delete;
	scheduler_o& operator=( scheduler_o&& )      = delete; // move assignment

  public:
	// create a new task list object
	task_list_t new_task_list();

	// add coroutines to a task list object

	// execute all tasks in the task list, then free the task list object
	// this takes possession of the task list object.
	void wait_for_task_list( task_list_t& p_t );

	static scheduler_o* create( int32_t num_worker_threads = 0 );

	~scheduler_o();
};

class task_list_t {

	task_list_o* p_impl; // owning

	task_list_t( const task_list_t& )            = delete;
	task_list_t( task_list_t&& )                 = delete; // move constructor
	task_list_t& operator=( const task_list_t& ) = delete;
	task_list_t& operator=( task_list_t&& )      = delete; // move assignment

  public:
	task_list_t( uint32_t hint_capacity = 1 ); // default constructor

	~task_list_t();

	void add_task( task c );

	friend class scheduler_impl;
};

struct promise {
	task            get_return_object() { return { task::from_promise( *this ) }; }
	schedule_task   initial_suspend() noexcept { return {}; }
	finalize_task   final_suspend() noexcept { return {}; }
	void            return_void(){};
	void            unhandled_exception(){};
	scheduler_impl* scheduler   = nullptr; // owned by scheduler
	task_list_o*    p_task_list = nullptr; // owned by scheduler
};
