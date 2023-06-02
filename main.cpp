#include "tasks.h"

#include <thread>
//

int main() {
	// we would like a syntax that goes:

	scheduler_o* scheduler = scheduler_o::create( 5 ); // create a scheduler with two hardware worker threads

	std::cout << "MAIN thread is: " << std::hex << std::this_thread::get_id() << std::endl;

	{

		task_list_t task_list{};
		auto        coro_generator = []( int i ) -> task {
            std::cout << "executing coroutine: " << std::dec << i++ << " on thread: " << std::hex << std::this_thread::get_id() << std::endl
                      << std::flush;

            std::this_thread::sleep_for( std::chrono::milliseconds( rand() % 200 ) );
            // this yields control back to the await_suspend method, and to our scheduler
            co_await scheduled_task();

            std::this_thread::sleep_for( std::chrono::milliseconds( rand() % 200 ) );
            std::cout << "executing coroutine: " << std::dec << i++ << " on thread: " << std::hex << std::this_thread::get_id() << std::endl;
            co_return;
		};

		for ( int i = 0; i != 100; i++ ) {
			task_list.add_task( coro_generator( i * 100 ) );
		}

		scheduler->wait_for_task_list( task_list );
	}

	std::cout << "Back with main program." << std::endl
	          << std::flush;
	delete scheduler;

	return 0;
}
