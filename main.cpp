#include "src/tasks.h"
#include <iostream>

#include <thread>

int main() {

	// Create a scheduler with as many hardware threads as possible
	//  0 ... No worker threads, just one main thread
	//  n ... n number of worker threads
	// -1 ... As many worker threads as cpus, -1
	Scheduler* scheduler = Scheduler::create( -1 );

	if ( false ) {

		TaskList tasks{};
		auto     task_generator = []( int i ) -> Task {
            std::cout << "doing some work: " << i++ << std::endl;

            // put this coroutine back on the scheduler
            co_await defer_task();

            // we have resumed this coroutine from the scheduler
            std::cout << "resuming work: " << i++ << std::endl;

            // complete work, signal to the compiler that this is a
            // coroutine for political reasons.
            co_return;
		};

		for ( int i = 0; i != 5; i++ ) {
			tasks.add_task( task_generator( i ) );
		}

		// add many more tasks

		// Execute all tasks we find on the task list
		scheduler->wait_for_task_list( tasks );
	}
	//

	// ----------------------------------------------------------------------

	srand( 0xdeadbeef );

	std::cout << "MAIN thread is: " << std::hex << std::this_thread::get_id() << std::endl;

	if ( true ) {
		/*
		 * This test is for whether we can issue tasks from within our
		 * current task system.
		 *
		 */

		TaskList another_task_list{};
		auto     coro_generator = []( int i, Scheduler* sched ) -> Task {
            std::cout << "first level coroutine: " << std::dec << i++ << " on thread: " << std::hex << std::this_thread::get_id() << std::endl
                      << std::flush;

            std::this_thread::sleep_for( std::chrono::microseconds( rand() % 55000 ) );

            TaskList inner_task_list{};

            auto inner_coro_generator = []( int i, int j ) -> Task {
                std::cout << "\t executing inner coroutine: " << std::dec << i << ":" << j++ << " on thread: " << std::hex << std::this_thread::get_id() << std::endl
                          << std::flush;

                std::this_thread::sleep_for( std::chrono::microseconds( rand() % 40000 ) );
                // this yields control back to the await_suspend method, and to our scheduler
                co_await defer_task();

                std::this_thread::sleep_for( std::chrono::microseconds( rand() % 33000 ) );
                std::cout << "\t executing inner coroutine: " << std::dec << i << ":" << j++ << " on thread: " << std::hex << std::this_thread::get_id() << std::endl;
                co_return;
            };

            uint32_t num_tasks = rand() % 30;

            for ( int j = 0; j != num_tasks; j++ ) {
                inner_task_list.add_task( inner_coro_generator( i, j * 10 ) );
            }

            std::this_thread::sleep_for( std::chrono::nanoseconds( rand() % 40000000 ) );

            co_await defer_task();
            // this yields control back to the await_suspend method, and to our scheduler

            std::cout << "executing first level coroutine: " << std::dec << i++ << " on thread: " << std::hex << std::this_thread::get_id() << std::endl;
            sched->wait_for_task_list( inner_task_list );

            std::cout << "finished first level coroutine: " << std::dec << i << " on thread: " << std::hex << std::this_thread::get_id() << std::endl;
            co_return;
		};

		for ( int i = 0; i != 20; i++ ) {
			another_task_list.add_task( coro_generator( i * 10, scheduler ) );
		}

		std::cout << "main program starts wait for task list." << std::endl
		          << std::flush;

		scheduler->wait_for_task_list( another_task_list );
	}

	std::cout << "Back with main program." << std::endl
	          << std::flush;

	delete scheduler;

	return 0;
}
