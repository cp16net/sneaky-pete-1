#ifndef __NOVA_GUEST_AGENT
#define __NOVA_GUEST_AGENT

#include "nova/utils/Curl.h"
#include "nova/guest/guest.h"
#include "nova/flags.h"
#include <boost/foreach.hpp>
#include "nova/guest/GuestException.h"
#include "nova/guest/HeartBeat.h"
#include <boost/thread.hpp>
#include "nova/rpc/receiver.h"
#include <boost/tuple/tuple.hpp>
#include "nova/Log.h"
#include "nova/utils/threads.h"
#include "nova/guest/utils.h"

/* In release mode, all errors should be caught so the guest will not die.
 * If you want to see the bugs in the debug mode, you can fiddle with it here.
 */
// #ifndef _DEBUG
#define NOVA_GUEST_AGENT_START_THREAD_TASK() try {
#define NOVA_GUEST_AGENT_END_THREAD_TASK(name)  \
     } catch(const std::exception & e) { \
        NOVA_LOG_ERROR("Error in " name "! : %s", e.what()); \
     } catch(...) { \
        NOVA_LOG_ERROR("Error in " name "! Exception type unknown."); \
     }



namespace nova { namespace guest { namespace agent {


/**
 * Sends updates back to Trove regarding the status of both
 * Sneaky Pete and the running application.
 */
template<typename AppStatusPtr>
class PeriodicTasker : public nova::utils::Thread::Runner {

private:
    HeartBeat & heart_beat;
    const unsigned long periodic_interval;
    const unsigned long report_interval;
    AppStatusPtr status_updater;

public:
    PeriodicTasker(HeartBeat & heart_beat, AppStatusPtr status_updater,
                   unsigned long periodic_interval,
                   unsigned long report_interval)
      : heart_beat(heart_beat),
        periodic_interval(periodic_interval),
        report_interval(report_interval),
        status_updater(status_updater)
    {
    }

    virtual void operator()() {
        unsigned long next_periodic_task = periodic_interval;
        unsigned long next_reporting = report_interval;
        while(true) {
            unsigned long wait_time = next_periodic_task < next_reporting ?
                next_periodic_task : next_reporting;
            NOVA_LOG_TRACE("Waiting for %lu seconds...", wait_time);
            boost::posix_time::seconds time(wait_time);
            boost::this_thread::sleep(time);
            next_periodic_task -= wait_time;
            next_reporting -= wait_time;

            if (next_periodic_task <= 0) {
                next_periodic_task += periodic_interval;
                periodic_tasks();
            }
            if (next_reporting <= 0) {
                next_reporting += report_interval;
                report_state();
            }
        }
    }

    void periodic_tasks() {
        NOVA_GUEST_AGENT_START_THREAD_TASK();
            NOVA_LOG_TRACE("Running periodic tasks...");
            (*status_updater)();
            Log::rotate_logs_if_needed();
        NOVA_GUEST_AGENT_END_THREAD_TASK("periodic_tasks()");
    }

