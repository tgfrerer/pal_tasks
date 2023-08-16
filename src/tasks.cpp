#include "tasks.h"
#include <cassert>
#include <deque>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include "lockfree_ring_buffer.h"
#include "sched.h"

using coroutine_handle_t = std::coroutine_handle<TaskPromise>;

// A channel is a thread-safe primitive to communicate with worker threads -
// each worker thread has exactly one channel. Once a channel contains
// a payload it is blocked (you cannot push anymore handles onto this channel).
// The channel gets free and ready to receive another handle as soon as the
// worker thread has finished processing the current handle.
struct Channel {
	void*            handle = nullptr; // storage for channel payload: one single handle. void means that the channel is free.
	std::atomic_flag flag   = false;   // signal that the current channel is busy.

	bool try_push( coroutine_handle_t& h ) {

		if ( flag.test_and_set() ) {
			// if the current channel was already flagged
			// we cannot add anymore work.
			return false;
		}

		// --------| invariant: current channel is available now

		handle = h.address();

		// If there is a thread blocked on this operation, we
		// unblock it here.
		flag.notify_all();

		return true;
	}

	~Channel() {
		// Once the channel accepts a coroutine handle, it becomes the
		// owner of the handle. If there are any leftover valid handles
		// that we own when this object dips into oblivion, we must clean
		// them up first.
		//
		if ( this->handle ) {
			// std::cout << "WARNING: leftover task in channel." << std::endl;
			// std::cout << "destroying task: " << this->handle << std::endl;
			Task::from_address( this->handle ).destroy();
		}

		this->handle = nullptr;
	}
};

class task_list_o {
	alignas( 64 ) std::atomic_size_t task_count;                // number of tasks, only gets decremented if task has been removed
	void* waiting_task                               = nullptr; // weak: if this tasklist was issued by a coroutine, this is the coroutine to resume if the task list gets completed.
	alignas( 64 ) std::atomic_size_t protect_cleanup = 0;       // whether there is a task still in-flight
	lockfree_ring_buffer_t tasks;

  public:
	task_list_o( uint32_t capacity_hint = 32 ) // start with capacity of 32
	    : tasks( capacity_hint )
	    , task_count( 0 )
	    , protect_cleanup( 0 ) {
	}

	~task_list_o() {
		// If there are any tasks left on the task list, we must destroy them, as we own them.
		void* task;
		while ( ( task = this->tasks.try_pop() ) ) {
			Task::from_address( task ).destroy();
		}
	}

	// Push a suspended task back onto the end of the task list
	inline void push_task( coroutine_handle_t const& c ) {
		tasks.push( c.address() );
	}

	// Get the next task if possible, if there is no next task,
	// return an empty coroutine handle.
	// An empty coroutine handle will compare true to nullptr
	inline coroutine_handle_t pop_task() {
		return Task::from_address( tasks.try_pop() );
	}


	// Add a new task to the task list - only allowed in setup phase,
	// where only one thread has access to the task list.
	void add_task( coroutine_handle_t& c ) {
		c.promise().p_task_list = this;
		tasks.unsafe_initial_dynamic_push( c.address() );
		++task_count;
	}

	void tag_all_tasks_with_scheduler( scheduler_impl* p_scheduler ) {
		tasks.unsafe_for_each(
		    []( void* c, void* p_scheduler ) {
			    coroutine_handle_t::from_address( c ).promise().scheduler = static_cast<scheduler_impl*>( p_scheduler );
		    },
		    p_scheduler );
	}

	/// Return the number of tasks which are both in-flight and waiting
	///
	/// Note this is not the same as tasks.size() as any tasks which are being
	/// processed and are in flight will not show up on the task list.
	///
	/// num_tasks gets decremented only if a task was fully completed
	///
	/// The additional protect_cleanup will be greater than 0
	/// iff decrement_task_count is still in-progress.
	inline size_t get_task_count() {
		return task_count + protect_cleanup;
	}

