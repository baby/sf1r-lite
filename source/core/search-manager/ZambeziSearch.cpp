#include "ZambeziSearch.h"
#include "ZambeziFilter.h"
#include "SearchManagerPreProcessor.h"
#include "Sorter.h"
#include "QueryBuilder.h"
#include "HitQueue.h"
#include <la-manager/AttrTokenizeWrapper.h>
#include <query-manager/SearchKeywordOperation.h>
#include <query-manager/QueryTypeDef.h> // FilteringType
#include <common/ResultType.h>
#include <common/ResourceManager.h>
#include <common/PropSharedLockSet.h>
#include <document-manager/DocumentManager.h>
#include <mining-manager/MiningManager.h>
#include <mining-manager/zambezi-manager/ZambeziManager.h>
#include <mining-manager/group-manager/GroupFilterBuilder.h>
#include <mining-manager/group-manager/GroupFilter.h>
#include <mining-manager/product-scorer/ProductScorer.h>
#include <mining-manager/util/convert_ustr.h>
#include <b5m-manager/product_matcher.h>
#include <ir/index_manager/utility/BitVector.h>
#include <util/ClockTimer.h>
#include <glog/logging.h>
#include <iostream>
#include <set>
#include <boost/scoped_ptr.hpp>

namespace sf1r
{

namespace
{
const std::size_t kAttrTopDocNum = 200;
const std::size_t kZambeziTopKNum = 1e6;

const std::string kTopLabelPropName = "Category";
const size_t kRootCateNum = 10;

const std::string kMerchantPropName = "Source";
const izenelib::util::UString::EncodingType kEncodeType =
    izenelib::util::UString::UTF_8;
const izenelib::util::UString kAttrExcludeMerchant =
    izenelib::util::UString("淘宝网", kEncodeType);

const izenelib::util::UString::CharT kUCharSpace = ' ';

const MonomorphicFilter<true> kAllPassFilter;
}

ZambeziSearch::ZambeziSearch(
    DocumentManager& documentManager,
    SearchManagerPreProcessor& preprocessor,
    QueryBuilder& queryBuilder)
    : documentManager_(documentManager)
    , preprocessor_(preprocessor)
    , queryBuilder_(queryBuilder)
    , groupFilterBuilder_(NULL)
    , zambeziManager_(NULL)
    , categoryValueTable_(NULL)
    , merchantValueTable_(NULL)
{
}

void ZambeziSearch::setMiningManager(
    const boost::shared_ptr<MiningManager>& miningManager)
{
    groupFilterBuilder_ = miningManager->GetGroupFilterBuilder();
    zambeziManager_ = miningManager->getZambeziManager();
    categoryValueTable_ = miningManager->GetPropValueTable(kTopLabelPropName);
    merchantValueTable_ = miningManager->GetPropValueTable(kMerchantPropName);
}

bool ZambeziSearch::search(
    const SearchKeywordOperation& actionOperation,
    KeywordSearchResult& searchResult,
    std::size_t limit,
    std::size_t offset)
{
    const std::string& query = actionOperation.actionItem_.env_.queryString_;
    LOG(INFO) << "zambezi search for query: " << query;

    if (query.empty())
        return false;

    std::vector<docid_t> candidates;
    std::vector<uint32_t> scores;

    if (!zambeziManager_)
    {
        LOG(WARNING) << "the instance of ZambeziManager is empty";
        return false;
    }

    faceted::GroupParam& groupParam = actionOperation.actionItem_.groupParam_;
    const bool originIsAttrGroup = groupParam.isAttrGroup_;
    groupParam.isAttrGroup_ = false;

    PropSharedLockSet propSharedLockSet;
    boost::scoped_ptr<ProductScorer> productScorer(
        preprocessor_.createProductScorer(actionOperation.actionItem_, propSharedLockSet, NULL));

    boost::shared_ptr<faceted::GroupFilter> groupFilter;
    if (groupFilterBuilder_)
    {
        groupFilter.reset(
            groupFilterBuilder_->createFilter(groupParam, propSharedLockSet));
    }

    const std::vector<QueryFiltering::FilteringType>& filterList =
        actionOperation.actionItem_.filteringList_;
    boost::shared_ptr<InvertedIndexManager::FilterBitmapT> filterBitmap;
    boost::shared_ptr<izenelib::ir::indexmanager::BitVector> filterBitVector;

    if (!filterList.empty())
    {
        queryBuilder_.prepare_filter(filterList, filterBitmap);
        filterBitVector.reset(new izenelib::ir::indexmanager::BitVector);
        filterBitVector->importFromEWAH(*filterBitmap);
    }

    AttrTokenizeWrapper* attrTokenize = AttrTokenizeWrapper::get();
    std::vector<std::pair<std::string, int> > tokenList;
    attrTokenize->attr_tokenize(query, tokenList);
    getAnalyzedQuery_(query, searchResult.analyzedQuery_);

    zambeziManager_->search(tokenList, kAllPassFilter, kZambeziTopKNum,
                            candidates, scores);

    if (candidates.empty())
    {
        std::vector<std::pair<std::string, int> > subTokenList;
        if (attrTokenize->attr_subtokenize(tokenList, subTokenList))
        {
            zambeziManager_->search(subTokenList, kAllPassFilter, kZambeziTopKNum,
                                    candidates, scores);
        }
    }

    if (candidates.empty())
    {
        LOG(INFO) << "empty search result for query: " << query;
        return false;
    }

    if (candidates.size() != scores.size())
    {
        LOG(WARNING) << "mismatch size of candidate docid and score";
        return false;
    }

    izenelib::util::ClockTimer timer;

    boost::shared_ptr<Sorter> sorter;
    CustomRankerPtr customRanker;
    preprocessor_.prepareSorterCustomRanker(actionOperation,
                                            sorter,
                                            customRanker);

    boost::scoped_ptr<HitQueue> scoreItemQueue;
    const std::size_t heapSize = limit + offset;

    if (sorter)
    {
        scoreItemQueue.reset(new PropertySortedHitQueue(sorter,
                                                        heapSize,
                                                        propSharedLockSet));
    }
    else
    {
        scoreItemQueue.reset(new ScoreSortedHitQueue(heapSize));
    }

    // reset relevance score
    const std::size_t candNum = candidates.size();
    std::size_t totalCount = 0;

    {
        ZambeziFilter filter(documentManager_, groupFilter, filterBitVector);

        for (size_t i = 0; i < candNum; ++i)
        {
            docid_t docId = candidates[i];

            if (!filter.test(docId))
                continue;

            ScoreDoc scoreItem(docId, scores[i]);
            if (customRanker)
            {
                scoreItem.custom_score = customRanker->evaluate(docId);
            }
            scoreItemQueue->insert(scoreItem);

            ++totalCount;
        }
    }

    /// ret score, add product score;
    std::vector<docid_t> topDocids;
    std::vector<float> topRelevanceScores;
    std::vector<float> topProductScores;

    unsigned int scoreSize = scoreItemQueue->size();
    for (int i = scoreSize - 1; i >= 0; --i)
    {
        const ScoreDoc& scoreItem = scoreItemQueue->pop();
        topDocids.push_back(scoreItem.docId);
        float productScore = 0;
        if (productScorer)
               productScore = productScorer->score(scoreItem.docId);
        topProductScores.push_back(productScore);
        topRelevanceScores.push_back(scoreItem.score);
    }
    zambeziManager_->NormalizeScore(topDocids, topRelevanceScores, topProductScores, propSharedLockSet);

    for (size_t i = 0; i < topRelevanceScores.size(); ++i)
    {
        ScoreDoc scoreItem(topDocids[i], topRelevanceScores[i]);
        scoreItemQueue->insert(scoreItem);
    }
    /// end

    searchResult.totalCount_ = totalCount;
    std::size_t topKCount = 0;
    if (offset < scoreItemQueue->size())// if bigger is zero;
    {
        topKCount = scoreItemQueue->size() - offset;
    }

    std::vector<unsigned int>& docIdList = searchResult.topKDocs_;
    std::vector<float>& rankScoreList = searchResult.topKRankScoreList_;
    std::vector<float>& customScoreList = searchResult.topKCustomRankScoreList_;

    docIdList.resize(topKCount);
    rankScoreList.resize(topKCount);

    if (customRanker)
    {
        customScoreList.resize(topKCount);
    }

    for (int i = topKCount-1; i >= 0; --i)
    {
        const ScoreDoc& scoreItem = scoreItemQueue->pop();
        docIdList[i] = scoreItem.docId;
        rankScoreList[i] = scoreItem.score;
        if (customRanker)
        {
            customScoreList[i] = scoreItem.custom_score;
        }
    }

    if (groupFilter)
    {
        getTopLabels_(docIdList, rankScoreList,
                      propSharedLockSet,
                      searchResult.autoSelectGroupLabels_);

        sf1r::faceted::OntologyRep tempAttrRep;
        groupFilter->getGroupRep(searchResult.groupRep_, tempAttrRep);
    }

    if (originIsAttrGroup)
    {
        getTopAttrs_(docIdList, groupParam, propSharedLockSet,
                     searchResult.attrRep_);
    }

    if (sorter)
    {
        preprocessor_.fillSearchInfoWithSortPropertyData(sorter.get(),
                                                         docIdList,
                                                         searchResult.distSearchInfo_,
                                                         propSharedLockSet);
    }

    LOG(INFO) << "in zambezi ranking, total count: " << totalCount
              << ", costs :" << timer.elapsed() << " seconds";

    return true;
}

void ZambeziSearch::getTopLabels_(
    const std::vector<unsigned int>& docIdList,
    const std::vector<float>& rankScoreList,
    PropSharedLockSet& propSharedLockSet,
    faceted::GroupParam::GroupLabelScoreMap& topLabelMap)
{
    if (!categoryValueTable_)
        return;

    izenelib::util::ClockTimer timer;
    propSharedLockSet.insertSharedLock(categoryValueTable_);

    typedef std::vector<std::pair<faceted::PropValueTable::pvid_t, faceted::GroupPathScoreInfo> > TopCatIdsT;
    TopCatIdsT topCateIds;
    const std::size_t topNum = docIdList.size();
    std::set<faceted::PropValueTable::pvid_t> rootCateIds;

    for (std::size_t i = 0; i < topNum; ++i)
    {
        if (rootCateIds.size() >= kRootCateNum)
            break;

        category_id_t catId =
            categoryValueTable_->getFirstValueId(docIdList[i]);

        if (catId != 0)
        {
            bool is_exist = false;
            for (TopCatIdsT::const_iterator cit = topCateIds.begin();
                 cit != topCateIds.end(); ++cit)
            {
                if (cit->first == catId)
                {
                    is_exist = true;
                    break;
                }
            }
            if (!is_exist)
            {
                topCateIds.push_back(std::make_pair(catId, faceted::GroupPathScoreInfo(rankScoreList[i], docIdList[i])));

                category_id_t rootId = categoryValueTable_->getRootValueId(catId);
                rootCateIds.insert(rootId);
            }
        }
    }

    faceted::GroupParam::GroupPathScoreVec& topLabels = topLabelMap[kTopLabelPropName];
    for (TopCatIdsT::const_iterator idIt =
             topCateIds.begin(); idIt != topCateIds.end(); ++idIt)
    {
        std::vector<izenelib::util::UString> ustrPath;
        categoryValueTable_->propValuePath(idIt->first, ustrPath, false);

        std::vector<std::string> path;
        convert_to_str_vector(ustrPath, path);

        topLabels.push_back(std::make_pair(path, idIt->second));
    }

    LOG(INFO) << "get top label num: "<< topLabels.size()
              << ", costs: " << timer.elapsed() << " seconds";
}

void ZambeziSearch::getTopAttrs_(
    const std::vector<unsigned int>& docIdList,
    faceted::GroupParam& groupParam,
    PropSharedLockSet& propSharedLockSet,
    faceted::OntologyRep& attrRep)
{
    if (!groupFilterBuilder_)
        return;

    izenelib::util::ClockTimer timer;

    faceted::GroupParam attrGroupParam;
    attrGroupParam.isAttrGroup_ = groupParam.isAttrGroup_ = true;
    attrGroupParam.attrGroupNum_ = groupParam.attrGroupNum_;
    attrGroupParam.searchMode_ = groupParam.searchMode_;

    boost::scoped_ptr<faceted::GroupFilter> attrGroupFilter(
        groupFilterBuilder_->createFilter(attrGroupParam, propSharedLockSet));

    if (!attrGroupFilter)
        return;

    faceted::PropValueTable::pvid_t excludeMerchantId = 0;
    if (merchantValueTable_)
    {
        std::vector<izenelib::util::UString> path;
        path.push_back(kAttrExcludeMerchant);

        propSharedLockSet.insertSharedLock(merchantValueTable_);
        excludeMerchantId = merchantValueTable_->propValueId(path, false);
    }

    size_t testNum = 0;
    for (size_t i = 0; i < docIdList.size() && testNum < kAttrTopDocNum; ++i)
    {
        docid_t docId = docIdList[i];

        if (excludeMerchantId &&
            merchantValueTable_->testDoc(docId, excludeMerchantId))
            continue;

        attrGroupFilter->test(docId);
        ++testNum;
    }

    faceted::GroupRep tempGroupRep;
    attrGroupFilter->getGroupRep(tempGroupRep, attrRep);

    LOG(INFO) << "attrGroupFilter costs :" << timer.elapsed() << " seconds";
}

void ZambeziSearch::getAnalyzedQuery_(
    const std::string& rawQuery,
    izenelib::util::UString& analyzedQuery)
{
    b5m::ProductMatcher* matcher = b5m::ProductMatcherInstance::get();

    if (!matcher->IsOpen())
        return;

    typedef std::pair<izenelib::util::UString, double> TokenScore;
    typedef std::list<TokenScore> TokenScoreList;
    TokenScoreList majorTokens;
    TokenScoreList minorTokens;
    std::list<izenelib::util::UString> leftTokens;
    izenelib::util::UString queryUStr(rawQuery, izenelib::util::UString::UTF_8);

    matcher->GetSearchKeywords(queryUStr, majorTokens, minorTokens, leftTokens);

    for (TokenScoreList::const_iterator it = majorTokens.begin();
         it != majorTokens.end(); ++it)
    {
        const izenelib::util::UString& token = it->first;

        if (queryUStr.find(token) == izenelib::util::UString::npos)
            continue;

        analyzedQuery.append(token);
        analyzedQuery.push_back(kUCharSpace);
    }
}

}
