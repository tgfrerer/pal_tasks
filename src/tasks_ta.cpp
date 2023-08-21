#include "tasks.h"
#include <cassert>
#include <deque>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include "lockfree_ring_buffer.h"
#include "sched.h"
#include <mutex>

using coroutine_handle_t = std::coroutine_handle<TaskPromise>;

/* *
 *
 *
 * How do we want this to work?
 *
 * currently, tasklist is the main element that owns our tasks.
 * we need to move all tasks to channels, and it is the channel
 * that then needs to make sure that tasks get completed.
 *
 * Each channel needs a queue for tasks, and new work needs to
 * be added to the queue for each task. we should probably have
 * some sort of priority system.
 *
 * we need to make sure that channels can't deadlock each other.
 * How could a deadlock happen?
 *
 *
 *
 *
 *
 * */

// each worker thread has exactly one channel. Once a channel contains
// a payload it is blocked (you cannot push anymore handles onto this channel).
// The channel gets free and ready to receive another handle as soon as the
// worker thread has finished processing the current handle.
struct Channel {

	// we want to be able to add to the current channel whenever we want
	// whatever we add to the current channel becomes owned by the current channel -
	// everything that we add is a move operation

	// multiple threads may add to the channel, only one thread (the channel's thread itself)
	// will ever remove from the channel - unless we allow for work-stealing.

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
	lockfree_ring_buffer_t tasks;

  public:
	task_list_o* next;                         // intrusive list
	task_list_o* prev;                         // intrusive list

	task_list_o( uint32_t capacity_hint = 32 ) // start with capacity of 32
	    : tasks( capacity_hint )
	    , task_count( 0 ) {
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

	inline size_t get_task_count() {
		return task_count;
	}

	void decrement_task_count() {
		--task_count;
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

	std::mutex   protect_task_list_list;
	task_list_o* task_lists_front = nullptr; // linked list of tasks
	task_list_o* task_lists_back  = nullptr; // last element in linked list of tasks

	/// atomic operation
	bool insert_task_list_at_end( task_list_o* tl );
	bool remove_task_list( task_list_o* tl );

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
	// await_tasks wait_for_task_list_inner( TaskList& p_t );

	friend struct await_tasks;
};

// we want one channel per thread -
// and we want to have one queue per thread. each task that gets suspended suspends to the queue of the thread on which it
// is currently executing, so that it will be resumed on the same thread as the thread on which it was suspended.
// we do this so that we can maintain thread-affinity, and don't have to move memory with tasks from thread to thread;

bool scheduler_impl::insert_task_list_at_end( task_list_o* tl ) {
	std::scoped_lock lock( this->protect_task_list_list );

	if ( task_lists_front == nullptr && task_lists_back == nullptr ) {
		// inserting first element
		task_lists_front = task_lists_back = tl;
		tl->prev                           = nullptr;
		tl->next                           = nullptr;
		return true;
	} else {
		tl->prev        = task_lists_back;
		tl->prev->next  = tl;
		task_lists_back = tl;
		tl->next        = nullptr;
		return true;
	}
}

// removes a task_list_o from linked list of task lists owned by the scheduler,
// if it can be found.
// you still have to deallocate the object in question.
bool scheduler_impl::remove_task_list( task_list_o* tl ) {
	std::scoped_lock lock( this->protect_task_list_list );

	// 1 find element which matches tl
	// if found, remove element.
	task_list_o* el = task_lists_front;

	while ( el ) {
		if ( el == tl ) {
			break;
		}
		el = el->next;
	}

	if ( el ) {

		// --------- | Invariant: we have found the element in question

		if ( el->prev ) {
			el->prev->next = el->next;
		} else {
			// we are deleting the first element
			task_lists_front = el->next;
		}

		if ( el->next ) {
			el->next->prev = el->prev;
		} else {
			// we are deleting the last element
			task_lists_back = el->prev;
		}

		return true;

	} else {
		// we have not found the element in question
		return false;
	}
}

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
	cpu_set_t cpuset;
	for ( int i = 0; i != num_worker_threads; i++ ) {
		channels.emplace_back( new Channel() );
		threads.emplace_back(
		    //
		    // Thread worker implementation
		    //
		    []( std::stop_token stop_token, Channel* ch ) {
			    // sleep until flag gets set
			    ch->flag.wait( false );

			    // std::cout << "New worker thread on CPU " << sched_getcpu()
			    //           << std::endl;

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

		CPU_ZERO( &cpuset );
		CPU_SET( i + 1, &cpuset );
		assert( 0 == pthread_setaffinity_np( threads.back().native_handle(), sizeof( cpuset ), &cpuset ) );
	}
}

// ----------------------------------------------------------------------

void await_tasks::await_suspend( std::coroutine_handle<TaskPromise> h ) noexcept {
	// not implemented yet
}
// ----------------------------------------------------------------------

void scheduler_impl::wait_for_task_list( TaskList& tl ) {

	task_list_o* task_list = tl.p_impl;
	tl.p_impl              = nullptr; // we take possession of this task list

	if ( task_list == nullptr ) {
		assert( false && "Task list must have been freshly initialised. Has this task list been waited for already?" );
		return;
	}

	// --------| Invariant: TaskList is valid

	// Before we start executing tasks we must take ownership of them
	// by tagging them so that they know which scheduler they belong to.
	task_list->tag_all_tasks_with_scheduler( this );

	// the scheduler needs a large queue onto which we can place our workloads -
	// we don't have something like this right now.

	/**
	 *
	 * What would the scheduler queue need to do for us? atomic insertion and deletion
	 *
	 * FIFO - things need to be consumed first in first out
	 *
	 * We would need this to be a queue and it would need to be able to re-allocate
	 *
	 * Channels pull from this queue, channels don't push onto this queue.
	 * only schedulers push onto this queue.
	 *
	 * we can use the TaskList as the source of tasks - they stay in there until pulled
	 * into channels / workers. How do we know that a channel still has some elements that
	 * can be pulled?
	 *
	 * We can keep a linked list of channels in the scheduler - the scheduler owns all task lists
	 * that are held by the linked list.

	 */

	// add current task list to the list of task lists that
	// are owned by this scheduler.
	this->insert_task_list_at_end( task_list );

	// TODO: put this taks list onto the list of task lists that this scheduler
	// keeps as sources for tasks

	// Distribute work, as long as there is work to distribute

	while ( task_list->get_task_count() ) {

		// ----------| Invariant: There are Tasks in this Task List which have not yet completed

		// TODO: consume all tasks into a queue of tasks that this scheduler must process.

		coroutine_handle_t t = task_list->pop_task();

		// --------| Invariant: All worker threads are busy - or there are no worker threads: we must execute on this thread
		if ( t ) {
			t();
		}
	}

	this->remove_task_list( task_list );
	// Once all tasks have been complete, release task list
	delete task_list;
}

// ----------------------------------------------------------------------

Scheduler::Scheduler( int32_t num_worker_threads )
    : p_impl( new scheduler_impl( num_worker_threads ) ) {
}

void Scheduler::wait_for_task_list( TaskList& p_t ) {
	p_impl->wait_for_task_list( p_t );
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
