#include "DriverLogServer.h"
#include "DriverLogServerController.h"
#include "LogDispatchHandler.h"
#include "LogServerCfg.h"
#include "LogServerStorage.h"

#include <util/driver/DriverConnectionFirewall.h>

using namespace izenelib::driver;

namespace sf1r
{

DriverLogServer::DriverLogServer(uint16_t port, uint32_t threadNum)
    : port_(port)
    , threadNum_(threadNum)
    , bStarted_(false)
{
}

DriverLogServer::~DriverLogServer()
{
    std::cout << "~DriverLogServer()" << std::endl;
    stop();
}

bool DriverLogServer::init()
{
    boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::tcp::v4(), port_);
    router_.reset(new ::izenelib::driver::Router);

    if (router_ && initRouter())
    {
        boost::shared_ptr<DriverConnectionFactory> factory(new DriverConnectionFactory(router_));
        factory->setFirewall(DriverConnectionFirewall());

        driverServer_.reset(new DriverServer(endpoint, factory, threadNum_));

        return driverServer_;
    }

    return false;
}

void DriverLogServer::start()
{
    if (!bStarted_)
    {
        bStarted_ = true;
        driverThread_.reset(new boost::thread(&DriverServer::run, driverServer_.get()));
    }
}

void DriverLogServer::join()
{
    driverThread_->join();
}

void DriverLogServer::stop()
{
    driverServer_->stop();
    driverThread_->interrupt();
    bStarted_ = false;
}

bool DriverLogServer::initRouter()
{
    typedef ::izenelib::driver::ActionHandler<DriverLogServerController> handler_t;

    boost::shared_ptr<LogDispatchHandler> logServerhandler(new LogDispatchHandler);
    logServerhandler->Init();
    DriverLogServerController logServerCtrl(logServerhandler);

    handler_t* cclogHandler = new handler_t(logServerCtrl, &DriverLogServerController::update_cclog);
    router_->map("log_server", "update_cclog", cclogHandler);

    handler_t* cclogBackupRawHandler = new handler_t(logServerCtrl, &DriverLogServerController::backup_raw_cclog);
    router_->map("log_server", "backup_raw_cclog", cclogBackupRawHandler);

    handler_t* cclogConvertRawHandler = new handler_t(logServerCtrl, &DriverLogServerController::convert_raw_cclog);
    router_->map("log_server", "convert_raw_cclog", cclogConvertRawHandler);

    handler_t* scdHandler = new handler_t(logServerCtrl, &DriverLogServerController::update_scd);
    router_->map("log_server", "update_scd", scdHandler);

    handler_t* docsHandler = new handler_t(logServerCtrl, &DriverLogServerController::update_documents);
    router_->map("log_server", "update_documents", docsHandler);

    handler_t* flushHandler = new handler_t(logServerCtrl, &DriverLogServerController::flush);
    router_->map("log_server", "flush", flushHandler);

    return true;
}

}