	/// NOTE: decrementing num_tasks *and* clearing up in
	/// case we're at the last task must happen as an atomic
	/// operation. Otherwise, whoever tests for num_tasks
	/// from the outside and finds 0 may delete our object
	/// from under our feet.
	///
	/// Conversely, we must also make sure that we don't
	/// receive the same reading for num_tasks more than once,
	/// otherwise our cleanup code would get executed more than
	/// once.
	///
	/// To do this without a mutex, we add a flag which protects
	/// our cleanup function
	void decrement_task_count() {

		this->protect_cleanup++; // add guard against deletion - once get_task_count returns 0 users may delete this task_list_o

		if ( 0 == --task_count ) {
			if ( this->waiting_task ) {
				// if this task list was issued from within a coroutine
				// using an await, this will resume the waiting coroutine.
				coroutine_handle_t::from_address( this->waiting_task ).resume();
			}
		}

		this->protect_cleanup--; // remove guard against deletion
	}
	friend struct await_tasks;
};

TaskList::TaskList( uint32_t hint_capacity )
    : p_impl( new task_list_o( hint_capacity ) ) {
}

void TaskList::add_task( Task c ) {
	assert( p_impl != nullptr && "task list must be valid. Was this task list already used?" );
	p_impl->add_task( c );
}

TaskList::~TaskList() {
	// In case that this task list was deleted already, p_impl will
	// be nullptr, which means that this delete operator is a no-op.
	// otherwise (in case a tasklist has not been used and needs to
	// be cleaned up), this will perform the cleanup for us.
	delete p_impl;
}

// ----------------------------------------------------------------------

class scheduler_impl {

	bool move_task_to_worker_thread( coroutine_handle_t& c );

	lockfree_ring_buffer_t async_task_lists{ 256 };

  public:
	std::vector<Channel*>     channels; // non-owning - channels are owned by their threads
	std::vector<std::jthread> threads;

	scheduler_impl( int32_t num_worker_threads = 0 );

	~scheduler_impl() {

		// We must request all threads to stop, as they will shortly be destroyed.
		//
		// We must then nudge any threads which are currently waiting on a flag signal
		// so that they get a chance to notice that they have been stopped - otherwise
		// they will wait to infinity.

		for ( auto& t : threads ) {
			t.request_stop();
		}

		// Nudge sleeping threads by flipping the wait flag explicitly - any sleeping
		// threads so nudged now wake up and get a chance to see that they have been
		// stopped and will exit their inner loop gracefully.
		//
		for ( auto* c : channels ) {
			if ( c ) {
				c->flag.test_and_set(); // Set flag so that if there is a worker blocked on this flag, it may proceed.
				c->flag.notify_all();   // Notify the worker thread (if any worker thread is waiting) that the flag has flipped.
				                        // without notify, waiters will not be notified that the flag has flipped.
			}
		}

		// We must wait until all the threads have been joined.
		// Deleting a jthread object implicitly stops (sets the stop_token) and joins.
		threads.clear();
	}

	// Execute all tasks in the task list, then invalidate the task list object
	void wait_for_task_list( TaskList& p_t );

	// Execute all tasks in the task list, then invalidate the task list object.
	// This version of the function returns an awaitable.
	await_tasks wait_for_task_list_inner( TaskList& p_t );

	friend struct await_tasks;
};

scheduler_impl::scheduler_impl( int32_t num_worker_threads ) {

	if ( num_worker_threads < 0 ) {
		// If negative, then this means that we must
		// count backwards from the number of available hardware threads
		num_worker_threads = std::jthread::hardware_concurrency() + num_worker_threads;
	}

	assert( num_worker_threads >= 0 && "Inferred number of worker threads must not be negative" );

	// std::cout << "Initializing scheduler with " << num_worker_threads << " worker threads" << std::endl;

	// Reserve memory so that we can take addresses for channel,
	// and don't have to worry about iterator validity
	channels.reserve( num_worker_threads );
	threads.reserve( num_worker_threads );

	// NOTE THAT BY DEFAULT WE DON'T HAVE ANY WORKER THREADS
	//
	for ( int i = 0; i != num_worker_threads; i++ ) {
		channels.emplace_back( new Channel() );
		threads.emplace_back(
		    //
		    // Thread worker implementation
		    //
		    []( std::stop_token stop_token, Channel* ch ) {
			    // sleep until flag gets set
			    ch->flag.wait( false );

			    std::cout << "New worker thread on CPU " << sched_getcpu()
			              << std::endl;

			    while ( !stop_token.stop_requested() ) {
				    //				    std::cout << "Executing on cpu: " << sched_getcpu()
				    //				              << std::endl;

				    if ( ch->handle ) {
					    coroutine_handle_t::from_address( ch->handle ).resume(); // resume coroutine
					    ch->handle = nullptr;
					    // signal that we are ready to receive new tasks
					    ch->flag.clear( std::memory_order::release );
					    continue;
				    }

				    // Sleep until flag gets set
				    //
				    // The flag is set on any of the following:
				    //   * A new job has been placed in the channel
				    // 	 * The current task list is empty.
				    ch->flag.wait( false, std::memory_order::acquire );
			    }

			    // Channel is owned by the thread - when the thread falls out of scope
			    // that means that the channel gets deleted, too.
			    delete ch;
		    },
		    channels.back() );
		cpu_set_t cpuset;
		CPU_ZERO( &cpuset );
		CPU_SET( i + 1, &cpuset );
		assert( 0 == pthread_setaffinity_np( threads.back().native_handle(), sizeof( cpuset ), &cpuset ) );
	}
	std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );
	for ( int i = 0; i != num_worker_threads; i++ ) {

		auto& c = channels[ i ];
		c->flag.test_and_set();
		c->flag.notify_one(); // start work on thread
	}
}

