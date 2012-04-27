#include "IndexWorker.h"
#include "SearchWorker.h"
#include "WorkerHelper.h"

#include <index-manager/IndexManager.h>
#include <index-manager/IndexHooker.h>
#include <index-manager/IndexModeSelector.h>
#include <search-manager/SearchManager.h>
#include <mining-manager/MiningManager.h>
#include <document-manager/DocumentManager.h>
#include <la-manager/LAManager.h>
#include <log-manager/ProductCount.h>
#include <log-manager/LogServerRequest.h>
#include <log-manager/LogServerConnection.h>
#include <common/JobScheduler.h>
#include <common/Utilities.h>

// xxx
#include <bundles/index/IndexBundleConfiguration.h>
#include <bundles/mining/MiningTaskService.h>
#include <bundles/recommend/RecommendTaskService.h>

#include <util/profiler/ProfilerGroup.h>

#include <la/util/UStringUtil.h>

#include <glog/logging.h>

#include <boost/filesystem.hpp>
namespace bfs = boost::filesystem;

using izenelib::util::UString;
using namespace izenelib::driver;


namespace sf1r
{

namespace
{
/** the directory for scd file backup */
const char* SCD_BACKUP_DIR = "backup";
const std::string DOCID("DOCID");
const std::string DATE("DATE");
}

IndexWorker::IndexWorker(
        IndexBundleConfiguration* bundleConfig,
        DirectoryRotator& directoryRotator,
        boost::shared_ptr<IndexManager> indexManager)
    : bundleConfig_(bundleConfig)
    , miningTaskService_(NULL)
    , recommendTaskService_(NULL)
    , indexManager_(indexManager)
    , directoryRotator_(directoryRotator)
    , scd_writer_(new ScdWriterController(bundleConfig_->logSCDPath()))
    , collectionId_(1)
    , indexProgress_()
    , checkInsert_(false)
    , numDeletedDocs_(0)
    , numUpdatedDocs_(0)
    , totalSCDSizeSinceLastBackup_(0)
{
    bool hasDateInConfig = false;
    const IndexBundleSchema& indexSchema = bundleConfig_->indexSchema_;
    for (IndexBundleSchema::const_iterator iter = indexSchema.begin(), iterEnd = indexSchema.end();
        iter != iterEnd; ++iter)
    {
        std::string propertyName = iter->getName();
        boost::to_lower(propertyName);
        if (propertyName=="date")
        {
            hasDateInConfig = true;
            dateProperty_ = *iter;
            break;
        }
    }
    if (!hasDateInConfig)
        throw std::runtime_error("Date Property Doesn't exist in config");

    indexStatus_.numDocs_ = indexManager_->numDocs();

    config_tool::buildPropertyAliasMap(bundleConfig_->indexSchema_, propertyAliasMap_);

    ///propertyId starts from 1
    laInputs_.resize(bundleConfig_->indexSchema_.size() + 1);

    for (IndexBundleSchema::const_iterator iter = indexSchema.begin(), iterEnd = indexSchema.end();
        iter != iterEnd; ++iter)
    {
        boost::shared_ptr<LAInput> laInput(new LAInput);
        laInputs_[iter->getPropertyId()] = laInput;
    }

    createPropertyList_();
    scd_writer_->SetFlushLimit(500);
}

IndexWorker::~IndexWorker()
{
    delete scd_writer_;
}

void IndexWorker::index(unsigned int numdoc, bool& result)
{
    task_type task = boost::bind(&IndexWorker::buildCollection, this, numdoc);
    JobScheduler::get()->addTask(task, bundleConfig_->collectionName_);
    result = true;
}

bool IndexWorker::reindex(boost::shared_ptr<DocumentManager>& documentManager)
{
    //task_type task = boost::bind(&IndexWorker::rebuildCollection, this, documentManager);
    //JobScheduler::get()->addTask(task, bundleConfig_->collectionName_);
    rebuildCollection(documentManager);
    return true;
}

bool IndexWorker::buildCollection(unsigned int numdoc)
{
    size_t currTotalSCDSize = getTotalScdSize_();
    ///If current directory is the one rotated from the backup directory,
    ///there should exist some missed SCDs since the last backup time,
    ///so we move those SCDs from backup directory, so that these data
    ///could be recovered through indexing
    recoverSCD_();

    string scdPath = bundleConfig_->indexSCDPath();
    Status::Guard statusGuard(indexStatus_);
    CREATE_PROFILER(buildIndex, "Index:SIAProcess", "Indexer : buildIndex")

    START_PROFILER(buildIndex);

    LOG(INFO) << "start BuildCollection";

    izenelib::util::ClockTimer timer;

    //flush all writing SCDs
    scd_writer_->Flush();

    indexProgress_.reset();

    // fetch scd from log server if necessary
    if (bundleConfig_->logCreatedDoc_)
    {
        LOG(INFO) << "fetching SCD from LogServer...";
        try
        {
            fetchSCDFromLogServer(scdPath);
        }
        catch (const std::exception& e)
        {
            LOG(ERROR) << "LogServer " << e.what();
        }
    }

    ScdParser parser(bundleConfig_->encoding_);

    // saves the name of the scd files in the path
    vector<string> scdList;
    try
    {
        if (!bfs::is_directory(scdPath))
        {
            LOG(ERROR) << "SCD Path does not exist. Path " << scdPath;
            return false;
        }
    }
    catch (bfs::filesystem_error& e)
    {
        LOG(ERROR) << "Error while opening directory " << e.what();
        return false;
    }

    // search the directory for files
    static const bfs::directory_iterator kItrEnd;
    for (bfs::directory_iterator itr(scdPath); itr != kItrEnd; ++itr)
    {
        if (bfs::is_regular_file(itr->status()))
        {
            std::string fileName = itr->path().filename().string();
            if (parser.checkSCDFormat(fileName))
            {
                scdList.push_back(itr->path().string());
                parser.load(scdPath+fileName);
                indexProgress_.totalFileSize_ += parser.getFileSize();
            }
            else
            {
                LOG(WARNING) << "SCD File not valid " << fileName;
            }
        }
    }

    indexProgress_.totalFileNum = scdList.size();

    if (indexProgress_.totalFileNum == 0)
    {
        LOG(WARNING) << "SCD Files do not exist. Path " << scdPath;
        if (miningTaskService_)
        {
            miningTaskService_->DoContinue();
        }
        return false;
    }

    indexProgress_.currentFileIdx = 1;

    //sort scdList
    sort(scdList.begin(), scdList.end(), ScdParser::compareSCD);

    //here, try to set the index mode(default[batch] or realtime)
    //The threshold is set to the scd_file_size/exist_doc_num, if smaller or equal than this threshold then realtime mode will turn on.
    //when the scd file size(M) larger than max_realtime_msize, the default mode will turn on while ignore the threshold above.
    long max_realtime_msize = 50L; //set to 50M
    double threshold = 50.0/500000.0;
    IndexModeSelector index_mode_selector(indexManager_, threshold, max_realtime_msize);
    index_mode_selector.TrySetIndexMode(indexProgress_.totalFileSize_);

    {
    DirectoryGuard dirGuard(directoryRotator_.currentDirectory().get());
    if (!dirGuard)
    {
        LOG(ERROR) << "Index directory is corrupted";
        return false;
    }

    LOG(INFO) << "SCD Files in Path processed in given order. Path " << scdPath;
    vector<string>::iterator scd_it;
    for (scd_it = scdList.begin(); scd_it != scdList.end(); ++scd_it)
        LOG(INFO) << "SCD File " << bfs::path(*scd_it).stem();

    try
    {
        // loops the list of SCD files that belongs to this collection
        long proccessedFileSize = 0;
        for (scd_it = scdList.begin(); scd_it != scdList.end(); scd_it++)
        {
            size_t pos = scd_it ->rfind("/")+1;
            string filename = scd_it ->substr(pos);
            indexProgress_.currentFileName = filename;
            indexProgress_.currentFilePos_ = 0;

            LOG(INFO) << "Processing SCD file. " << bfs::path(*scd_it).stem();

            switch (parser.checkSCDType(*scd_it))
            {
            case INSERT_SCD:
            {
                if (!doBuildCollection_(*scd_it, 1, numdoc))
                {
                    //continue;
                }
                LOG(INFO) << "Indexing Finished";

            }
            break;
            case DELETE_SCD:
            {
                if (documentManager_->getMaxDocId()> 0)
                {
                    doBuildCollection_(*scd_it, 3, 0);
                    LOG(INFO) << "Delete Finished";
                }
                else
                {
                    LOG(WARNING) << "Indexed documents do not exist. File " << bfs::path(*scd_it).stem();
                }
            }
            break;
            case UPDATE_SCD:
            {
                doBuildCollection_(*scd_it, 2, 0);
                LOG(INFO) << "Update Finished";
            }
            break;
            default:
                break;
            }
            parser.load(*scd_it);
            proccessedFileSize += parser.getFileSize();
            indexProgress_.totalFilePos_ = proccessedFileSize;
            indexProgress_.getIndexingStatus(indexStatus_);
            ++indexProgress_.currentFileIdx;
            uint32_t scd_index = indexProgress_.currentFileIdx;
            if(scd_index%50==0)
            {
                std::string report_file_name = "PerformanceIndexResult.SIAProcess-"+boost::lexical_cast<std::string>(scd_index);
                REPORT_PROFILE_TO_FILE(report_file_name.c_str())
            }

        } // end of loop for scd files of a collection

        documentManager_->flush();
        idManager_->flush();
        index_mode_selector.TryCommit();
        //indexManager_->optimizeIndex();
#ifdef __x86_64
        if (bundleConfig_->isTrieWildcard())
        {
            idManager_->startWildcardProcess();
            idManager_->joinWildcardProcess();
        }
#endif

        if (hooker_)
        {
            if(!hooker_->FinishHook())
            {
                std::cout<<"[IndexWorker] Hooker Finish failed."<<std::endl;
                return false;
            }
            std::cout<<"[IndexWorker] Hooker Finished."<<std::endl;
        }

        if (miningTaskService_)
        {
          indexManager_->pauseMerge();
          miningTaskService_->DoMiningCollection();
          indexManager_->resumeMerge();
        }
    }
    catch (std::exception& e)
    {
        LOG(WARNING) << "exception in indexing or mining: " << e.what();
        indexProgress_.getIndexingStatus(indexStatus_);
        indexProgress_.reset();
        return false;
    }
    indexManager_->getIndexReader();

    bfs::path bkDir = bfs::path(scdPath) / SCD_BACKUP_DIR;
    bfs::create_directory(bkDir);
    LOG(INFO) << "moving " << scdList.size() << " SCD files to directory " << bkDir;
    const boost::shared_ptr<Directory>& currentDir = directoryRotator_.currentDirectory();

    for (scd_it = scdList.begin(); scd_it != scdList.end(); ++scd_it)
    {
        try
        {
            bfs::rename(*scd_it, bkDir / bfs::path(*scd_it).filename());
            currentDir->appendSCD(bfs::path(*scd_it).filename().string());
        }
        catch (bfs::filesystem_error& e)
        {
            LOG(WARNING) << "exception in rename file " << *scd_it << ": " << e.what();
        }
    }

    indexProgress_.getIndexingStatus(indexStatus_);
    LOG(INFO) << "Indexing Finished! Documents Indexed: " << documentManager_->getMaxDocId()
              << " Deleted: " << numDeletedDocs_
              << " Updated: " << numUpdatedDocs_;

    //both variables are refreshed
    numDeletedDocs_ = 0;
    numUpdatedDocs_ = 0;

    indexProgress_.reset();

    STOP_PROFILER(buildIndex);

    REPORT_PROFILE_TO_FILE("PerformanceIndexResult.SIAProcess")
    LOG(INFO) << "End BuildCollection: ";
    LOG(INFO) << "time elapsed:" << timer.elapsed() <<"seconds";

    }

    if(requireBackup_(currTotalSCDSize))
    {
        ///When index can support binlog, this step is not necessary
        ///It means when work under realtime mode, the benefits of reduced merging
        ///caused by frequently updating can not be achieved if Backup is required
        index_mode_selector.ForceCommit();
        if (!backup_())
            return false;
        totalSCDSizeSinceLastBackup_ = 0;
    }

    return true;
}

bool IndexWorker::rebuildCollection(boost::shared_ptr<DocumentManager>& documentManager)
{
    LOG(INFO) << "start BuildCollection";

    if (!documentManager)
    {
        LOG(ERROR) << "documentManager is not initialized!";
        return false;
    }

    izenelib::util::ClockTimer timer;

    indexProgress_.reset();

    docid_t oldId = 0;
    docid_t minDocId = 1;
    docid_t maxDocId = documentManager->getMaxDocId();
    docid_t curDocId = 0;
    docid_t insertedCount = 0;
    for (curDocId = minDocId; curDocId <= maxDocId; curDocId++)
    {
        if (documentManager->isDeleted(curDocId))
        {
            //LOG(INFO) << "skip deleted docid: " << curDocId;
            continue;
        }

        Document document;
        documentManager->getDocument(curDocId, document);

        // update docid
        std::string docidName("DOCID");
        izenelib::util::UString docidValueU;
        if (!document.getProperty(docidName, docidValueU))
        {
            //LOG(WARNING) << "skip doc which has no DOCID property: " << curDocId;
            continue;
        }

        docid_t newDocId;
        std::string docid_str;
        docidValueU.convertString(docid_str, izenelib::util::UString::UTF_8);
        if (createInsertDocId_(Utilities::md5ToUint128(docid_str), newDocId))
        {
            //LOG(INFO) << document.getId() << " -> " << newDocId;
            document.setId(newDocId);
        }
        else
        {
            //LOG(WARNING) << "Failed to create new docid for: " << curDocId;
            continue;
        }

        IndexerDocument indexDocument;
        time_t timestamp = -1;
        prepareIndexDocument_(oldId, timestamp, document, indexDocument);

        timestamp = Utilities::createTimeStamp();
        if (!insertDoc_(document, indexDocument, timestamp))
            continue;

        insertedCount++;
        if (insertedCount % 10000 == 0)
        {
            LOG(INFO) << "inserted doc number: " << insertedCount;
        }

        // interrupt when closing the process
        boost::this_thread::interruption_point();
    }
    LOG(INFO) << "inserted doc number: " << insertedCount << ", total: " << maxDocId;
    LOG(INFO) << "Indexing Finished";

    documentManager_->flush();
    idManager_->flush();
    indexManager_->flush();

#ifdef __x86_64
    if (bundleConfig_->isTrieWildcard())
    {
        idManager_->startWildcardProcess();
        idManager_->joinWildcardProcess();
    }
#endif

    if (miningTaskService_)
    {
        indexManager_->pauseMerge();
        miningTaskService_->DoMiningCollection();
        indexManager_->resumeMerge();
    }

    LOG(INFO) << "End BuildCollection: ";
    LOG(INFO) << "time elapsed:" << timer.elapsed() <<"seconds";

    return true;
}

bool IndexWorker::optimizeIndex()
{
    if (!backup_())
        return false;

    DirectoryGuard dirGuard(directoryRotator_.currentDirectory().get());
    if (!dirGuard)
    {
        LOG(ERROR) << "Index directory is corrupted";
        return false;
    }
    indexManager_->optimizeIndex();
    return true;
}

void IndexWorker::doMining_()
{
    if(miningTaskService_)
    {
        std::string cronStr = miningTaskService_->getMiningBundleConfig()->mining_config_.dcmin_param.cron;
        if(cronStr.empty())
        {
            int docLimit = miningTaskService_->getMiningBundleConfig()->mining_config_.dcmin_param.docnum_limit;
            if(docLimit != 0 && (indexManager_->numDocs()) % docLimit == 0)
            {
                miningTaskService_->DoMiningCollection();
            }
        }
    }
}

bool IndexWorker::createDocument(const Value& documentValue)
{
    DirectoryGuard dirGuard(directoryRotator_.currentDirectory().get());
    if (!dirGuard)
    {
        LOG(ERROR) << "Index directory is corrupted";
        return false;
    }
    SCDDoc scddoc;
    value2SCDDoc(documentValue, scddoc);
    scd_writer_->Write(scddoc, INSERT_SCD);

    time_t timestamp = Utilities::createTimeStamp();

    Document document;
    IndexerDocument indexDocument;
    docid_t oldId = 0;
    bool rType = false;
    std::map<std::string, pair<PropertyDataType, izenelib::util::UString> > rTypeFieldValue;
    docid_t id = 0;
    std::string source = "";

    if (!prepareDocument_(scddoc, document, oldId, rType, rTypeFieldValue, source, timestamp, true))
        return false;

    prepareIndexDocument_(oldId, timestamp, document, indexDocument);

    if (rType)
    {
        id = document.getId();
    }

    bool ret =  insertDoc_(document, indexDocument, timestamp);
    if(ret)
    {
        doMining_();
    }
    searchWorker_->reset_cache(rType, id, rTypeFieldValue);

    // to log server
    if (bundleConfig_->logCreatedDoc_)
    {
        try
        {
            logCreatedDocToLogServer(scddoc);
        }
        catch (const std::exception& e)
        {}
    }

    return ret;
}

void IndexWorker::logCreatedDocToLogServer(const SCDDoc& scdDoc)
{
    // prepare request data
    std::string docidStr;
    std::string content;

    std::string propertyValue;
    for (SCDDoc::const_iterator it = scdDoc.begin(); it != scdDoc.end(); it++)
    {
        const std::string& propertyName = it->first;
        it->second.convertString(propertyValue, bundleConfig_->encoding_);
        if (boost::iequals(propertyName,DOCID))
        {
            docidStr = propertyValue;
        }
        else
        {
            content += "<" + propertyName + ">" + propertyValue + "\n";
        }
    }

    CreateScdDocRequest scdDocReq;
    try
    {
        scdDocReq.param_.docid_ = Utilities::md5ToUint128(docidStr);
    }
    catch (const std::exception)
    {
        return;
    }

    scdDocReq.param_.collection_ = bundleConfig_->collectionName_;
    scdDocReq.param_.content_ = "<DOCID>" + docidStr + "\n" + content;
    //std::cout << scdDocReq.param_.content_ << std::endl;

    // request to log server
    LogServerConnection::instance().asynRequest(scdDocReq);
    LogServerConnection::instance().flushRequests();
}

bool IndexWorker::fetchSCDFromLogServer(const std::string& scdPath)
{
    GetScdFileRequest scdFileReq;
    scdFileReq.param_.username_ = bundleConfig_->localHostUsername_;
    scdFileReq.param_.host_ = bundleConfig_->localHostIp_;
    scdFileReq.param_.path_ = boost::filesystem::absolute(scdPath).string();
    scdFileReq.param_.collection_ = bundleConfig_->collectionName_;

    GetScdFileResponseData response;
    LogServerConnection::instance().syncRequest(scdFileReq, response); // timeout?

    if (response.success_)
    {
        std::cout << "Successfully fetched SCD: " << response.scdFileName_ << std::endl;
        return true;
    }
    else
    {
        std::cout << "Failed to fetch SCD  :  " << response.error_ << std::endl;
        return false;
    }
}

bool IndexWorker::updateDocument(const Value& documentValue)
{
    DirectoryGuard dirGuard(directoryRotator_.currentDirectory().get());
    if (!dirGuard)
    {
        LOG(ERROR) << "Index directory is corrupted";
        return false;
    }
    SCDDoc scddoc;
    value2SCDDoc(documentValue, scddoc);
    scd_writer_->Write(scddoc, UPDATE_SCD);

    time_t timestamp = Utilities::createTimeStamp();

    Document document;
    IndexerDocument indexDocument;
    docid_t oldId = 0;
    bool rType = false;
    std::map<std::string, pair<PropertyDataType, izenelib::util::UString> > rTypeFieldValue;
    docid_t id = 0;
    std::string source = "";

    if (!prepareDocument_(scddoc, document, oldId, rType, rTypeFieldValue, source, timestamp, false))
    {
        return false;
    }

    prepareIndexDocument_(oldId, timestamp, document, indexDocument);

    if (rType)
    {
        id = document.getId();
    }

    bool ret = updateDoc_(document, indexDocument, timestamp, rType);
    if(ret)
    {
        doMining_();
    }
    searchWorker_->reset_cache(rType, id, rTypeFieldValue);

    // to log server
    if (bundleConfig_->logCreatedDoc_)
    {
        try
        {
            logCreatedDocToLogServer(scddoc);
        }
        catch (const std::exception& e)
        {}
    }

    return ret;
}

bool IndexWorker::destroyDocument(const Value& documentValue)
{
    DirectoryGuard dirGuard(directoryRotator_.currentDirectory().get());
    if (!dirGuard)
    {
        LOG(ERROR) << "Index directory is corrupted";
        return false;
    }
    SCDDoc scddoc;
    value2SCDDoc(documentValue, scddoc);

    docid_t docid;
    uint128_t num_docid = Utilities::md5ToUint128(asString(documentValue["DOCID"]));

    if( idManager_->getDocIdByDocName(num_docid, docid, false) == false )
        return false;

    scd_writer_->Write(scddoc, DELETE_SCD);
    time_t timestamp = Utilities::createTimeStamp();
    bool ret = deleteDoc_(docid, timestamp);
    if(ret)
    {
        doMining_();
    }

    // delete from log server
    if (bundleConfig_->logCreatedDoc_)
    {
        DeleteScdDocRequest deleteReq;
        try
        {
            deleteReq.param_.docid_ = Utilities::md5ToUint128(asString(documentValue["DOCID"]));
            deleteReq.param_.collection_ = bundleConfig_->collectionName_;
        }
        catch (const std::exception)
        {
        }

        LogServerConnection::instance().asynRequest(deleteReq);
        LogServerConnection::instance().flushRequests();
    }

    return ret;
}

bool IndexWorker::getIndexStatus(Status& status)
{
    indexProgress_.getIndexingStatus(indexStatus_);
    status = indexStatus_;
    return true;
}

uint32_t IndexWorker::getDocNum()
{
    return indexManager_->numDocs();
}

uint32_t IndexWorker::getKeyCount(const std::string& property_name)
{
    return indexManager_->getBTreeIndexer()->count(property_name);
}

boost::shared_ptr<DocumentManager> IndexWorker::getDocumentManager() const
{
    return documentManager_;
}

/// private ////////////////////////////////////////////////////////////////////

void IndexWorker::createPropertyList_()
{
    for (DocumentSchema::const_iterator iter = bundleConfig_->documentSchema_.begin();
         iter != bundleConfig_->documentSchema_.end(); ++iter)
    {
        string propertyName = iter->propertyName_;
        boost::to_lower(propertyName);
        propertyList_.push_back(propertyName);
    }
}

bool IndexWorker::completePartialDocument_(docid_t oldId, Document& doc)
{
    docid_t newId = doc.getId();
    Document oldDoc;
    if (!documentManager_->getDocument(oldId, oldDoc))
    {
        return false;
    }

    oldDoc.copyPropertiesFromDocument(doc);

    doc.swap(oldDoc);
    doc.setId(newId);
    return true;
}

bool IndexWorker::getPropertyValue_(const PropertyValue& value, std::string& valueStr)
{
    try
    {
        izenelib::util::UString sourceFieldValue = get<izenelib::util::UString>(value);
        sourceFieldValue.convertString(valueStr, izenelib::util::UString::UTF_8);
        return true;
    }
    catch (boost::bad_get& e)
    {
        LOG(WARNING) << "exception in get property value: " << e.what();
        return false;
    }
}

bool IndexWorker::doBuildCollection_(
        const std::string& fileName,
        int op,
        uint32_t numdoc)
{
    ScdParser parser(bundleConfig_->encoding_);
    if (!parser.load(fileName))
    {
        LOG(ERROR) << "Could not Load Scd File. File " << fileName;
        return false;
    }

    indexProgress_.currentFileSize_ = parser.getFileSize();
    indexProgress_.currentFilePos_ = 0;
    productSourceCount_.clear();

    // Filename: B-00-YYYYMMDDhhmm-ssuuu-I-C.SCD
    // Timestamp: YYYYMMDDThhmmss,fff
    std::string baseName(basename(fileName.c_str()));
    std::stringstream ss;
    ss << baseName.substr(5, 8);
    ss << "T";
    ss << baseName.substr(13, 4);
    ss << baseName.substr(18, 2);
    ss << ",";
    ss << baseName.substr(20, 3);
    boost::posix_time::ptime pt;
    try
    {
        pt = boost::posix_time::from_iso_string(ss.str());
    }
    catch (const std::exception& ex)
    {}
    time_t timestamp = Utilities::createTimeStamp(pt);
    if (timestamp == -1)
        timestamp = Utilities::createTimeStamp();

    if (op <= 2) // insert or update
    {
        bool isInsert = (op == 1);
        if (!insertOrUpdateSCD_(parser, isInsert, numdoc, timestamp))
            return false;
    }
    else //delete
    {
        if (!deleteSCD_(parser, timestamp))
            return false;
    }

    saveSourceCount_(op);

    return true;
}

bool IndexWorker::insertOrUpdateSCD_(
        ScdParser& parser,
        bool isInsert,
        uint32_t numdoc,
        time_t timestamp)
{
    CREATE_SCOPED_PROFILER (insertOrUpdateSCD, "IndexWorker", "IndexWorker::insertOrUpdateSCD_");

    uint32_t n = 0;
    long lastOffset = 0;
    //for (ScdParser::cached_iterator doc_iter = parser.cbegin(propertyList_);
        //doc_iter != parser.cend(); ++doc_iter, ++n)
    Document document;
    IndexerDocument indexDocument;
        
    for (ScdParser::iterator doc_iter = parser.begin(propertyList_);
        doc_iter != parser.end(); ++doc_iter, ++n)
    {
        if (*doc_iter == NULL)
        {
            LOG(WARNING) << "SCD File not valid.";
            return false;
        }

        indexProgress_.currentFilePos_ += doc_iter.getOffset() - lastOffset;
        indexProgress_.totalFilePos_ += doc_iter.getOffset() - lastOffset;
        lastOffset = doc_iter.getOffset();
        if (0 < numdoc && numdoc <= n)
            break;

        if (n % 1000 == 0)
        {
            indexProgress_.getIndexingStatus(indexStatus_);
            indexStatus_.progress_ = indexProgress_.getTotalPercent();
            indexStatus_.elapsedTime_ = boost::posix_time::seconds((int)indexProgress_.getElapsed());
            indexStatus_.leftTime_ =  boost::posix_time::seconds((int)indexProgress_.getLeft());
        }

        SCDDocPtr doc = (*doc_iter);
        docid_t oldId = 0;
        bool rType = false;
        std::map<std::string, pair<PropertyDataType, izenelib::util::UString> > rTypeFieldValue;
        docid_t id = 0;
        std::string source = "";
        time_t new_timestamp = timestamp;
        document.clear();
        indexDocument.clear();
        if (!prepareDocument_(*doc, document, oldId, rType, rTypeFieldValue, source, new_timestamp, isInsert))
            continue;

        prepareIndexDocument_(oldId, new_timestamp, document, indexDocument);

        if (!source.empty())
        {
            ++productSourceCount_[source];
        }

        if (rType)
        {
            id = document.getId();
        }

        if (isInsert || oldId == 0)
        {
            if (!insertDoc_(document, indexDocument, new_timestamp))
                continue;
        }
        else
        {
            if (!updateDoc_(document, indexDocument, new_timestamp, rType))
                continue;

            ++numUpdatedDocs_;
        }
        searchWorker_->reset_cache(rType, id, rTypeFieldValue);

        // interrupt when closing the process
        boost::this_thread::interruption_point();
    } // end of for loop for all documents

    searchWorker_->reset_all_property_cache();
    return true;
}

bool IndexWorker::createUpdateDocId_(
        const uint128_t& scdDocId,
        bool rType,
        docid_t& oldId,
        docid_t& newId)
{
    bool result = false;

    if (rType)
    {
        if ((result = idManager_->getDocIdByDocName(scdDocId, oldId, false)))
        {
            newId = oldId;
        }
    }
    else
    {
        result = idManager_->updateDocIdByDocName(scdDocId, oldId, newId);
    }

    return result;
}

bool IndexWorker::createInsertDocId_(
        const uint128_t& scdDocId,
        docid_t& newId)
{
    CREATE_SCOPED_PROFILER (proCreateInsertDocId, "IndexWorker", "IndexWorker::createInsertDocId_");
    docid_t docId = 0;

    // already converted
    if (idManager_->getDocIdByDocName(scdDocId, docId))
    {
        if (documentManager_->isDeleted(docId))
        {
            docid_t oldId = 0;
            if (! idManager_->updateDocIdByDocName(scdDocId, oldId, docId))
            {
                //LOG(WARNING) << "doc id " << docId << " should have been converted";
                return false;
            }
        }
        else
        {
            //LOG(WARNING) << "duplicated doc id " << docId << ", it has already been inserted before";
            return false;
        }
    }

    if (docId <= documentManager_->getMaxDocId())
    {
        //LOG(WARNING) << "skip insert doc id " << docId << ", it is less than max DocId";
        return false;
    }

    newId = docId;
    return true;
}

bool IndexWorker::deleteSCD_(ScdParser& parser, time_t timestamp)
{
    std::vector<izenelib::util::UString> rawDocIDList;
    if (!parser.getDocIdList(rawDocIDList))
    {
        LOG(WARNING) << "SCD File not valid.";
        return false;
    }

    //get the docIds for deleting
    std::vector<docid_t> docIdList;
    docIdList.reserve(rawDocIDList.size());
    indexProgress_.currentFileSize_ =rawDocIDList.size();
    indexProgress_.currentFilePos_ = 0;
    for (std::vector<izenelib::util::UString>::iterator iter = rawDocIDList.begin();
        iter != rawDocIDList.end(); ++iter)
    {
        docid_t docId;
        std::string docid_str;
        iter->convertString(docid_str, izenelib::util::UString::UTF_8);
        if (idManager_->getDocIdByDocName(Utilities::md5ToUint128(docid_str), docId, false))
        {
            docIdList.push_back(docId);
        }
        else
        {
            string property;
            iter->convertString(property, bundleConfig_->encoding_);
            //LOG(ERROR) << "Deleted document " << property << " does not exist, skip it";
        }
    }
    std::sort(docIdList.begin(), docIdList.end());

    //process delete document in index manager
    for (std::vector<docid_t>::iterator iter = docIdList.begin(); iter
            != docIdList.end(); ++iter)
    {
        if (numDeletedDocs_ % 1000 == 0)
        {
            indexProgress_.getIndexingStatus(indexStatus_);
            indexStatus_.progress_ = indexProgress_.getTotalPercent();
            indexStatus_.elapsedTime_ = boost::posix_time::seconds((int)indexProgress_.getElapsed());
            indexStatus_.leftTime_ =  boost::posix_time::seconds((int)indexProgress_.getLeft());
        }

        if (!bundleConfig_->productSourceField_.empty())
        {
            PropertyValue value;
            if (documentManager_->getPropertyValue(*iter, bundleConfig_->productSourceField_, value))
            {
                std::string source("");
                if (getPropertyValue_(value, source))
                {
                    ++productSourceCount_[source];
                }
                else
                {
                    return false;
                }
            }
        }

        //marks delete key to true in DB

        if (!deleteDoc_(*iter, timestamp))
        {
            LOG(WARNING) << "Cannot delete removed Document. docid. " << *iter;
            continue;
        }
        ++indexProgress_.currentFilePos_;

    }

    std::map<std::string, pair<PropertyDataType, izenelib::util::UString> > rTypeFieldValue;
    searchWorker_->reset_cache(false, 0, rTypeFieldValue);

    // interrupt when closing the process
    boost::this_thread::interruption_point();

    return true;
}

bool IndexWorker::insertDoc_(Document& document, IndexerDocument& indexDocument, time_t timestamp)
{
    CREATE_PROFILER(proDocumentIndexing, "IndexWorker", "IndexWorker : InsertDocument")
    CREATE_PROFILER(proIndexing, "IndexWorker", "IndexWorker : indexing")

    if (hooker_)
    {
        ///TODO compatibility issue:
        ///timestamp 
        if(-1 != timestamp) timestamp *= 1000000; 
        if (!hooker_->HookInsert(document, indexDocument, timestamp)) return false;
    }
    START_PROFILER(proDocumentIndexing);
    if (documentManager_->insertDocument(document))
    {
        STOP_PROFILER(proDocumentIndexing);

        START_PROFILER(proIndexing);
        indexManager_->insertDocument(indexDocument);
        STOP_PROFILER(proIndexing);
        indexStatus_.numDocs_ = indexManager_->numDocs();
        return true;
    }
    else return false;
}

bool IndexWorker::updateDoc_(
        Document& document,
        IndexerDocument& indexDocument,
        time_t timestamp,
        bool rType)
{
    CREATE_SCOPED_PROFILER (proDocumentUpdating, "IndexWorker", "IndexWorker::UpdateDocument");

    if (hooker_)
    {
        if (!hooker_->HookUpdate(document, indexDocument, timestamp, rType)) return false;
    }
    if (rType)
    {
        // Store the old property value.
        IndexerDocument oldIndexDocument;
        if (!preparePartialDocument_(document, oldIndexDocument))
            return false;

        // Update document data in the SDB repository.
        if (!documentManager_->updatePartialDocument(document))
        {
            LOG(ERROR) << "Document Update Failed in SDB. " << document.property("DOCID");
            return false;
        }

        indexManager_->updateRtypeDocument(oldIndexDocument, indexDocument);
    }
    else
    {
        uint32_t oldId = indexDocument.getId();
        if (!documentManager_->removeDocument(oldId))
        {
            //LOG(WARNING) << "document " << oldId << " is already deleted";
        }
        if (!documentManager_->insertDocument(document))
        {
            LOG(ERROR) << "Document Insert Failed in SDB. " << document.property("DOCID");
            return false;
        }

        indexManager_->updateDocument(indexDocument);
    }

    return true;
}

bool IndexWorker::deleteDoc_(docid_t docid, time_t timestamp)
{
    CREATE_SCOPED_PROFILER (proDocumentDeleting, "IndexWorker", "IndexWorker::DeleteDocument");

    if (hooker_)
    {
        if (!hooker_->HookDelete(docid, timestamp)) return false;
    }
    if (documentManager_->removeDocument(docid))
    {
        indexManager_->removeDocument(collectionId_, docid);
        ++numDeletedDocs_;
        indexStatus_.numDocs_ = indexManager_->numDocs();
        return true;
    }
    else return false;
}

void IndexWorker::savePriceHistory_(int op)
{
}

void IndexWorker::saveSourceCount_(int op)
{
    if (bundleConfig_->productSourceField_.empty())
        return;

    boost::posix_time::ptime now = boost::posix_time::microsec_clock::local_time();
    for (map<std::string, uint32_t>::const_iterator iter = productSourceCount_.begin();
        iter != productSourceCount_.end(); ++iter)
    {
        ProductCount productCount;
        productCount.setSource(iter->first);
        productCount.setCollection(bundleConfig_->collectionName_);
        productCount.setNum(iter->second);
        if (op == 1)
        {
            productCount.setFlag("insert");
        }
        else if (op == 2)
        {
            productCount.setFlag("update");
        }
        else
        {
            productCount.setFlag("delete");
        }
        productCount.setTimeStamp(now);
        productCount.save();
    }
}

bool IndexWorker::prepareDocument_(
        SCDDoc& doc,
        Document& document,
        docid_t& oldId,
        bool& rType,
        std::map<std::string, pair<PropertyDataType, izenelib::util::UString> >& rTypeFieldValue,
        std::string& source,
        time_t& timestamp,
        bool insert)
{
    CREATE_SCOPED_PROFILER (preparedocument, "IndexWorker", "IndexWorker::prepareDocument_");

    docid_t docId = 0;
    string fieldStr;
    vector<CharacterOffset> sentenceOffsetList;
    AnalysisInfo analysisInfo;
    if (doc.empty()) return false;
    // the iterator is not const because the p-second value may change
    // due to the maxlen setting

    SCDDoc::iterator p = doc.begin();
    bool dateExistInSCD = false;

    for (; p != doc.end(); p++)
    {
        const std::string& fieldStr = p->first;
        PropertyConfig temp;
        temp.propertyName_ = fieldStr;

        IndexBundleSchema::iterator iter = bundleConfig_->indexSchema_.find(temp);
        bool isIndexSchema = (iter != bundleConfig_->indexSchema_.end());

        const izenelib::util::UString & propertyValueU = p->second; // preventing copy

        if (!bundleConfig_->productSourceField_.empty()
              && boost::iequals(fieldStr,bundleConfig_->productSourceField_))
        {
            propertyValueU.convertString(source, bundleConfig_->encoding_);
        }

        if (boost::iequals(fieldStr,DOCID) && isIndexSchema)
        {
            izenelib::util::UString::EncodingType encoding = bundleConfig_->encoding_;
            std::string fieldValue;
            propertyValueU.convertString(fieldValue, encoding);

            // update
            if (!insert)
            {
                bool isUpdate = false;
                rType = checkRtype_(doc, rTypeFieldValue, isUpdate);
                if (rType && !isUpdate)
                {
                    //LOG(WARNING) << "skip updating SCD DOC " << fieldValue << ", as none of its property values is changed";
                    return false;
                }

                if (! createUpdateDocId_(Utilities::md5ToUint128(fieldValue), rType, oldId, docId))
                    insert = true;
            }

            if (insert && !createInsertDocId_(Utilities::md5ToUint128(fieldValue), docId))
            {
                //LOG(WARNING) << "failed to create id for SCD DOC " << fieldValue;
                return false;
            }

            document.setId(docId);
            PropertyValue propData(propertyValueU);
            document.property(fieldStr).swap(propData);
        }
        else if (boost::iequals(fieldStr,DATE))
        {
            /// format <DATE>20091009163011
            dateExistInSCD = true;
            izenelib::util::UString dateStr;
            timestamp = Utilities::createTimeStampInSeconds(propertyValueU, bundleConfig_->encoding_, dateStr);
            PropertyValue propData(propertyValueU);
            document.property(dateProperty_.getName()).swap(propData);
        }
        else if (isIndexSchema)
        {
            if (iter->getType() == STRING_PROPERTY_TYPE)
            {
                PropertyValue propData(propertyValueU);
                document.property(fieldStr).swap(propData);
                analysisInfo.clear();
                analysisInfo = iter->getAnalysisInfo();
                if (!analysisInfo.analyzerId_.empty())
                {
                    CREATE_SCOPED_PROFILER (prepare_summary, "IndexWorker", "IndexWorker::prepareDocument_::Summary");
                    unsigned int numOfSummary = 0;
                    if ((iter->getIsSnippet() || iter->getIsSummary()))
                    {
                        if (iter->getIsSummary())
                        {
                            numOfSummary = iter->getSummaryNum();
                            if (numOfSummary <= 0)
                                numOfSummary = 1; //atleast one sentence required for summary
                        }

                        if (!makeSentenceBlocks_(propertyValueU, iter->getDisplayLength(),
                                                numOfSummary, sentenceOffsetList))
                        {
                            LOG(ERROR) << "Make Sentence Blocks Failes ";
                        }
                        PropertyValue propData(sentenceOffsetList);
                        document.property(fieldStr + ".blocks").swap(propData);
                    }
                }
            }
            else if (iter->getType() == INT_PROPERTY_TYPE
                    || iter->getType() == FLOAT_PROPERTY_TYPE
                    || iter->getType() == NOMINAL_PROPERTY_TYPE)
            {
                PropertyValue propData(propertyValueU);
                document.property(fieldStr).swap(propData);
            }
            else
            {
            }
        }
    }

    if (dateExistInSCD) timestamp = -1;
    else
    {
        std::string dateStr = boost::posix_time::to_iso_string(boost::posix_time::from_time_t(timestamp / 1000000 - timezone));
        PropertyValue propData(izenelib::util::UString(dateStr.erase(8, 1), izenelib::util::UString::UTF_8));
        document.property(dateProperty_.getName()).swap(propData);
    }

    if (!insert && !rType)
    {
        if (!completePartialDocument_(oldId, document))
             return false;
    }
    return true;
}

bool IndexWorker::prepareIndexDocument_(
        docid_t oldId,
        time_t timestamp,
        const Document& document,
        IndexerDocument& indexDocument)
{
    CREATE_SCOPED_PROFILER (preparedocument, "IndexWorker", "IndexWorker::prepareIndexDocument_");
    CREATE_PROFILER (pid_date, "IndexWorker", "IndexWorker::prepareIndexDocument_::DATE");
    CREATE_PROFILER (pid_string, "IndexWorker", "IndexWorker::prepareIndexDocument_::STRING");
    CREATE_PROFILER (pid_int, "IndexWorker", "IndexWorker::prepareIndexDocument_::INT");
    CREATE_PROFILER (pid_float, "IndexWorker", "IndexWorker::prepareIndexDocument_::FLOAT");

    docid_t docId = document.getId();//new id;
    izenelib::util::UString::EncodingType encoding = bundleConfig_->encoding_;
    AnalysisInfo analysisInfo;
    typedef Document::property_const_iterator document_iterator;
    document_iterator p;
    // the iterator is not const because the p-second value may change
    // due to the maxlen setting
    for (p = document.propertyBegin(); p != document.propertyEnd(); ++p)
    {
        const string& fieldStr = p->first;

        PropertyConfig temp;
        temp.propertyName_ = fieldStr;
        IndexBundleSchema::iterator iter = bundleConfig_->indexSchema_.find(temp);

        if(iter == bundleConfig_->indexSchema_.end())
            continue;

        IndexerPropertyConfig indexerPropertyConfig;

        const izenelib::util::UString & propertyValueU = *(get<izenelib::util::UString>(&(p->second)));
        indexerPropertyConfig.setPropertyId(iter->getPropertyId());
        indexerPropertyConfig.setName(iter->getName());
        indexerPropertyConfig.setIsIndex(iter->isIndex());
        indexerPropertyConfig.setIsAnalyzed(iter->isAnalyzed());
        indexerPropertyConfig.setIsFilter(iter->getIsFilter());
        indexerPropertyConfig.setIsMultiValue(iter->getIsMultiValue());
        indexerPropertyConfig.setIsStoreDocLen(iter->getIsStoreDocLen());


        if (boost::iequals(fieldStr,DOCID))
        {
            indexDocument.setId(oldId);
            indexDocument.setDocId(docId, collectionId_);
        }
        else if (boost::iequals(fieldStr,DATE))
        {
            START_PROFILER(pid_date);
            /// format <DATE>20091009163011
            if(-1 == timestamp)
                timestamp = Utilities::createTimeStampInSeconds(propertyValueU);
            indexerPropertyConfig.setPropertyId(dateProperty_.getPropertyId());
            indexerPropertyConfig.setName(dateProperty_.getName());
            indexerPropertyConfig.setIsIndex(true);
            indexerPropertyConfig.setIsFilter(true);
            indexerPropertyConfig.setIsAnalyzed(false);
            indexerPropertyConfig.setIsMultiValue(false);
            indexDocument.insertProperty(indexerPropertyConfig, timestamp);
            STOP_PROFILER(pid_date);
        }
        else
        {
            if (iter->getType() == STRING_PROPERTY_TYPE)
            {
                START_PROFILER(pid_string);
                if (!propertyValueU.empty())
                {
                    ///process for properties that requires forward index to be created
                    if (iter->isIndex())
                    {
                        analysisInfo.clear();
                        analysisInfo = iter->getAnalysisInfo();
                        if (analysisInfo.analyzerId_.empty())
                        {
                            if (iter->getIsFilter() && iter->getIsMultiValue())
                            {
                                MultiValuePropertyType props;
                                split_string(propertyValueU,props, encoding,',');
                                indexDocument.insertProperty(indexerPropertyConfig, props);
                            }
                            else
                                indexDocument.insertProperty(indexerPropertyConfig,
                                                          propertyValueU);
                        }
                        else
                        {
                            laInputs_[iter->getPropertyId()]->setDocId(docId);
                            if (!makeForwardIndex_(propertyValueU, fieldStr, iter->getPropertyId(), analysisInfo))
                            {
                                LOG(ERROR) << "Forward Indexing Failed Error Line : " << __LINE__;
                                return false;
                            }
                            if (iter->getIsFilter())
                            {
                                if (iter->getIsMultiValue())
                                {
                                    MultiValuePropertyType props;
                                    split_string(propertyValueU,props, encoding,',');

                                    MultiValueIndexPropertyType
                                    indexData = std::make_pair(laInputs_[iter->getPropertyId()],props);
                                    indexDocument.insertProperty(indexerPropertyConfig, indexData);

                                }
                                else
                                {
                                    IndexPropertyType
                                    indexData =
                                        std::make_pair(
                                            laInputs_[iter->getPropertyId()],
                                            const_cast<izenelib::util::UString &>(propertyValueU));
                                    indexDocument.insertProperty(indexerPropertyConfig, indexData);
                                }
                            }
                            else
                                indexDocument.insertProperty(
                                    indexerPropertyConfig, laInputs_[iter->getPropertyId()]);

                            // For alias indexing
                            config_tool::PROPERTY_ALIAS_MAP_T::iterator mapIter =
                                propertyAliasMap_.find(iter->getName());
                            if (mapIter != propertyAliasMap_.end()) // if there's alias property
                            {
                                std::vector<PropertyConfig>::iterator vecIter =
                                    mapIter->second.begin();
                                for (; vecIter != mapIter->second.end(); vecIter++)
                                {
                                    AnalysisInfo aliasAnalysisInfo =
                                        vecIter->getAnalysisInfo();
                                    laInputs_[vecIter->getPropertyId()]->setDocId(docId);
                                    if (!makeForwardIndex_(
                                                propertyValueU,
                                                fieldStr,
                                                vecIter->getPropertyId(),
                                                aliasAnalysisInfo))
                                    {
                                        LOG(ERROR) << "Forward Indexing Failed Error Line : " << __LINE__;
                                        return false;
                                    }
                                    IndexerPropertyConfig
                                    aliasIndexerPropertyConfig(
                                        vecIter->getPropertyId(),
                                        vecIter->getName(),
                                        vecIter->isIndex(),
                                        vecIter->isAnalyzed());
                                    aliasIndexerPropertyConfig.setIsFilter(vecIter->getIsFilter());
                                    aliasIndexerPropertyConfig.setIsMultiValue(vecIter->getIsMultiValue());
                                    aliasIndexerPropertyConfig.setIsStoreDocLen(vecIter->getIsStoreDocLen());
                                    indexDocument.insertProperty(
                                        aliasIndexerPropertyConfig,laInputs_[vecIter->getPropertyId()]);
                                } // end - for
                            } // end - if (mapIter != end())

                        }
                    }
                    // insert property name and value for other properties that is not DOCID and neither required to be indexed
                    else
                    {
                        //other extra properties that need not be in index manager
                        indexDocument.insertProperty(indexerPropertyConfig,
                                                      propertyValueU);
                    }
                }
                STOP_PROFILER(pid_string);
            }
            else if (iter->getType() == INT_PROPERTY_TYPE)
            {
                START_PROFILER(pid_int);
                if (iter->isIndex())
                {
                    if (iter->getIsMultiValue())
                    {
                        MultiValuePropertyType props;
                        split_int(propertyValueU,props, encoding,',');
                        indexDocument.insertProperty(indexerPropertyConfig, props);
                    }
                    else
                    {
                        std::string str("");
                        propertyValueU.convertString(str, encoding);
                        int64_t value = 0;
                        try
                        {
                            value = boost::lexical_cast< int64_t >(str);
                            indexDocument.insertProperty(indexerPropertyConfig, value);
                        }
                        catch (const boost::bad_lexical_cast &)
                        {
                            MultiValuePropertyType multiProps;
                            if (checkSeparatorType_(propertyValueU, encoding, '-'))
                            {
                                split_int(propertyValueU, multiProps, encoding,'-');
                                indexerPropertyConfig.setIsMultiValue(true);
                                indexDocument.insertProperty(indexerPropertyConfig, multiProps);
                            }
                            else if (checkSeparatorType_(propertyValueU, encoding, '~'))
                            {
                                split_int(propertyValueU, multiProps, encoding,'~');
                                indexerPropertyConfig.setIsMultiValue(true);
                                indexDocument.insertProperty(indexerPropertyConfig, multiProps);
                            }
                            else if (checkSeparatorType_(propertyValueU, encoding, ','))
                            {
                                split_int(propertyValueU, multiProps, encoding,',');
                                indexerPropertyConfig.setIsMultiValue(true);
                                indexDocument.insertProperty(indexerPropertyConfig, multiProps);
                            }
                            else
                            {
                                try
                                {
                                    value = (int64_t)(boost::lexical_cast< float >(str));
                                    indexDocument.insertProperty(indexerPropertyConfig, value);
                                }catch (const boost::bad_lexical_cast &)
                                {
                                    //LOG(ERROR) << "Wrong format of number value. DocId " << docId <<" Property "<<fieldStr<< " Value" << str;
                                }
                            }
                        }
                    }
                }
                STOP_PROFILER(pid_int);
            }
            else if (iter->getType() == FLOAT_PROPERTY_TYPE)
            {
                START_PROFILER(pid_float);
                if (iter->isIndex())
                {
                    if (iter->getIsMultiValue())
                    {
                        MultiValuePropertyType props;
                        split_float(propertyValueU,props, encoding,',');
                        indexDocument.insertProperty(indexerPropertyConfig, props);
                    }
                    else
                    {
                        std::string str("");
                        propertyValueU.convertString(str, encoding);
                        float value = 0;
                        try
                        {
                            value = boost::lexical_cast< float >(str);
                            indexDocument.insertProperty(indexerPropertyConfig, value);
                        }
                        catch (const boost::bad_lexical_cast &)
                        {
                            MultiValuePropertyType multiProps;
                            if (checkSeparatorType_(propertyValueU, encoding, '-'))
                            {
                                split_float(propertyValueU, multiProps, encoding,'-');
                            }
                            else if (checkSeparatorType_(propertyValueU, encoding, '~'))
                            {
                                split_float(propertyValueU, multiProps, encoding,'~');
                            }
                            else if (checkSeparatorType_(propertyValueU, encoding, ','))
                            {
                                split_float(propertyValueU, multiProps, encoding,',');
                            }
                            indexerPropertyConfig.setIsMultiValue(true);
                            indexDocument.insertProperty(indexerPropertyConfig, multiProps);
                        }
                    }
                }
                STOP_PROFILER(pid_float);
            }
            else
            {
            }
        }
    }

    return true;
}

bool IndexWorker::checkSeparatorType_(
        const izenelib::util::UString& propertyValueStr,
        izenelib::util::UString::EncodingType encoding,
        char separator)
{
    izenelib::util::UString tmpStr(propertyValueStr);
    izenelib::util::UString sep(" ",encoding);
    sep[0] = separator;
    size_t n = 0;
    n = tmpStr.find(sep,0);
    if (n != izenelib::util::UString::npos)
        return true;
    return false;
}

bool IndexWorker::preparePartialDocument_(
        Document& document,
        IndexerDocument& oldIndexDocument)
{
    // Store the old property value.
    docid_t docId = document.getId();
    Document oldDoc;

    if (!documentManager_->getDocument(docId, oldDoc))
    {
        return false;
    }

    typedef Document::property_const_iterator iterator;
    for (iterator it = document.propertyBegin(), itEnd = document.propertyEnd(); it
                 != itEnd; ++it) {
        if(! boost::iequals(it->first,DOCID) && ! boost::iequals(it->first,DATE))
        {
            PropertyConfig temp;
            temp.propertyName_ = it->first;
            IndexBundleSchema::iterator iter = bundleConfig_->indexSchema_.find(temp);

            if (iter == bundleConfig_->indexSchema_.end())
                continue;

            //set indexerPropertyConfig
            IndexerPropertyConfig indexerPropertyConfig;
            if (iter->isIndex() && iter->getIsFilter() && !iter->isAnalyzed())
            {
                indexerPropertyConfig.setPropertyId(iter->getPropertyId());
                indexerPropertyConfig.setName(iter->getName());
                indexerPropertyConfig.setIsIndex(iter->isIndex());
                indexerPropertyConfig.setIsAnalyzed(iter->isAnalyzed());
                indexerPropertyConfig.setIsFilter(iter->getIsFilter());
                indexerPropertyConfig.setIsMultiValue(iter->getIsMultiValue());
                indexerPropertyConfig.setIsStoreDocLen(iter->getIsStoreDocLen());

                PropertyValue propertyValue = oldDoc.property(it->first);
                const izenelib::util::UString* stringValue = get<izenelib::util::UString>(&propertyValue);

                izenelib::util::UString::EncodingType encoding = bundleConfig_->encoding_;
                std::string str("");
                stringValue->convertString(str, encoding);
                if (iter->getType() == INT_PROPERTY_TYPE)
                {
                    if (iter->getIsMultiValue())
                    {
                        MultiValuePropertyType props;
                        split_int(*stringValue, props, encoding, ',');
                        oldIndexDocument.insertProperty(indexerPropertyConfig, props);
                    }
                    else
                    {
                        int64_t value = 0;
                        try
                        {
                            value = boost::lexical_cast<int64_t>(str);
                            oldIndexDocument.insertProperty(indexerPropertyConfig, value);
                        }
                        catch (const boost::bad_lexical_cast &)
                        {
                            MultiValuePropertyType props;
                            if (checkSeparatorType_(*stringValue, encoding, '-'))
                            {
                                split_int(*stringValue, props, encoding,'-');
                            }
                            else if (checkSeparatorType_(*stringValue, encoding, '~'))
                            {
                                split_int(*stringValue, props, encoding,'~');
                            }
                            else if (checkSeparatorType_(*stringValue, encoding, ','))
                            {
                                split_int(*stringValue, props, encoding,',');
                            }
                            indexerPropertyConfig.setIsMultiValue(true);
                            oldIndexDocument.insertProperty(indexerPropertyConfig, props);
                         }
                    }
                }
                else if (iter->getType() == FLOAT_PROPERTY_TYPE)
                {
                    if (iter->getIsMultiValue())
                    {
                        MultiValuePropertyType props;
                        split_float(*stringValue, props, encoding,',');
                        oldIndexDocument.insertProperty(indexerPropertyConfig, props);
                    }
                    else
                    {
                        float value = 0.0;
                        try
                        {
                            value = boost::lexical_cast< float >(str);
                            oldIndexDocument.insertProperty(indexerPropertyConfig, value);
                        }
                        catch (const boost::bad_lexical_cast &)
                        {
                            MultiValuePropertyType props;
                            if (checkSeparatorType_(*stringValue, encoding, '-'))
                            {
                                split_float(*stringValue, props, encoding,'-');
                            }
                            else if (checkSeparatorType_(*stringValue, encoding, '~'))
                            {
                                split_float(*stringValue, props, encoding,'~');
                            }
                            else if (checkSeparatorType_(*stringValue, encoding, ','))
                            {
                                split_float(*stringValue, props, encoding,',');
                            }
                            indexerPropertyConfig.setIsMultiValue(true);
                            oldIndexDocument.insertProperty(indexerPropertyConfig, props);
                         }
                    }
                }
                else if(iter->getType() == STRING_PROPERTY_TYPE)
                {
                    if (iter->getIsMultiValue())
                    {
                        MultiValuePropertyType props;
                        split_string(*stringValue, props, encoding, ',');
                        oldIndexDocument.insertProperty(indexerPropertyConfig, props);
                    }
                    else
                    {
                        oldIndexDocument.insertProperty(indexerPropertyConfig, * stringValue);
                    }
                }
            }
        }
    }
    return true;
}

bool IndexWorker::checkRtype_(
    SCDDoc& doc,
    std::map<std::string, pair<PropertyDataType, izenelib::util::UString> >& rTypeFieldValue,
    bool& isUpdate
)
{
    //R-type check
    bool rType = false;
    docid_t docId = 0;
    izenelib::util::UString newPropertyValue, oldPropertyValue;
    SCDDoc::iterator p = doc.begin();

    for (; p != doc.end(); ++p)
    {
        const string& fieldName = p->first;
        const izenelib::util::UString & propertyValueU = p->second;

        PropertyConfig tempPropertyConfig;
        tempPropertyConfig.propertyName_ = fieldName;
        IndexBundleSchema::iterator iter = bundleConfig_->indexSchema_.find(tempPropertyConfig);

        if (iter == bundleConfig_->indexSchema_.end())
            break;

        if (boost::iequals(fieldName,DOCID))
        {
            std::string docid_str;
            propertyValueU.convertString(docid_str, izenelib::util::UString::UTF_8);
            if (!idManager_->getDocIdByDocName(Utilities::md5ToUint128(docid_str), docId, false))
                break;

            continue;
        }

        newPropertyValue = propertyValueU;
        if (boost::iequals(fieldName,DATE))
        {
            izenelib::util::UString dateStr;
            Utilities::createTimeStampInSeconds(propertyValueU, bundleConfig_->encoding_, dateStr);
            newPropertyValue = dateStr;
        }

        string newValueStr(""), oldValueStr("");
        newPropertyValue.convertString(newValueStr, izenelib::util::UString::UTF_8);

        PropertyValue value;
        if (!documentManager_->getPropertyValue(docId, iter->getName(), value))
            break;

        if (!getPropertyValue_(value, oldValueStr))
            return false;

        if (newValueStr == oldValueStr)
            continue;

        if (iter->isIndex() && iter->getIsFilter() && !iter->isAnalyzed())
        {
            rTypeFieldValue[iter->getName()] = std::make_pair(iter->getType(),newPropertyValue);
            isUpdate = true;
        }
        else if(!iter->isIndex())
        {
            isUpdate = true;
        }
        else
        {
            break;
        }
    }

    if (p == doc.end())
    {
        rType = true;
    }
    else
    {
        rType = false;
        rTypeFieldValue.clear();
    }

    return rType;
}

bool IndexWorker::makeSentenceBlocks_(
        const izenelib::util::UString & text,
        const unsigned int maxDisplayLength,
        const unsigned int numOfSummary,
        vector<CharacterOffset>& sentenceOffsetList)
{
    sentenceOffsetList.clear();
    if (!summarizer_.getOffsetPairs(text, maxDisplayLength, numOfSummary, sentenceOffsetList))
    {
        return false;
    }
    return true;
}

/// @desc Make a forward index of a given text.
/// You can specify an Language Analysis option through AnalysisInfo parameter.
/// You have to get a proper AnalysisInfo value from the configuration. (Currently not implemented.)
bool IndexWorker::makeForwardIndex_(
        const izenelib::util::UString& text,
        const std::string& propertyName,
        unsigned int propertyId,
        const AnalysisInfo& analysisInfo)
{
    CREATE_SCOPED_PROFILER(proTermExtracting, "IndexWorker", "Analyzer overhead");

//    la::TermIdList termIdList;
    laInputs_[propertyId]->resize(0);

    // Remove the spaces between two Chinese Characters
//    izenelib::util::UString refinedText;
//    la::removeRedundantSpaces(text, refinedText);
//    if (!laManager_->getTermList(refinedText, analysisInfo, true, termList, true))
    la::MultilangGranularity indexingLevel = bundleConfig_->indexMultilangGranularity_;
    if (indexingLevel == la::SENTENCE_LEVEL)
    {
        if (bundleConfig_->bIndexUnigramProperty_)
        {
            if (propertyName.find("_unigram") != std::string::npos)
                indexingLevel = la::FIELD_LEVEL;  /// for unigram property, we do not need sentence level indexing
        }
    }

    if (!laManager_->getTermIdList(idManager_.get(), text, analysisInfo, (*laInputs_[propertyId]), indexingLevel))
            return false;

    return true;
}

size_t IndexWorker::getTotalScdSize_()
{
    string scdPath = bundleConfig_->indexSCDPath();

    ScdParser parser(bundleConfig_->encoding_);

    size_t sizeInBytes = 0;
    // search the directory for files
    static const bfs::directory_iterator kItrEnd;
    for (bfs::directory_iterator itr(scdPath); itr != kItrEnd; ++itr)
    {
        if (bfs::is_regular_file(itr->status()))
        {
            std::string fileName = itr->path().filename().string();
            if (parser.checkSCDFormat(fileName))
            {
                parser.load(scdPath+fileName);
                sizeInBytes += parser.getFileSize();
            }
        }
    }
    return sizeInBytes/(1024*1024);
}

bool IndexWorker::requireBackup_(size_t currTotalScdSizeInMB)
{
    static size_t threshold = 200;//200M
    totalSCDSizeSinceLastBackup_ += currTotalScdSizeInMB;
    const boost::shared_ptr<Directory>& current
        = directoryRotator_.currentDirectory();
    const boost::shared_ptr<Directory>& next
        = directoryRotator_.nextDirectory();
    if (next
        && current->name() != next->name())
        //&& ! (next->valid() && next->parentName() == current->name()))
    {
        ///TODO policy required here
        if(totalSCDSizeSinceLastBackup_ > threshold)
        return true;
    }
    return false;
}

bool IndexWorker::backup_()
{
    const boost::shared_ptr<Directory>& current
        = directoryRotator_.currentDirectory();
    const boost::shared_ptr<Directory>& next
        = directoryRotator_.nextDirectory();

    // valid pointer
    // && not the same directory
    // && have not copied successfully yet
    if (next
        && current->name() != next->name())
        //&& ! (next->valid() && next->parentName() == current->name()))
    {
        try
        {
            LOG(INFO) << "Copy index dir from " << current->name()
                      << " to " << next->name();
            next->copyFrom(*current);
            return true;
        }
        catch (bfs::filesystem_error& e)
        {
            LOG(ERROR) << "Failed to copy index directory " << e.what();
        }

        // try copying but failed
        return false;
    }

    // not copy, always returns true
    return true;
}

bool IndexWorker::recoverSCD_()
{
    const boost::shared_ptr<Directory>& currentDir
        = directoryRotator_.currentDirectory();
    const boost::shared_ptr<Directory>& next
        = directoryRotator_.nextDirectory();

    if (!(next && currentDir->name() != next->name()))
        return false;

    std::ifstream scdLogInput;
    scdLogInput.open(currentDir->scdLogString().c_str());
    std::istream_iterator<std::string> begin(scdLogInput);
    std::istream_iterator<std::string> end;
    boost::unordered_set<std::string> existingSCDs;
    for(std::istream_iterator<std::string> it = begin; it != end; ++it)
    {
        std::cout << *it << "@@"<<std::endl;
        existingSCDs.insert(*it);
    }
    if(existingSCDs.empty()) return false;
    bfs::path scdBkDir = bfs::path(bundleConfig_->indexSCDPath()) / SCD_BACKUP_DIR;

    try
    {
        if (!bfs::is_directory(scdBkDir))
        {
            return false;
        }
    }
    catch (bfs::filesystem_error& e)
    {
        return false;
    }

    // search the directory for files
    static const bfs::directory_iterator kItrEnd;
    bfs::path scdIndexDir = bfs::path(bundleConfig_->indexSCDPath());

    for (bfs::directory_iterator itr(scdBkDir); itr != kItrEnd; ++itr)
    {
        if (bfs::is_regular_file(itr->status()))
        {
            std::string fileName = itr->path().filename().string();
            if(existingSCDs.find(fileName) == existingSCDs.end())
            {
                try
                {
                    bfs::rename(*itr, scdIndexDir / bfs::path(fileName));
                }
                catch (bfs::filesystem_error& e)
                {
                    LOG(WARNING) << "exception in recovering file " << fileName << ": " << e.what();
                }
            }
        }
    }

    return true;
}

void IndexWorker::value2SCDDoc(const Value& value, SCDDoc& scddoc)
{
    const Value::ObjectType& objectValue = value.getObject();
    scddoc.resize(objectValue.size());

    std::size_t propertyId = 0;
    for (Value::ObjectType::const_iterator it = objectValue.begin();
         it != objectValue.end(); ++it, ++propertyId)
    {
        scddoc[propertyId].first.assign(asString(it->first));
        scddoc[propertyId].second.assign(
            asString(it->second),
            izenelib::util::UString::UTF_8
        );
    }
}


}