    void report_state() {
        NOVA_GUEST_AGENT_START_THREAD_TASK();
            this->heart_beat.update();
        NOVA_GUEST_AGENT_END_THREAD_TASK("report_state()");
    }
};

nova::LogOptions log_options_from_flags(const nova::flags::FlagValues & flags);

void run_json_method(std::vector<MessageHandlerPtr> & handlers,
                     const char * msg);

GuestOutput run_method(std::vector<MessageHandlerPtr> & handlers,
                       GuestInput & input);

void message_loop(nova::rpc::ResilentReceiver & receiver,
                  std::vector<MessageHandlerPtr> & handlers);


template<typename initialize_handlers_func, typename AppStatusPtr>
void initialize_and_run(const char * const title,
                        nova::flags::FlagValues & flags) {
    // This should evaluate to a single const char string.
    const char * const info = ("v" NOVA_GUEST_VERSION " " __DATE__ " " __TIME__);
    NOVA_LOG_INFO(" ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^");
    std::string title_text = str(boost::format(
                  " ^ ^ ^         %|=29| ^") % title);
    NOVA_LOG_INFO(title_text.c_str());
    NOVA_LOG_INFO(" ^ '  '        ------TROVE-GUEST-AGENT------ ^");
    NOVA_LOG_INFO(" ^ \\`-'/         -------Sneaky--Pete------   ^");
    std::string banner_text = str(boost::format(
                  " ^   |__     %|+31| ^") % info);
    NOVA_LOG_INFO(banner_text.c_str());
    NOVA_LOG_INFO(" ^  /                         starting now...^");
    NOVA_LOG_INFO(" ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^");

    // Initialize MySQL libraries. This should be done before spawning
    // threads.
    nova::db::mysql::MySqlApiScope mysql_api_scope;

    /* Create connection to Nova database. */
    nova::db::mysql::MySqlConnectionWithDefaultDbPtr nova_db(
        new nova::db::mysql::MySqlConnectionWithDefaultDb(
            flags.nova_sql_host(), flags.nova_sql_user(),
            flags.nova_sql_password(),
            flags.nova_sql_database()));

    /* Create the function object, in case other goodies are attached to
     * it (such as CurlScope). */
    initialize_handlers_func initialize_handlers;

    /* Create job runner, but don't start it's thread until later. */
    nova::utils::ThreadBasedJobRunner job_runner;

    /* Create JSON message handlers. */
    std::vector<MessageHandlerPtr> handlers;
    AppStatusPtr status_updater;
    boost::tie(handlers, status_updater) =
        initialize_handlers(flags, nova_db, job_runner);

    /* Set host value. */
    std::string actual_host = nova::guest::utils::get_host_name();
    std::string host = flags.host().get_value_or(actual_host.c_str());

    /* Create AMQP connection. */
    std::string topic = str(boost::format("guestagent.%s") % flags.guest_id());

    /* Create HeartBeat. */
    HeartBeat heart_beat(nova_db, flags.guest_id());

    NOVA_LOG_INFO("Starting status thread...");
    PeriodicTasker<AppStatusPtr> tasker(heart_beat, status_updater,
                          flags.periodic_interval(), flags.report_interval());
    nova::utils::Thread statusThread(flags.status_thread_stack_size(), tasker);

    NOVA_LOG_INFO("Starting job thread...");
    nova::utils::Thread workerThread(flags.worker_thread_stack_size(),
                                     job_runner);

    // If a "message" is specified we just run it and quit. Otherwise,
    // it's Rabbit time.
    boost::optional<const char *> message = flags.message();
    if (message) {
        run_json_method(handlers, message.get());
        // If a SP is being run with a message, it's possible it needs to run
        // in the job runner thread. If so, be sure to wait, or else this
        // thread will shut down the job runner before the request can even
        // begin.
        if (!job_runner.is_idle()) {
            NOVA_LOG_INFO("Sleeping to give the job runner thread a chance.");
            boost::this_thread::sleep(boost::posix_time::seconds(1));
        }
    } else {
        NOVA_LOG_INFO("Before we go further, lets chill out and take a nap.");
        // Before we go any further, lets all just chill out and take a nap.
        // TODO(tim.simpson) In the past a large spike in virtual memory (60
        // MB) was observed when starting the guest. However, sleeping here
        // appears to avert this phenomenon.
        boost::this_thread::sleep(boost::posix_time::seconds(3));

        nova::rpc::ResilentReceiver receiver(flags.rabbit_host(), flags.rabbit_port(),
                    flags.rabbit_userid(), flags.rabbit_password(),
                    flags.rabbit_client_memory(), topic.c_str(),
                    flags.control_exchange(),
                    flags.rabbit_reconnect_wait_time());

        message_loop(receiver, handlers);
    }

    // Gracefully kill the job runner.
    NOVA_LOG_INFO("Shutting down Sneaky Pete. Killing job runner.");
    job_runner.shutdown();
    NOVA_LOG_INFO(" ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^");
    NOVA_LOG_INFO(" ^           ^ ^                             ^");
    NOVA_LOG_INFO(" ^           -  -   < Good bye.)             ^");
    NOVA_LOG_INFO(" ^        /==/`-'\\                           ^");
    NOVA_LOG_INFO(" ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^");
}



template<typename initialize_handlers_func, typename AppStatusPtr>
int execute_main(const char * const title, int & argc, char** & argv)
{
#ifndef _DEBUG
    try {
#endif
        /* Grab flag values. */
        nova::flags::FlagValues flags(
            nova::flags::FlagMap::create_from_args(argc, argv, true));

        /* Initialize logging. */
        nova::LogOptions log_options = log_options_from_flags(flags);
        LogApiScope log_api_scope(log_options);

        #ifndef _DEBUG
            try {
        #endif
            initialize_and_run<initialize_handlers_func, AppStatusPtr>(
                title, flags);
        #ifndef _DEBUG
            } catch (const std::exception & e) {
                NOVA_LOG_ERROR("Error: %s", e.what());
            }
        #endif

#ifndef _DEBUG
    } catch (const std::exception & e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
#endif
    return 0;
}



} } } // end namespace

#endif