// ----------------------------------------------------------------------

await_tasks scheduler_impl::wait_for_task_list_inner( TaskList& tl ) {

	if ( tl.p_impl == nullptr ) {
		assert( false && "Task list must have been freshly initialised. Has this task list been waited for already?" );
		return {};
	}

	// --------| Invariant: TaskList is valid

	// tag all tasks with their scheduler,
	// so that they know where they belong to
	tl.p_impl->tag_all_tasks_with_scheduler( this );

	// create an awaitable that can own a tasklist

	// await_tasks::await_suspend is where control will go next.
	await_tasks result;
	result.p_task_list = tl.p_impl; // ownership transfer
	tl.p_impl          = nullptr;   // since the original object does not own, it must not delete.

	return result;
}

// ----------------------------------------------------------------------

void await_tasks::await_suspend( std::coroutine_handle<TaskPromise> h ) noexcept {

	// the task that waits on a tasklist is now fully suspended.

	// we must take a local variable here as we cannot guarantee to access `this` after
	// the handle gets resumed, which may cause the awaiter object to destroy.
	task_list_o* p_task_list = this->p_task_list;

	if ( 0 == p_task_list->get_task_count() ) {
		// If the child task list is empty, then we can progress immediately.
		h.resume();
		return;
	}

	/// ------ WARNING: DO NOT ACCESS `THIS` ANYMORE AS IT MAY BE DESTROYED
	/// ------ AFTER THE HANDLE HAS COMPLETED RESUME

	h.promise().child_task_list               = p_task_list;
	h.promise().child_task_list->waiting_task = h.address(); // tell the task list that somebody is awaiting it.

	// At this point, we must distribute the elements fron this task list onto the
	// scheduler. otherwise there is no chance for forward progress.

	// we do this by using a blocking call to the scheduler, where the
	// scheduler takes control of this task list and will add it to its current
	// run of tasklists to process.

	h.promise().scheduler->async_task_lists.push( p_task_list ); // take ownership of task_list into scheduler

	                                                             // this will drop us back to where resume was called()
}
// ----------------------------------------------------------------------

