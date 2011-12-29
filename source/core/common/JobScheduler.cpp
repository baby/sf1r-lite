#include "JobScheduler.h"

namespace sf1r
{

JobScheduler::JobScheduler()
{
    asynchronousWorker_ = boost::thread(
        &JobScheduler::runAsynchronousTasks_, this
    );
}

JobScheduler::~JobScheduler()
{
    close();
}

void JobScheduler::close()
{
    asynchronousWorker_.interrupt();
    asynchronousWorker_.join();
}

void JobScheduler::addTask(task_type task)
{
    asynchronousTasks_.push(task);
}

void JobScheduler::runAsynchronousTasks_()
{
    try
    {
        task_type task;
        while (true)
        {
            asynchronousTasks_.pop(task);
            task();

            // terminate execution if interrupted
            boost::this_thread::interruption_point();
        }
    }
    catch (boost::thread_interrupted&)
    {
        return;
    }
}

}