void scheduler_impl::wait_for_task_list( TaskList& tl ) {

	if ( tl.p_impl == nullptr ) {
		assert( false && "Task list must have been freshly initialised. Has this task list been waited for already?" );
		return;
	}

	// --------| Invariant: TaskList is valid

	// Execute tasks in this task list until there are no more tasks left
	// to execute.

	// Before we start executing tasks we must take ownership of them
	// by tagging them so that they know which scheduler they belong to.
	tl.p_impl->tag_all_tasks_with_scheduler( this );

	// Distribute work, as long as there is work to distribute

	std::vector<task_list_o*> task_stack;
	task_stack.push_back( tl.p_impl );

	while ( !task_stack.empty() ) {

		task_list_o* task_list = task_stack.back();
		task_stack.pop_back();

		while ( task_list->get_task_count() ) {

			// ----------| Invariant: There are Tasks in this Task List which have not yet completed

			coroutine_handle_t c = task_list->pop_task();

			if ( c == nullptr ) {

				// we could not fetch anything from our main task list - but
				// perhaps there is a tasklist from a sub-task where there
				// are still some tasks that need some work...
				// let's see if there is an extra task list that we may access

				// get a task list that has been placed onto the scheduler

				// if we can steal a task list from the scheduler, then we make this our new task list.
				task_list_o* async_tl = static_cast<task_list_o*>( this->async_task_lists.try_pop() );

				if ( async_tl ) {

					task_stack.push_back( task_list );
					task_list = async_tl;
					// we have changed the task list, let's try if we
					// can fetch some work from this task list.
					continue;
				}
			}

			if ( c == nullptr ) {

				// We could not fetch a task from the task list - this means
				// that there are tasks in-progress that we must wait for.


				continue;
			}

			// ----------| Invariant: current coroutine is valid

			// Find a free channel. If there is, then place this handle in the channel,
			// which means that it will be executed on the worker thread associated
			// with this channel.

			if ( move_task_to_worker_thread( c ) ) {
				// Pushing consumes the coroutine handle - that is, it becomes owned by the channel
				// who owns it for the worker thread.
				//
				// If we made it in here, the handle was successfully offloaded to a worker thread.
				//
				// The worker thread must now execute the payload, and the task will decrement the
				// counter for the current TaskList upon completion.
				continue;
			}

			// --------| Invariant: All worker threads are busy - or there are no worker threads: we must execute on this thread
			c();
		}

		delete task_list;

		// pop last element off task list

		// and then the task list
		//
		// Once all tasks have been complete, release task list
	}

	tl.p_impl = nullptr; // Signal to any future users that this task list has been used already
}

// ----------------------------------------------------------------------

inline bool scheduler_impl::move_task_to_worker_thread( coroutine_handle_t& c ) {
	// Iterate over all channels. If we can place the coroutine
	// on a channel, do so.
	//
	// FIXME: this pays no respect to which thread originally processed the
	// handle - ideally we would prefer to resume a coroutine on the same thread
	// as the one on which it was suspended, as we would expect a better chance
	// of caches still being hot on this thread.
	//
	// it would also be better if we would only touch our threads when they are idle.
	// perhaps it would be better if the threads would pick up work, and not the scheduler
	// pushed the work onto the threads?
	//
	for ( auto& ch : channels ) {
		if ( true == ch->try_push( c ) ) {
			return true;
		}
	}
	return false;
}

// ----------------------------------------------------------------------

Scheduler::Scheduler( int32_t num_worker_threads )
    : p_impl( new scheduler_impl( num_worker_threads ) ) {
}

void Scheduler::wait_for_task_list( TaskList& p_t ) {
	p_impl->wait_for_task_list( p_t );
}

await_tasks Scheduler::wait_for_task_list_inner( TaskList& p_t ) {
	return p_impl->wait_for_task_list_inner( p_t );
}

Scheduler* Scheduler::create( int32_t num_worker_threads ) {
	return new Scheduler( num_worker_threads );
}

Scheduler::~Scheduler() {
	delete p_impl;
}

// ----------------------------------------------------------------------

void suspend_task::await_suspend( std::coroutine_handle<TaskPromise> h ) noexcept {

	// ----------| Invariant: At this point the coroutine pointed to by h
	// has been fully suspended. This is guaranteed by the c++ standard.

	auto& promise = h.promise();

	task_list_o* task_list = promise.p_task_list;

	// Put the current coroutine to the back of the scheduler queue
	// as it has been fully suspended at this point.

	task_list->push_task( promise.get_return_object() );

	{
		// --- Eager Workers ---
		//
		// Eagerly try to fetch & execute the next task from the front of the
		// scheduler queue -
		// We do this so that multiple threads can share the
		// scheduling workload.
		//
		// But we can also disable that, so that there is only one thread
		// that does the scheduling, and removing elements from the
		// queue.

		coroutine_handle_t c = task_list->pop_task();

		if ( c ) {
			assert( !c.done() && "task must not be done" );
			c();
		}
	}

	// Note: Once we drop off here, control will return to where the resume()
	// command that brought us here was issued.
}

// ----------------------------------------------------------------------

void finalize_task::await_suspend( std::coroutine_handle<TaskPromise> h ) noexcept {
	// This is the last time that this coroutine will be awakened
	// we do not suspend it anymore after this
	h.promise().p_task_list->decrement_task_count();
	// ---
	h.destroy();
}
