/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2015 Couchbase, Inc
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include "config.h"

#ifdef _MSC_VER
#define PATH_MAX MAX_PATH
#endif

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <list>
#include <map>
#include <phosphor/phosphor.h>
#include <platform/cb_malloc.h>
#include <platform/checked_snprintf.h>
#include <string>
#include <utility>
#include <vector>
#include <cJSON.h>
#include <platform/dirutils.h>

#include "common.h"
#include "couch-kvstore/couch-kvstore.h"
#include "ep_types.h"
#define STATWRITER_NAMESPACE couchstore_engine
#include "statwriter.h"
#undef STATWRITER_NAMESPACE
#include "vbucket.h"
#include "vbucket_bgfetch_item.h"

#include <JSON_checker.h>
#include <kvstore.h>
#include <platform/compress.h>

extern "C" {
    static int recordDbDumpC(Db *db, DocInfo *docinfo, void *ctx)
    {
        return CouchKVStore::recordDbDump(db, docinfo, ctx);
    }
}

extern "C" {
    static int getMultiCbC(Db *db, DocInfo *docinfo, void *ctx)
    {
        return CouchKVStore::getMultiCb(db, docinfo, ctx);
    }
}

static std::string getStrError(Db *db) {
    const size_t max_msg_len = 256;
    char msg[max_msg_len];
    couchstore_last_os_error(db, msg, max_msg_len);
    std::string errorStr(msg);
    return errorStr;
}

/**
 * Determine the datatype for a blob. It is _highly_ unlikely that
 * this method is being called, as it would have to be for an item
 * which is read off the disk _before_ we started to write the
 * datatype to disk (we did that in a 3.x server).
 *
 * @param doc The document to check
 * @return JSON or RAW bytes
 */
static protocol_binary_datatype_t determine_datatype(sized_buf doc) {
    if (checkUTF8JSON(reinterpret_cast<uint8_t*>(doc.buf), doc.size)) {
        return PROTOCOL_BINARY_DATATYPE_JSON;
    } else {
        return PROTOCOL_BINARY_RAW_BYTES;
    }
}

static bool endWithCompact(const std::string &filename) {
    size_t pos = filename.find(".compact");
    if (pos == std::string::npos ||
                        (filename.size() - sizeof(".compact")) != pos) {
        return false;
    }
    return true;
}

static void discoverDbFiles(const std::string &dir,
                            std::vector<std::string> &v) {
    auto files = cb::io::findFilesContaining(dir, ".couch");
    std::vector<std::string>::iterator ii;
    for (ii = files.begin(); ii != files.end(); ++ii) {
        if (!endWithCompact(*ii)) {
            v.push_back(*ii);
        }
    }
}

static int getMutationStatus(couchstore_error_t errCode) {
    switch (errCode) {
    case COUCHSTORE_SUCCESS:
        return MUTATION_SUCCESS;
    case COUCHSTORE_ERROR_NO_HEADER:
    case COUCHSTORE_ERROR_NO_SUCH_FILE:
    case COUCHSTORE_ERROR_DOC_NOT_FOUND:
        // this return causes ep engine to drop the failed flush
        // of an item since it does not know about the itme any longer
        return DOC_NOT_FOUND;
    default:
        // this return causes ep engine to keep requeuing the failed
        // flush of an item
        return MUTATION_FAILED;
    }
}

static bool allDigit(std::string &input) {
    size_t numchar = input.length();
    for(size_t i = 0; i < numchar; ++i) {
        if (!isdigit(input[i])) {
            return false;
        }
    }
    return true;
}

static std::string couchkvstore_strerrno(Db *db, couchstore_error_t err) {
    return (err == COUCHSTORE_ERROR_OPEN_FILE ||
            err == COUCHSTORE_ERROR_READ ||
            err == COUCHSTORE_ERROR_WRITE ||
            err == COUCHSTORE_ERROR_FILE_CLOSE) ? getStrError(db) : "none";
}

static DocKey makeDocKey(const sized_buf buf, bool restoreNamespace) {
    if (restoreNamespace) {
        return DocKey(reinterpret_cast<const uint8_t*>(&buf.buf[1]),
                      buf.size - 1,
                      DocNamespace(buf.buf[0]));
    } else {
        return DocKey(reinterpret_cast<const uint8_t*>(buf.buf),
                      buf.size,
                      DocNamespace::DefaultCollection);
    }
}

struct GetMultiCbCtx {
    GetMultiCbCtx(CouchKVStore &c, uint16_t v, vb_bgfetch_queue_t &f) :
        cks(c), vbId(v), fetches(f) {}

    CouchKVStore &cks;
    uint16_t vbId;
    vb_bgfetch_queue_t &fetches;
};

struct StatResponseCtx {
public:
    StatResponseCtx(std::map<std::pair<uint16_t, uint16_t>, vbucket_state> &sm,
                    uint16_t vb) : statMap(sm), vbId(vb) {
        /* EMPTY */
    }

    std::map<std::pair<uint16_t, uint16_t>, vbucket_state> &statMap;
    uint16_t vbId;
};

struct AllKeysCtx {
    AllKeysCtx(std::shared_ptr<Callback<const DocKey&>> callback, uint32_t cnt)
        : cb(callback), count(cnt) { }

    std::shared_ptr<Callback<const DocKey&>> cb;
    uint32_t count;
};

couchstore_content_meta_flags CouchRequest::getContentMeta(const Item& it) {
    couchstore_content_meta_flags rval;

    if (mcbp::datatype::is_json(it.getDataType())) {
        rval = COUCH_DOC_IS_JSON;
    } else {
        rval = COUCH_DOC_NON_JSON_MODE;
    }

    if (mcbp::datatype::is_snappy(it.getDataType())) {
        // We are currently using couchstore in a mode where it will try to
        // compress documents iff COUCH_DOC_IS_COMPRESSED is specified.
        // This would cause a conflict if we tried to store a document which
        // is already compressed.
        //
        // Luckily for us we don't support keeping objects compressed in
        // memory yet, so this won't happen. Let's throw an exception
        // to make sure that we fix this when we're going to support
        // compressed objects.
        throw std::logic_error(
            "CouchRequest::getContentMeta: can't store compressed items");
    }

    if (it.getNBytes() > 0) {
        // Don't try to compress empty bodies ;-)
        rval |= COUCH_DOC_IS_COMPRESSED;
    }

    return rval;
}

CouchRequest::CouchRequest(const Item& it,
                           uint64_t rev,
                           MutationRequestCallback& cb,
                           bool del,
                           bool persistDocNamespace)
    : IORequest(it.getVBucketId(), cb, del, it.getKey()),
      value(it.getValue()),
      fileRevNum(rev) {
    // Collections: TODO: Temporary switch to ensure upgrades don't break.
    if (persistDocNamespace) {
        dbDoc.id = {const_cast<char*>(reinterpret_cast<const char*>(
                            key.getDocNameSpacedData())),
                    it.getKey().getDocNameSpacedSize()};
    } else {
        dbDoc.id = {const_cast<char*>(key.c_str()), it.getKey().size()};
    }

    if (it.getNBytes()) {
        dbDoc.data.buf = const_cast<char *>(value->getData());
        dbDoc.data.size = it.getNBytes();
    } else {
        dbDoc.data.buf = NULL;
        dbDoc.data.size = 0;
    }
    meta.setCas(it.getCas());
    meta.setFlags(it.getFlags());
    if (del) {
        meta.setExptime(ep_real_time());
    } else {
        meta.setExptime(it.getExptime());
    }

    meta.setDataType(it.getDataType());

    dbDocInfo.db_seq = it.getBySeqno();

    // Now allocate space to hold the meta and get it ready for storage
    dbDocInfo.rev_meta.size = MetaData::getMetaDataSize(MetaData::Version::V1);
    dbDocInfo.rev_meta.buf = meta.prepareAndGetForPersistence();

    dbDocInfo.rev_seq = it.getRevSeqno();
    dbDocInfo.size = dbDoc.data.size;

    if (del) {
        dbDocInfo.deleted =  1;
    } else {
        dbDocInfo.deleted = 0;
    }
    dbDocInfo.id = dbDoc.id;
    dbDocInfo.content_meta = getContentMeta(it);
}

CouchKVStore::CouchKVStore(KVStoreConfig& config)
    : CouchKVStore(config, *couchstore_get_default_file_ops()) {
}

CouchKVStore::CouchKVStore(KVStoreConfig& config,
                           FileOpsInterface& ops,
                           bool readOnly,
                           std::vector<std::atomic<uint64_t>>& dbFileRevMap,
                           size_t fileRevMapSize)
    : KVStore(config, readOnly),
      dbname(config.getDBName()),
      dbFileRevMap(dbFileRevMap),
      fileRevMap(fileRevMapSize),
      intransaction(false),
      scanCounter(0),
      logger(config.getLogger()),
      base_ops(ops) {
    createDataDir(dbname);
    statCollectingFileOps = getCouchstoreStatsOps(st.fsStats, base_ops);
    statCollectingFileOpsCompaction = getCouchstoreStatsOps(
        st.fsStatsCompaction, base_ops);

    // init db file map with default revision number, 1
    numDbFiles = configuration.getMaxVBuckets();
    cachedVBStates.reserve(numDbFiles);

    // pre-allocate lookup maps (vectors) given we have a relatively
    // small, fixed number of vBuckets.
    cachedDocCount.assign(numDbFiles, Couchbase::RelaxedAtomic<size_t>(0));
    cachedDeleteCount.assign(numDbFiles, Couchbase::RelaxedAtomic<size_t>(-1));
    cachedFileSize.assign(numDbFiles, Couchbase::RelaxedAtomic<uint64_t>(0));
    cachedSpaceUsed.assign(numDbFiles, Couchbase::RelaxedAtomic<uint64_t>(0));
    cachedVBStates.assign(numDbFiles, nullptr);

    initialize();
}

CouchKVStore::CouchKVStore(KVStoreConfig& config, FileOpsInterface& ops)
    : CouchKVStore(config,
                   ops,
                   false /*readonly*/,
                   fileRevMap,
                   config.getMaxVBuckets()) {
}

CouchKVStore::CouchKVStore(const CouchKVStore& copyFrom)
    : KVStore(copyFrom),
      dbname(copyFrom.dbname),
      dbFileRevMap(copyFrom.dbFileRevMap),
      fileRevMap(copyFrom.fileRevMap.size()),
      numDbFiles(copyFrom.numDbFiles),
      intransaction(false),
      logger(copyFrom.logger),
      base_ops(copyFrom.base_ops) {
    // std::atomic needs to be manually copied (not copy constructor)
    for (size_t ii = 0; ii < fileRevMap.size(); ii++) {
        fileRevMap[ii].store(copyFrom.fileRevMap[ii].load());
    }
    createDataDir(dbname);
    statCollectingFileOps = getCouchstoreStatsOps(st.fsStats, base_ops);
    statCollectingFileOpsCompaction = getCouchstoreStatsOps(
        st.fsStatsCompaction, base_ops);
}

/**
 * Make a read-only CouchKVStore from this object
 */
std::unique_ptr<CouchKVStore> CouchKVStore::makeReadOnlyStore() {
    // Not using make_unique due to the private constructor we're calling
    return std::unique_ptr<CouchKVStore>(
            new CouchKVStore(configuration, fileRevMap));
}

CouchKVStore::CouchKVStore(KVStoreConfig& config,
                           std::vector<std::atomic<uint64_t>>& dbFileRevMap)
    : CouchKVStore(config,
                   *couchstore_get_default_file_ops(),
                   true /*readonly*/,
                   dbFileRevMap,
                   0) {
}

void CouchKVStore::initialize() {
    std::vector<uint16_t> vbids;
    std::vector<std::string> files;
    discoverDbFiles(dbname, files);
    populateFileNameMap(files, &vbids);

    Db *db = NULL;
    couchstore_error_t errorCode;

    std::vector<uint16_t>::iterator itr = vbids.begin();
    for (; itr != vbids.end(); ++itr) {
        uint16_t id = *itr;
        uint64_t rev = dbFileRevMap[id];

        errorCode = openDB(id, rev, &db, COUCHSTORE_OPEN_FLAG_RDONLY);
        if (errorCode == COUCHSTORE_SUCCESS) {
            readVBState(db, id);
            /* update stat */
            ++st.numLoadedVb;
            closeDatabaseHandle(db);
        } else {
            logger.log(EXTENSION_LOG_WARNING, "CouchKVStore::initialize: openDB"
                       " error:%s, name:%s/%" PRIu16 ".couch.%" PRIu64,
                       couchstore_strerror(errorCode), dbname.c_str(), id, rev);
            remVBucketFromDbFileMap(id);
            cachedVBStates[id] = NULL;
        }

        db = NULL;
        if (!isReadOnly()) {
            removeCompactFile(dbname, id, rev);
        }
    }
}

CouchKVStore::~CouchKVStore() {
    close();

    for (std::vector<vbucket_state *>::iterator it = cachedVBStates.begin();
         it != cachedVBStates.end(); it++) {
        vbucket_state *vbstate = *it;
        if (vbstate) {
            delete vbstate;
            *it = NULL;
        }
    }
}
void CouchKVStore::reset(uint16_t vbucketId) {
    if (isReadOnly()) {
        throw std::logic_error("CouchKVStore::reset: Not valid on a read-only "
                        "object.");
    }

    vbucket_state *state = cachedVBStates[vbucketId];
    if (state) {
        state->reset();

        cachedDocCount[vbucketId] = 0;
        cachedDeleteCount[vbucketId] = 0;
        cachedFileSize[vbucketId] = 0;
        cachedSpaceUsed[vbucketId] = 0;

        // Unlink the current revision and then increment it to ensure any
        // pending delete doesn't delete us. Note that the expectation is that
        // some higher level per VB lock is required to prevent data-races here.
        // KVBucket::vb_mutexes is used in this case.
        unlinkCouchFile(vbucketId, dbFileRevMap[vbucketId]);
        incrementRevision(vbucketId);

        setVBucketState(
                vbucketId, *state, VBStatePersist::VBSTATE_PERSIST_WITH_COMMIT);
    } else {
        throw std::invalid_argument("CouchKVStore::reset: No entry in cached "
                        "states for vbucket " + std::to_string(vbucketId));
    }
}

void CouchKVStore::set(const Item &itm, Callback<mutation_result> &cb) {
    if (isReadOnly()) {
        throw std::logic_error("CouchKVStore::set: Not valid on a read-only "
                        "object.");
    }
    if (!intransaction) {
        throw std::invalid_argument("CouchKVStore::set: intransaction must be "
                        "true to perform a set operation.");
    }

    bool deleteItem = false;
    MutationRequestCallback requestcb;
    uint64_t fileRev = dbFileRevMap[itm.getVBucketId()];

    // each req will be de-allocated after commit
    requestcb.setCb = &cb;
    CouchRequest* req =
            new CouchRequest(itm,
                             fileRev,
                             requestcb,
                             deleteItem,
                             configuration.shouldPersistDocNamespace());
    pendingReqsQ.push_back(req);
}

GetValue CouchKVStore::get(const DocKey& key, uint16_t vb, bool fetchDelete) {
    Db *db = NULL;
    uint64_t fileRev = dbFileRevMap[vb];
    couchstore_error_t errCode = openDB(vb, fileRev, &db,
                                        COUCHSTORE_OPEN_FLAG_RDONLY);
    if (errCode != COUCHSTORE_SUCCESS) {
        ++st.numGetFailure;
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::get: openDB error:%s, vb:%" PRIu16,
                   couchstore_strerror(errCode), vb);
        return GetValue(nullptr, couchErr2EngineErr(errCode));
    }

    GetValue gv = getWithHeader(db, key, vb, GetMetaOnly::No, fetchDelete);
    closeDatabaseHandle(db);
    return gv;
}

GetValue CouchKVStore::getWithHeader(void* dbHandle,
                                     const DocKey& key,
                                     uint16_t vb,
                                     GetMetaOnly getMetaOnly,
                                     bool fetchDelete) {
    Db *db = (Db *)dbHandle;
    hrtime_t start = gethrtime();
    DocInfo *docInfo = NULL;
    sized_buf id;
    GetValue rv;

    id.size = key.size();
    id.buf = const_cast<char*>(reinterpret_cast<const char*>(key.data()));

    couchstore_error_t errCode = couchstore_docinfo_by_id(db, (uint8_t *)id.buf,
                                                          id.size, &docInfo);
    if (errCode != COUCHSTORE_SUCCESS) {
        if (getMetaOnly == GetMetaOnly::No) {
            // log error only if this is non-xdcr case
            logger.log(EXTENSION_LOG_WARNING,
                       "CouchKVStore::getWithHeader: couchstore_docinfo_by_id "
                       "error:%s [%s], vb:%" PRIu16,
                       couchstore_strerror(errCode),
                       couchkvstore_strerrno(db, errCode).c_str(), vb);
        }
    } else {
        if (docInfo == nullptr) {
            throw std::logic_error("CouchKVStore::getWithHeader: "
                    "couchstore_docinfo_by_id returned success but docInfo "
                    "is NULL");
        }
        errCode = fetchDoc(db, docInfo, rv, vb, getMetaOnly);
        if (errCode != COUCHSTORE_SUCCESS) {
            logger.log(EXTENSION_LOG_WARNING,
                       "CouchKVStore::getWithHeader: fetchDoc error:%s [%s],"
                       " vb:%" PRIu16 ", deleted:%s",
                       couchstore_strerror(errCode),
                       couchkvstore_strerrno(db, errCode).c_str(), vb,
                       docInfo->deleted ? "yes" : "no");
        }

        // record stats
        st.readTimeHisto.add((gethrtime() - start) / 1000);
        if (errCode == COUCHSTORE_SUCCESS) {
            st.readSizeHisto.add(key.size() + rv.item->getNBytes());
        }
    }

    if(errCode != COUCHSTORE_SUCCESS) {
        ++st.numGetFailure;
    }

    couchstore_free_docinfo(docInfo);
    rv.setStatus(couchErr2EngineErr(errCode));
    return rv;
}

void CouchKVStore::getMulti(uint16_t vb, vb_bgfetch_queue_t &itms) {
    int numItems = itms.size();
    uint64_t fileRev = dbFileRevMap[vb];

    Db *db = NULL;
    couchstore_error_t errCode = openDB(vb, fileRev, &db,
                                        COUCHSTORE_OPEN_FLAG_RDONLY);
    if (errCode != COUCHSTORE_SUCCESS) {
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::getMulti: openDB error:%s, "
                   "vb:%" PRIu16 ", numDocs:%d",
                   couchstore_strerror(errCode), vb, numItems);
        st.numGetFailure += numItems;
        for (auto& item : itms) {
            item.second.value.setStatus(ENGINE_NOT_MY_VBUCKET);
        }
        return;
    }

    size_t idx = 0;
    sized_buf *ids = new sized_buf[itms.size()];
    for (auto& item : itms) {
        if (configuration.shouldPersistDocNamespace()) {
            ids[idx] = {const_cast<char*>(reinterpret_cast<const char*>(
                                item.first.getDocNameSpacedData())),
                        item.first.getDocNameSpacedSize()};
        } else {
            ids[idx] = {const_cast<char*>(reinterpret_cast<const char*>(
                                item.first.data())),
                        item.first.size()};
        }

        ++idx;
    }

    GetMultiCbCtx ctx(*this, vb, itms);

    errCode = couchstore_docinfos_by_id(db, ids, itms.size(),
                                        0, getMultiCbC, &ctx);
    if (errCode != COUCHSTORE_SUCCESS) {
        st.numGetFailure += numItems;
        logger.log(EXTENSION_LOG_WARNING, "CouchKVStore::getMulti: "
                   "couchstore_docinfos_by_id error %s [%s], vb:%" PRIu16,
                   couchstore_strerror(errCode),
                   couchkvstore_strerrno(db, errCode).c_str(), vb);
        for (auto& item : itms) {
            item.second.value.setStatus(couchErr2EngineErr(errCode));
        }
    }
    closeDatabaseHandle(db);
    delete []ids;
}

void CouchKVStore::del(const Item &itm,
                       Callback<int> &cb) {
    if (isReadOnly()) {
        throw std::logic_error("CouchKVStore::del: Not valid on a read-only "
                        "object.");
    }
    if (!intransaction) {
        throw std::invalid_argument("CouchKVStore::del: intransaction must be "
                        "true to perform a delete operation.");
    }

    uint64_t fileRev = dbFileRevMap[itm.getVBucketId()];
    MutationRequestCallback requestcb;
    requestcb.delCb = &cb;
    CouchRequest* req =
            new CouchRequest(itm,
                             fileRev,
                             requestcb,
                             true,
                             configuration.shouldPersistDocNamespace());
    pendingReqsQ.push_back(req);
}

void CouchKVStore::delVBucket(uint16_t vbucket, uint64_t fileRev) {
    if (isReadOnly()) {
        throw std::logic_error("CouchKVStore::delVBucket: Not valid on a "
                        "read-only object.");
    }

    unlinkCouchFile(vbucket, fileRev);
}

std::vector<vbucket_state *> CouchKVStore::listPersistedVbuckets() {
    return cachedVBStates;
}

void CouchKVStore::getPersistedStats(std::map<std::string,
                                     std::string> &stats) {
    char *buffer = NULL;
    std::string fname = dbname + "/stats.json";
    if (access(fname.c_str(), R_OK) == -1) {
        return ;
    }

    std::ifstream session_stats;
    session_stats.exceptions (session_stats.failbit | session_stats.badbit);
    try {
        session_stats.open(fname.c_str(), std::ios::binary);
        session_stats.seekg(0, std::ios::end);
        int flen = session_stats.tellg();
        if (flen < 0) {
            logger.log(EXTENSION_LOG_WARNING, "CouchKVStore::getPersistedStats:"
                       " Error in session stats ifstream!!!");
            session_stats.close();
            return;
        }
        session_stats.seekg(0, std::ios::beg);
        buffer = new char[flen + 1];
        session_stats.read(buffer, flen);
        session_stats.close();
        buffer[flen] = '\0';

        cJSON *json_obj = cJSON_Parse(buffer);
        if (!json_obj) {
            logger.log(EXTENSION_LOG_WARNING, "CouchKVStore::getPersistedStats:"
                       " Failed to parse the session stats json doc!!!");
            delete[] buffer;
            return;
        }

        int json_arr_size = cJSON_GetArraySize(json_obj);
        for (int i = 0; i < json_arr_size; ++i) {
            cJSON *obj = cJSON_GetArrayItem(json_obj, i);
            if (obj) {
                stats[obj->string] = obj->valuestring ? obj->valuestring : "";
            }
        }
        cJSON_Delete(json_obj);

    } catch (const std::ifstream::failure &e) {
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::getPersistedStats: Failed to load the engine "
                   "session stats due to IO exception \"%s\"", e.what());
    } catch (...) {
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::getPersistedStats: Failed to load the engine "
                   "session stats due to IO exception");
    }

    delete[] buffer;
}

static std::string getDBFileName(const std::string &dbname,
                                 uint16_t vbid,
                                 uint64_t rev) {
    return dbname + "/" + std::to_string(vbid) + ".couch." +
           std::to_string(rev);
}

static int edit_docinfo_hook(DocInfo **info, const sized_buf *item) {
    // Examine the metadata of the doc
    auto documentMetaData = MetaDataFactory::createMetaData((*info)->rev_meta);
    // Allocate latest metadata
    std::unique_ptr<MetaData> metadata;
    if (documentMetaData->getVersionInitialisedFrom() == MetaData::Version::V0) {
        // Metadata doesn't have flex_meta_code/datatype. Provision space for
        // these paramenters.

        // If the document is compressed we need to inflate it to
        // determine if it is json or not.
        cb::compression::Buffer inflated;
        cb::const_char_buffer data { item->buf, item->size };

        if (((*info)->content_meta | COUCH_DOC_IS_COMPRESSED) ==
                (*info)->content_meta) {
            if (!cb::compression::inflate(cb::compression::Algorithm::Snappy,
                                          item->buf, item->size, inflated)) {
                throw std::runtime_error(
                    "edit_docinfo_hook: failed to inflate document with seqno: " +
                    std::to_string((*info)->db_seq) + " revno: " +
                    std::to_string((*info)->rev_seq));
            }
            data = { inflated.data.get(), inflated.len };
        }

        protocol_binary_datatype_t datatype = PROTOCOL_BINARY_RAW_BYTES;
        if (checkUTF8JSON(reinterpret_cast<const uint8_t*>(data.data()),
                          data.size())) {
            datatype = PROTOCOL_BINARY_DATATYPE_JSON;
        }

        // Now create a blank latest metadata.
        metadata = MetaDataFactory::createMetaData();
        // Copy the metadata this will pull across available V0 fields.
        *metadata = *documentMetaData;

        // Setup flex code and datatype
        metadata->setFlexCode();
        metadata->setDataType(datatype);
    } else {
        // The metadata in the document is V1 and needs no changes.
        return 0;
    }

    // the docInfo pointer includes the DocInfo and the data it points to.
    // this must be a pointer which cb_free() can deallocate
    char* buffer = static_cast<char*>(cb_calloc(1, sizeof(DocInfo) +
                             (*info)->id.size +
                             MetaData::getMetaDataSize(MetaData::Version::V1)));


    DocInfo* docInfo = reinterpret_cast<DocInfo*>(buffer);

    // Deep-copy the incoming DocInfo, then we'll fix the pointers/buffer data
    *docInfo = **info;

    // Correct the id buffer
    docInfo->id.buf = buffer + sizeof(DocInfo);
    std::memcpy(docInfo->id.buf, (*info)->id.buf, docInfo->id.size);

    // Correct the rev_meta pointer and fill it in.
    docInfo->rev_meta.size = MetaData::getMetaDataSize(MetaData::Version::V1);
    docInfo->rev_meta.buf = buffer + sizeof(DocInfo) + docInfo->id.size;
    metadata->copyToBuf(docInfo->rev_meta);

    // Free the orginal
    couchstore_free_docinfo(*info);

    // Return the newly allocated docinfo with corrected metadata
    *info = docInfo;

    return 1;
}

/**
 * Notify the expiry callback that a document has expired
 *
 * @param info     document information for the expired item
 * @param metadata metadata of the document
 * @param item     buffer containing data and size
 * @param ctx      context for compaction
 * @param currtime current time
 */
static int notify_expired_item(DocInfo& info,
                               MetaData& metadata,
                               sized_buf item,
                               compaction_ctx& ctx,
                               time_t currtime) {
    std::array<uint8_t, 1> ext_meta = {{metadata.getDataType()}};
    cb::char_buffer data;
    cb::compression::Buffer inflated;

    if (mcbp::datatype::is_xattr(metadata.getDataType())) {
        if (item.buf == nullptr) {
            // We need to pass on the entire document to the callback
            return COUCHSTORE_COMPACT_NEED_BODY;
        }

        if (info.content_meta | COUCH_DOC_IS_COMPRESSED) {
            using namespace cb::compression;

            if (!inflate(Algorithm::Snappy,
                         item.buf, item.size, inflated)) {
                LOG(EXTENSION_LOG_WARNING,
                    "time_purge_hook: failed to inflate document with seqno %" PRIu64 ""
                    "revno: %" PRIu64, info.db_seq, info.rev_seq);
                return COUCHSTORE_ERROR_CORRUPT;
            }
            data = {inflated.data.get(),inflated.len};
        }
    }

    // Collections: TODO: Restore to stored namespace
    Item it(makeDocKey(info.id, ctx.config->shouldPersistDocNamespace()),
            metadata.getFlags(),
            metadata.getExptime(),
            data.buf,
            data.len,
            &ext_meta[0],
            EXT_META_LEN,
            metadata.getCas(),
            info.db_seq,
            ctx.db_file_id,
            info.rev_seq);

    it.setRevSeqno(info.rev_seq);
    ctx.expiryCallback->callback(it, currtime);

    return COUCHSTORE_SUCCESS;
}

static int time_purge_hook(Db* d, DocInfo* info, sized_buf item, void* ctx_p) {
    compaction_ctx* ctx = static_cast<compaction_ctx*>(ctx_p);
    const uint16_t vbid = ctx->db_file_id;

    if (info == nullptr) {
        // Compaction finished
        return couchstore_set_purge_seq(d, ctx->max_purged_seq[vbid]);
    }

    DbInfo infoDb;
    auto err = couchstore_db_info(d, &infoDb);
    if (err != COUCHSTORE_SUCCESS) {
        LOG(EXTENSION_LOG_WARNING,
            "time_purge_hook: couchstore_db_info() failed: %s",
            couchstore_strerror(err));
        return err;
    }

    uint64_t max_purge_seq = 0;
    auto it = ctx->max_purged_seq.find(vbid);

    if (it == ctx->max_purged_seq.end()) {
        ctx->max_purged_seq[vbid] = 0;
    } else {
        max_purge_seq = it->second;
    }

    if (info->rev_meta.size >= MetaData::getMetaDataSize(MetaData::Version::V0)) {
        auto metadata = MetaDataFactory::createMetaData(info->rev_meta);
        uint32_t exptime = metadata->getExptime();
        if (info->deleted) {
            if (info->db_seq != infoDb.last_sequence) {
                if (ctx->drop_deletes) { // all deleted items must be dropped ...
                    if (max_purge_seq < info->db_seq) {
                        ctx->max_purged_seq[vbid] = info->db_seq; // track max_purged_seq
                    }
                    return COUCHSTORE_COMPACT_DROP_ITEM;      // ...unconditionally
                }
                if (exptime < ctx->purge_before_ts &&
                        (!ctx->purge_before_seq ||
                         info->db_seq <= ctx->purge_before_seq)) {
                    if (max_purge_seq < info->db_seq) {
                        ctx->max_purged_seq[vbid] = info->db_seq;
                    }
                    return COUCHSTORE_COMPACT_DROP_ITEM;
                }
            }
        } else {
            time_t currtime = ep_real_time();
            if (exptime && exptime < currtime) {
                int ret;
                try {
                    ret = notify_expired_item(*info, *metadata, item,
                                             *ctx, currtime);
                } catch (const std::bad_alloc&) {
                    LOG(EXTENSION_LOG_WARNING,
                        "time_purge_hook: memory allocation failed");
                    return COUCHSTORE_ERROR_ALLOC_FAIL;
                }

                if (ret != COUCHSTORE_SUCCESS) {
                    return ret;
                }
            }
        }
    }

    if (ctx->bloomFilterCallback) {
        bool deleted = info->deleted;
        // Collections: TODO: Permanently restore to stored namespace
        DocKey key = makeDocKey(
                info->id, ctx->config->shouldPersistDocNamespace());
        ctx->bloomFilterCallback->callback(ctx->db_file_id, key, deleted);
    }

    return COUCHSTORE_COMPACT_KEEP_ITEM;
}

bool CouchKVStore::compactDB(compaction_ctx *hook_ctx) {
    return compactDBInternal(hook_ctx, edit_docinfo_hook);
}

bool CouchKVStore::compactDBInternal(compaction_ctx* hook_ctx,
                                     couchstore_docinfo_hook docinfo_hook) {
    if (isReadOnly()) {
        throw std::logic_error("CouchKVStore::compactDB: Cannot perform "
                        "on a read-only instance.");
    }
    TRACE_EVENT("ep-engine/couch-kvstore", "compactDB",
                this->configuration.getShardId());

    couchstore_compact_hook       hook = time_purge_hook;
    couchstore_docinfo_hook dhook = docinfo_hook;
    FileOpsInterface         *def_iops = statCollectingFileOpsCompaction.get();
    Db                      *compactdb = NULL;
    Db                       *targetDb = NULL;
    couchstore_error_t         errCode = COUCHSTORE_SUCCESS;
    hrtime_t                     start = gethrtime();
    std::string                 dbfile;
    std::string           compact_file;
    std::string               new_file;
    kvstats_ctx kvctx(configuration);
    DbInfo                        info;
    uint16_t                      vbid = hook_ctx->db_file_id;
    uint64_t                   fileRev = dbFileRevMap[vbid];
    uint64_t                   new_rev = fileRev + 1;
    hook_ctx->config = &configuration;

    // Open the source VBucket database file ...
    errCode = openDB(vbid,
                     fileRev,
                     &compactdb,
                     (uint64_t)COUCHSTORE_OPEN_FLAG_RDONLY,
                     def_iops);
    if (errCode != COUCHSTORE_SUCCESS) {
        logger.log(EXTENSION_LOG_WARNING,
                       "CouchKVStore::compactDB openDB error:%s, vb:%" PRIu16
                       ", fileRev:%" PRIu64, couchstore_strerror(errCode),
                       vbid, fileRev);
        return false;
    }

    // Build the temporary vbucket.compact file name
    dbfile       = getDBFileName(dbname, vbid, fileRev);
    compact_file = dbfile + ".compact";

    couchstore_open_flags flags(COUCHSTORE_COMPACT_FLAG_UPGRADE_DB);

    /**
     * This flag disables IO buffering in couchstore which means
     * file operations will trigger syscalls immediately. This has
     * a detrimental impact on performance and is only intended
     * for testing.
     */
    if(!configuration.getBuffered()) {
        flags |= COUCHSTORE_OPEN_FLAG_UNBUFFERED;
    }

    // Should automatic fsync() be configured for compaction?
    const auto periodicSyncBytes = configuration.getPeriodicSyncBytes();
    if (periodicSyncBytes != 0) {
        flags |= couchstore_encode_periodic_sync_flags(periodicSyncBytes);
    }

    // Perform COMPACTION of vbucket.couch.rev into vbucket.couch.rev.compact
    errCode = couchstore_compact_db_ex(compactdb, compact_file.c_str(), flags,
                                       hook, dhook, hook_ctx, def_iops);
    if (errCode != COUCHSTORE_SUCCESS) {
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::compactDB:couchstore_compact_db_ex "
                   "error:%s [%s], name:%s",
                   couchstore_strerror(errCode),
                   couchkvstore_strerrno(compactdb, errCode).c_str(),
                   dbfile.c_str());
        closeDatabaseHandle(compactdb);
        return false;
    }

    // Close the source Database File once compaction is done
    closeDatabaseHandle(compactdb);

    // Rename the .compact file to one with the next revision number
    new_file = getDBFileName(dbname, vbid, new_rev);
    if (rename(compact_file.c_str(), new_file.c_str()) != 0) {
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::compactDB: rename error:%s, old:%s, new:%s",
                   cb_strerror().c_str(), compact_file.c_str(), new_file.c_str());

        removeCompactFile(compact_file);
        return false;
    }

    // Open the newly compacted VBucket database file ...
    errCode = openDB(
            vbid, new_rev, &targetDb, (uint64_t)COUCHSTORE_OPEN_FLAG_RDONLY);
    if (errCode != COUCHSTORE_SUCCESS) {
        logger.log(EXTENSION_LOG_WARNING,
                       "CouchKVStore::compactDB: openDB#2 error:%s, file:%s, "
                       "fileRev:%" PRIu64, couchstore_strerror(errCode),
                       new_file.c_str(), new_rev);
        if (remove(new_file.c_str()) != 0) {
            logger.log(EXTENSION_LOG_WARNING,
                       "CouchKVStore::compactDB: remove error:%s, path:%s",
                       cb_strerror().c_str(), new_file.c_str());
        }
        return false;
    }

    // Update the global VBucket file map so all operations use the new file
    updateDbFileMap(vbid, new_rev);

    logger.log(EXTENSION_LOG_INFO,
               "INFO: created new couch db file, name:%s rev:%" PRIu64,
               new_file.c_str(), new_rev);

    couchstore_db_info(targetDb, &info);

    cachedFileSize[vbid] = info.file_size;
    cachedSpaceUsed[vbid] = info.space_used;

    // also update cached state with dbinfo
    vbucket_state *state = cachedVBStates[vbid];
    if (state) {
        state->highSeqno = info.last_sequence;
        state->purgeSeqno = info.purge_seq;
        cachedDeleteCount[vbid] = info.deleted_count;
        cachedDocCount[vbid] = info.doc_count;
    }

    closeDatabaseHandle(targetDb);

    // Removing the stale couch file
    unlinkCouchFile(vbid, fileRev);

    st.compactHisto.add((gethrtime() - start) / 1000);

    return true;
}

vbucket_state * CouchKVStore::getVBucketState(uint16_t vbucketId) {
    return cachedVBStates[vbucketId];
}

bool CouchKVStore::setVBucketState(uint16_t vbucketId,
                                   const vbucket_state& vbstate,
                                   VBStatePersist options) {
    Db *db = NULL;
    uint64_t fileRev;
    std::map<uint16_t, uint64_t>::iterator mapItr;
    kvstats_ctx kvctx(configuration);
    kvctx.vbucket = vbucketId;
    couchstore_error_t errorCode;

    if (options == VBStatePersist::VBSTATE_PERSIST_WITHOUT_COMMIT ||
            options == VBStatePersist::VBSTATE_PERSIST_WITH_COMMIT) {
        fileRev = dbFileRevMap[vbucketId];
        errorCode = openDB(
                vbucketId, fileRev, &db, (uint64_t)COUCHSTORE_OPEN_FLAG_CREATE);
        if (errorCode != COUCHSTORE_SUCCESS) {
            ++st.numVbSetFailure;
            logger.log(EXTENSION_LOG_WARNING,
                       "CouchKVStore::setVBucketState: openDB error:%s, "
                       "vb:%" PRIu16 ", fileRev:%" PRIu64,
                       couchstore_strerror(errorCode),
                       vbucketId,
                       fileRev);
            return false;
        }

        errorCode = saveVBState(db, vbstate);
        if (errorCode != COUCHSTORE_SUCCESS) {
            ++st.numVbSetFailure;
            logger.log(EXTENSION_LOG_WARNING,
                       "CouchKVStore:setVBucketState: saveVBState error:%s, "
                       "vb:%" PRIu16 ", fileRev:%" PRIu64,
                       couchstore_strerror(errorCode),
                       vbucketId,
                       fileRev);
            closeDatabaseHandle(db);
            return false;
        }

        if (options == VBStatePersist::VBSTATE_PERSIST_WITH_COMMIT) {
            errorCode = couchstore_commit(db);
            if (errorCode != COUCHSTORE_SUCCESS) {
                ++st.numVbSetFailure;
                logger.log(EXTENSION_LOG_WARNING,
                           "CouchKVStore:setVBucketState: couchstore_commit "
                           "error:%s [%s], vb:%" PRIu16 ", rev:%" PRIu64,
                           couchstore_strerror(errorCode),
                           couchkvstore_strerrno(db, errorCode).c_str(),
                           vbucketId, fileRev);
                closeDatabaseHandle(db);
                return false;
            }
        }

        DbInfo info;
        errorCode = couchstore_db_info(db, &info);
        if (errorCode != COUCHSTORE_SUCCESS) {
            logger.log(EXTENSION_LOG_WARNING,
                       "CouchKVStore::setVBucketState: couchstore_db_info "
                       "error:%s, vb:%" PRIu16, couchstore_strerror(errorCode),
                       vbucketId);
        } else {
            cachedSpaceUsed[vbucketId] = info.space_used;
            cachedFileSize[vbucketId] = info.file_size;
        }

        closeDatabaseHandle(db);
    } else {
        throw std::invalid_argument("CouchKVStore::setVBucketState: invalid vb state "
                        "persist option specified for vbucket id:" +
                        std::to_string(vbucketId));
    }

    return true;
}

bool CouchKVStore::snapshotVBucket(uint16_t vbucketId,
                                   const vbucket_state &vbstate,
                                   VBStatePersist options) {
    if (isReadOnly()) {
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::snapshotVBucket: cannot be performed on a "
                   "read-only KVStore instance");
        return false;
    }

    hrtime_t start = gethrtime();

    if (updateCachedVBState(vbucketId, vbstate) &&
         (options == VBStatePersist::VBSTATE_PERSIST_WITHOUT_COMMIT ||
          options == VBStatePersist::VBSTATE_PERSIST_WITH_COMMIT)) {
        vbucket_state *vbs = cachedVBStates[vbucketId];
        if (!setVBucketState(vbucketId, *vbs, options)) {
            logger.log(EXTENSION_LOG_WARNING,
                       "CouchKVStore::snapshotVBucket: setVBucketState failed "
                       "state:%s, vb:%" PRIu16,
                       VBucket::toString(vbstate.state), vbucketId);
            return false;
        }
    }

    LOG(EXTENSION_LOG_DEBUG,
        "CouchKVStore::snapshotVBucket: Snapshotted vbucket:%" PRIu16 " state:%s",
        vbucketId,
        vbstate.toJSON().c_str());

    st.snapshotHisto.add((gethrtime() - start) / 1000);

    return true;
}

StorageProperties CouchKVStore::getStorageProperties() {
    StorageProperties rv(StorageProperties::EfficientVBDump::Yes,
                         StorageProperties::EfficientVBDeletion::Yes,
                         StorageProperties::PersistedDeletion::Yes,
                         StorageProperties::EfficientGet::Yes,
                         StorageProperties::ConcurrentWriteCompact::No);
    return rv;
}

bool CouchKVStore::commit(const Item* collectionsManifest) {
    TRACE_EVENT("ep-engine/couch-kvstore", "commit",
                this->configuration.getShardId());

    if (isReadOnly()) {
        throw std::logic_error("CouchKVStore::commit: Not valid on a read-only "
                        "object.");
    }

    if (intransaction) {
        if (commit2couchstore(collectionsManifest)) {
            intransaction = false;
        }
    }

    return !intransaction;
}

bool CouchKVStore::getStat(const char* name, size_t& value)  {
    if (strcmp("io_total_read_bytes", name) == 0) {
        value = st.fsStats.totalBytesRead.load() +
                st.fsStatsCompaction.totalBytesRead.load();
        return true;
    } else if (strcmp("io_total_write_bytes", name) == 0) {
        value = st.fsStats.totalBytesWritten.load() +
                st.fsStatsCompaction.totalBytesWritten.load();
        return true;
    } else if (strcmp("io_compaction_read_bytes", name) == 0) {
        value = st.fsStatsCompaction.totalBytesRead;
        return true;
    } else if (strcmp("io_compaction_write_bytes", name) == 0) {
        value = st.fsStatsCompaction.totalBytesWritten;
        return true;
    }

    return false;
}

void CouchKVStore::pendingTasks() {
    if (isReadOnly()) {
        throw std::logic_error("CouchKVStore::pendingTasks: Not valid on a "
                        "read-only object.");
    }

    if (!pendingFileDeletions.empty()) {
        std::queue<std::string> queue;
        pendingFileDeletions.getAll(queue);

        while (!queue.empty()) {
            std::string filename_str = queue.front();
            if (remove(filename_str.c_str()) == -1) {
                logger.log(EXTENSION_LOG_WARNING, "CouchKVStore::pendingTasks: "
                           "remove error:%d, file%s", errno,
                           filename_str.c_str());
                if (errno != ENOENT) {
                    pendingFileDeletions.push(filename_str);
                }
            }
            queue.pop();
        }
    }
}

ScanContext* CouchKVStore::initScanContext(std::shared_ptr<Callback<GetValue> > cb,
                                           std::shared_ptr<Callback<CacheLookup> > cl,
                                           uint16_t vbid, uint64_t startSeqno,
                                           DocumentFilter options,
                                           ValueFilter valOptions) {
    Db *db = NULL;
    uint64_t rev = dbFileRevMap[vbid];
    couchstore_error_t errorCode = openDB(vbid, rev, &db,
                                          COUCHSTORE_OPEN_FLAG_RDONLY);
    if (errorCode != COUCHSTORE_SUCCESS) {
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::initScanContext: openDB error:%s, "
                   "name:%s/%" PRIu16 ".couch.%" PRIu64,
                   couchstore_strerror(errorCode), dbname.c_str(), vbid, rev);
        remVBucketFromDbFileMap(vbid);
        return NULL;
    }

    DbInfo info;
    errorCode = couchstore_db_info(db, &info);
    if (errorCode != COUCHSTORE_SUCCESS) {
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::initScanContext: couchstore_db_info error:%s",
                   couchstore_strerror(errorCode));
        closeDatabaseHandle(db);
        LOG(EXTENSION_LOG_WARNING,
            "CouchKVStore::initScanContext: Failed to read DB info for "
            "backfill. vb:%" PRIu16 " rev:%" PRIu64 " error: %s",
            vbid,
            rev,
            couchstore_strerror(errorCode));
        return NULL;
    }

    uint64_t count = 0;
    errorCode = couchstore_changes_count(db,
                                         startSeqno,
                                         std::numeric_limits<uint64_t>::max(),
                                         &count);
    if (errorCode != COUCHSTORE_SUCCESS) {
        closeDatabaseHandle(db);
        LOG(EXTENSION_LOG_WARNING,
            "CouchKVStore::initScanContext:Failed to obtain changes "
            "count for vb:%" PRIu16 " rev:%" PRIu64 " start_seqno:%" PRIu64
            " error: %s",
            vbid,
            rev,
            startSeqno,
            couchstore_strerror(errorCode));
        return NULL;
    }

    size_t scanId = scanCounter++;

    {
        LockHolder lh(scanLock);
        scans[scanId] = db;
    }

    ScanContext* sctx = new ScanContext(cb,
                                        cl,
                                        vbid,
                                        scanId,
                                        startSeqno,
                                        info.last_sequence,
                                        options,
                                        valOptions,
                                        count,
                                        configuration);
    sctx->logger = &logger;
    return sctx;
}

static couchstore_docinfos_options getDocFilter(const DocumentFilter& filter) {
    switch (filter) {
    case DocumentFilter::ALL_ITEMS:
        return COUCHSTORE_NO_OPTIONS;
    case DocumentFilter::NO_DELETES:
        return COUCHSTORE_NO_DELETES;
    }

    std::string err("getDocFilter: Illegal document filter!" +
                    std::to_string(static_cast<int>(filter)));
    throw std::runtime_error(err);
}

scan_error_t CouchKVStore::scan(ScanContext* ctx) {
    if (!ctx) {
        return scan_failed;
    }

    if (ctx->lastReadSeqno == ctx->maxSeqno) {
        return scan_success;
    }

    Db* db;
    {
        LockHolder lh(scanLock);
        auto itr = scans.find(ctx->scanId);
        if (itr == scans.end()) {
            return scan_failed;
        }

        db = itr->second;
    }

    uint64_t start = ctx->startSeqno;
    if (ctx->lastReadSeqno != 0) {
        start = ctx->lastReadSeqno + 1;
    }

    couchstore_error_t errorCode;
    errorCode = couchstore_changes_since(db,
                                         start,
                                         getDocFilter(ctx->docFilter),
                                         recordDbDumpC,
                                         static_cast<void*>(ctx));
    if (errorCode != COUCHSTORE_SUCCESS) {
        if (errorCode == COUCHSTORE_ERROR_CANCEL) {
            return scan_again;
        } else {
            logger.log(EXTENSION_LOG_WARNING,
                       "CouchKVStore::scan couchstore_changes_since "
                       "error:%s [%s]", couchstore_strerror(errorCode),
                       couchkvstore_strerrno(db, errorCode).c_str());
            remVBucketFromDbFileMap(ctx->vbid);
            return scan_failed;
        }
    }
    return scan_success;
}

void CouchKVStore::destroyScanContext(ScanContext* ctx) {
    if (!ctx) {
        return;
    }

    LockHolder lh(scanLock);
    auto itr = scans.find(ctx->scanId);
    if (itr != scans.end()) {
        closeDatabaseHandle(itr->second);
        scans.erase(itr);
    }
    delete ctx;
}

DbInfo CouchKVStore::getDbInfo(uint16_t vbid) {
    Db *db = nullptr;
    const uint64_t rev = dbFileRevMap.at(vbid);
    couchstore_error_t errCode = openDB(vbid, rev, &db,
                                        COUCHSTORE_OPEN_FLAG_RDONLY);
    if (errCode == COUCHSTORE_SUCCESS) {
        DbInfo info;
        errCode = couchstore_db_info(db, &info);
        closeDatabaseHandle(db);
        if (errCode == COUCHSTORE_SUCCESS) {
            return info;
        } else {
            throw std::runtime_error("CouchKVStore::getDbInfo: failed "
                    "to read database info for vBucket " + std::to_string(vbid) +
                    " revision " + std::to_string(rev) +
                    " - couchstore returned error: " +
                    couchstore_strerror(errCode));
        }
    } else {
        // open failed - map couchstore error code to exception.
        std::errc ec;
        switch (errCode) {
            case COUCHSTORE_ERROR_OPEN_FILE:
                ec = std::errc::no_such_file_or_directory; break;
            default:
                ec = std::errc::io_error; break;
        }
        throw std::system_error(std::make_error_code(ec),
                                "CouchKVStore::getDbInfo: failed to open database file for "
                                "vBucket = " + std::to_string(vbid) +
                                " rev = " + std::to_string(rev) +
                                " with error:" + couchstore_strerror(errCode));
    }
}

void CouchKVStore::close() {
    intransaction = false;
}

uint64_t CouchKVStore::checkNewRevNum(std::string &dbFileName, bool newFile) {
    uint64_t newrev = 0;
    std::string nameKey;

    if (!newFile) {
        // extract out the file revision number first
        size_t secondDot = dbFileName.rfind(".");
        nameKey = dbFileName.substr(0, secondDot);
    } else {
        nameKey = dbFileName;
    }
    nameKey.append(".");
    const auto files = cb::io::findFilesWithPrefix(nameKey);
    std::vector<std::string>::const_iterator itor;
    // found file(s) whoes name has the same key name pair with different
    // revision number
    for (itor = files.begin(); itor != files.end(); ++itor) {
        const std::string &filename = *itor;
        if (endWithCompact(filename)) {
            continue;
        }

        size_t secondDot = filename.rfind(".");
        char *ptr = NULL;
        uint64_t revnum = strtoull(filename.substr(secondDot + 1).c_str(), &ptr, 10);
        if (newrev < revnum) {
            newrev = revnum;
            dbFileName = filename;
        }
    }
    return newrev;
}

void CouchKVStore::updateDbFileMap(uint16_t vbucketId, uint64_t newFileRev) {
    if (vbucketId >= numDbFiles) {
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::updateDbFileMap: Cannot update db file map "
                   "for an invalid vbucket, vb:%" PRIu16", rev:%" PRIu64,
                   vbucketId, newFileRev);
        return;
    }

    dbFileRevMap[vbucketId] = newFileRev;
}

couchstore_error_t CouchKVStore::openDB(uint16_t vbucketId,
                                        uint64_t fileRev,
                                        Db** db,
                                        couchstore_open_flags options,
                                        FileOpsInterface* ops) {
    std::string dbFileName = getDBFileName(dbname, vbucketId, fileRev);

    if(ops == nullptr) {
        ops = statCollectingFileOps.get();
    }

    couchstore_error_t errorCode = COUCHSTORE_SUCCESS;

    /**
     * This flag disables IO buffering in couchstore which means
     * file operations will trigger syscalls immediately. This has
     * a detrimental impact on performance and is only intended
     * for testing.
     */
    if(!configuration.getBuffered()) {
        options |= COUCHSTORE_OPEN_FLAG_UNBUFFERED;
    }

    /* get the flags that determine the tracing and validation of
     *  couchstore file operations
     */
    if (configuration.getCouchstoreTracingEnabled()) {
        options |= COUCHSTORE_OPEN_WITH_TRACING;
    }
    if (configuration.getCouchstoreWriteValidationEnabled()) {
        options |= COUCHSTORE_OPEN_WITH_WRITE_VALIDATION;
    }
    if (configuration.getCouchstoreMprotectEnabled()) {
        options |= COUCHSTORE_OPEN_WITH_MPROTECT;
    }

    errorCode = couchstore_open_db_ex(dbFileName.c_str(), options, ops, db);

    /* update command statistics */
    st.numOpen++;
    if (errorCode) {
        st.numOpenFailure++;
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::openDB: error:%s [%s],"
                   " name:%s, option:%" PRIX64 ", fileRev:%" PRIu64,
                   couchstore_strerror(errorCode),
                   cb_strerror().c_str(),
                   dbFileName.c_str(),
                   options,
                   fileRev);
    }

    return errorCode;
}

void CouchKVStore::populateFileNameMap(std::vector<std::string> &filenames,
                                       std::vector<uint16_t> *vbids) {
    std::vector<std::string>::iterator fileItr;

    for (fileItr = filenames.begin(); fileItr != filenames.end(); ++fileItr) {
        const std::string &filename = *fileItr;
        size_t secondDot = filename.rfind(".");
        std::string nameKey = filename.substr(0, secondDot);
        size_t firstDot = nameKey.rfind(".");
#ifdef _MSC_VER
        size_t firstSlash = nameKey.rfind("\\");
#else
        size_t firstSlash = nameKey.rfind("/");
#endif

        std::string revNumStr = filename.substr(secondDot + 1);
        char *ptr = NULL;
        uint64_t revNum = strtoull(revNumStr.c_str(), &ptr, 10);

        std::string vbIdStr = nameKey.substr(firstSlash + 1,
                                            (firstDot - firstSlash) - 1);
        if (allDigit(vbIdStr)) {
            int vbId = atoi(vbIdStr.c_str());
            if (vbids) {
                vbids->push_back(static_cast<uint16_t>(vbId));
            }
            uint64_t old_rev_num = dbFileRevMap[vbId];
            if (old_rev_num == revNum) {
                continue;
            } else if (old_rev_num < revNum) { // stale revision found
                dbFileRevMap[vbId] = revNum;
            } else { // stale file found (revision id has rolled over)
                old_rev_num = revNum;
            }
            std::stringstream old_file;
            old_file << dbname << "/" << vbId << ".couch." << old_rev_num;
            if (access(old_file.str().c_str(), F_OK) == 0) {
                if (!isReadOnly()) {
                    if (remove(old_file.str().c_str()) == 0) {
                        logger.log(EXTENSION_LOG_INFO,
                                  "CouchKVStore::populateFileNameMap: Removed "
                                  "stale file:%s", old_file.str().c_str());
                    } else {
                        logger.log(EXTENSION_LOG_WARNING,
                                   "CouchKVStore::populateFileNameMap: remove "
                                   "error:%s, file:%s", cb_strerror().c_str(),
                                   old_file.str().c_str());
                    }
                } else {
                    logger.log(EXTENSION_LOG_WARNING,
                               "CouchKVStore::populateFileNameMap: A read-only "
                               "instance of the underlying store "
                               "was not allowed to delete a stale file:%s",
                               old_file.str().c_str());
                }
            }
        } else {
            // skip non-vbucket database file, master.couch etc
            logger.log(EXTENSION_LOG_DEBUG,
                       "CouchKVStore::populateFileNameMap: Non-vbucket database file, %s, skip adding "
                       "to CouchKVStore dbFileMap", filename.c_str());
        }
    }
}

couchstore_error_t CouchKVStore::fetchDoc(Db* db,
                                          DocInfo* docinfo,
                                          GetValue& docValue,
                                          uint16_t vbId,
                                          GetMetaOnly metaOnly) {
    couchstore_error_t errCode = COUCHSTORE_SUCCESS;
    std::unique_ptr<MetaData> metadata;
    try {
        metadata = MetaDataFactory::createMetaData(docinfo->rev_meta);
    } catch(std::logic_error& e) {
        return COUCHSTORE_ERROR_DB_NO_LONGER_VALID;
    }

    if (metaOnly == GetMetaOnly::Yes) {
        uint8_t extMeta[EXT_META_LEN];
        extMeta[0] = metadata->getDataType();
        // Collections: TODO: Permanently restore to stored namespace
        auto it = std::make_unique<Item>(
                makeDocKey(docinfo->id,
                           configuration.shouldPersistDocNamespace()),
                metadata->getFlags(),
                metadata->getExptime(),
                nullptr,
                docinfo->size,
                extMeta,
                EXT_META_LEN,
                metadata->getCas(),
                docinfo->db_seq,
                vbId);

        it->setRevSeqno(docinfo->rev_seq);

        if (docinfo->deleted) {
            it->setDeleted();
        }
        docValue = GetValue(std::move(it));
        // update ep-engine IO stats
        ++st.io_num_read;
        st.io_read_bytes += docinfo->id.size;
    } else {
        Doc *doc = nullptr;
        size_t valuelen = 0;
        void* valuePtr = nullptr;
        uint8_t extMeta = 0;
        errCode = couchstore_open_doc_with_docinfo(db, docinfo, &doc,
                                                   DECOMPRESS_DOC_BODIES);
        if (errCode == COUCHSTORE_SUCCESS) {
            if (doc == nullptr) {
                throw std::logic_error("CouchKVStore::fetchDoc: doc is NULL");
            }

            if (doc->id.size > UINT16_MAX) {
                throw std::logic_error("CouchKVStore::fetchDoc: "
                            "doc->id.size (which is" +
                            std::to_string(doc->id.size) + ") is greater than "
                            + std::to_string(UINT16_MAX));
            }

            valuelen = doc->data.size;
            valuePtr = doc->data.buf;

            if (metadata->getVersionInitialisedFrom() == MetaData::Version::V0) {
                // This is a super old version of a couchstore file.
                // Try to determine if the document is JSON or raw bytes
                extMeta = determine_datatype(doc->data);
            } else {
                extMeta = metadata->getDataType();
            }
        } else if (errCode == COUCHSTORE_ERROR_DOC_NOT_FOUND && docinfo->deleted) {
            extMeta = metadata->getDataType();
        } else {
            return errCode;
        }

        try {
            // Collections: TODO: Restore to stored namespace
            auto it = std::make_unique<Item>(
                            makeDocKey(docinfo->id,
                                   configuration.shouldPersistDocNamespace()),
                            metadata->getFlags(),
                            metadata->getExptime(),
                            valuePtr,
                            valuelen,
                            &extMeta,
                            EXT_META_LEN,
                            metadata->getCas(),
                            docinfo->db_seq,
                            vbId,
                            docinfo->rev_seq);

             if (docinfo->deleted) {
                 it->setDeleted();
             }
             docValue = GetValue(std::move(it));
        } catch (std::bad_alloc&) {
            couchstore_free_document(doc);
            return COUCHSTORE_ERROR_ALLOC_FAIL;
        }

        // update ep-engine IO stats
        ++st.io_num_read;
        st.io_read_bytes += (docinfo->id.size + valuelen);

        couchstore_free_document(doc);
    }
    return COUCHSTORE_SUCCESS;
}

int CouchKVStore::recordDbDump(Db *db, DocInfo *docinfo, void *ctx) {

    ScanContext* sctx = static_cast<ScanContext*>(ctx);
    std::shared_ptr<Callback<GetValue> > cb = sctx->callback;
    std::shared_ptr<Callback<CacheLookup> > cl = sctx->lookup;

    Doc *doc = nullptr;
    sized_buf value{nullptr, 0};
    uint64_t byseqno = docinfo->db_seq;
    uint16_t vbucketId = sctx->vbid;

    sized_buf key = docinfo->id;
    if (key.size > UINT16_MAX) {
        throw std::invalid_argument("CouchKVStore::recordDbDump: "
                        "docinfo->id.size (which is " + std::to_string(key.size) +
                        ") is greater than " + std::to_string(UINT16_MAX));
    }

    // Collections: TODO: Permanently restore to stored namespace
    DocKey docKey = makeDocKey(
            docinfo->id, sctx->config.shouldPersistDocNamespace());
    CacheLookup lookup(docKey, byseqno, vbucketId);
    cl->callback(lookup);
    if (cl->getStatus() == ENGINE_KEY_EEXISTS) {
        sctx->lastReadSeqno = byseqno;
        return COUCHSTORE_SUCCESS;
    } else if (cl->getStatus() == ENGINE_ENOMEM) {
        return COUCHSTORE_ERROR_CANCEL;
    }

    auto metadata = MetaDataFactory::createMetaData(docinfo->rev_meta);

    if (sctx->valFilter != ValueFilter::KEYS_ONLY) {
        couchstore_open_options openOptions = 0;

        /**
         * If the stored document has V0 metdata (no datatype)
         * or no special request is made to retrieve compressed documents
         * as is, then DECOMPRESS the document and update datatype
         */
        if (docinfo->rev_meta.size == metadata->getMetaDataSize(MetaData::Version::V0) ||
            sctx->valFilter == ValueFilter::VALUES_DECOMPRESSED) {
            openOptions = DECOMPRESS_DOC_BODIES;
        }

        auto errCode = couchstore_open_doc_with_docinfo(db, docinfo, &doc,
                                                        openOptions);

        if (errCode == COUCHSTORE_SUCCESS) {
            value = doc->data;
            if (doc->data.size) {
                if ((openOptions & DECOMPRESS_DOC_BODIES) == 0) {
                    // We always store the document bodies compressed on disk,
                    // but now the client _wanted_ to fetch the document
                    // in a compressed mode.
                    // We've never stored the "compressed" flag on disk
                    // (as we don't keep items compressed in memory).
                    // Update the datatype flag for this item to
                    // reflect that it is compressed so that the
                    // receiver of the object may notice (Note:
                    // this is currently _ONLY_ happening via DCP
                     auto datatype = metadata->getDataType();
                     metadata->setDataType(datatype | PROTOCOL_BINARY_DATATYPE_SNAPPY);
                } else if (metadata->getVersionInitialisedFrom() == MetaData::Version::V0) {
                    // This is a super old version of a couchstore file.
                    // Try to determine if the document is JSON or raw bytes
                    metadata->setDataType(determine_datatype(doc->data));
                }
            } else {
                // No data, it cannot have a datatype!
                metadata->setDataType(PROTOCOL_BINARY_RAW_BYTES);
            }
        } else if (errCode != COUCHSTORE_ERROR_DOC_NOT_FOUND) {
            sctx->logger->log(EXTENSION_LOG_WARNING,
                              "CouchKVStore::recordDbDump: "
                              "couchstore_open_doc_with_docinfo error:%s [%s], "
                              "vb:%" PRIu16 ", seqno:%" PRIu64,
                              couchstore_strerror(errCode),
                              couchkvstore_strerrno(db, errCode).c_str(),
                              vbucketId, docinfo->rev_seq);
            return COUCHSTORE_SUCCESS;
        }
    }

    uint8_t extMeta = metadata->getDataType();
    uint8_t extMetaLen = metadata->getFlexCode() == FLEX_META_CODE ? EXT_META_LEN : 0;
    // Collections: TODO: Permanently restore to stored namespace
    auto it = std::make_unique<Item>(
            DocKey(makeDocKey(key, sctx->config.shouldPersistDocNamespace())),
            metadata->getFlags(),
            metadata->getExptime(),
            value.buf,
            value.size,
            &extMeta,
            extMetaLen,
            metadata->getCas(),
            docinfo->db_seq, // return seq number being persisted on disk
            vbucketId,
            docinfo->rev_seq);

    if (docinfo->deleted) {
        it->setDeleted();
    }

    bool onlyKeys = (sctx->valFilter == ValueFilter::KEYS_ONLY) ? true : false;
    GetValue rv(std::move(it), ENGINE_SUCCESS, -1, onlyKeys);
    cb->callback(rv);

    couchstore_free_document(doc);

    if (cb->getStatus() == ENGINE_ENOMEM) {
        return COUCHSTORE_ERROR_CANCEL;
    }

    sctx->lastReadSeqno = byseqno;
    return COUCHSTORE_SUCCESS;
}

bool CouchKVStore::commit2couchstore(const Item* collectionsManifest) {
    bool success = true;

    size_t pendingCommitCnt = pendingReqsQ.size();
    if (pendingCommitCnt == 0 && !collectionsManifest) {
        return success;
    }

    // Use the vbucket of the first item or the manifest item
    uint16_t vbucket2flush = pendingCommitCnt
                                     ? pendingReqsQ[0]->getVBucketId()
                                     : collectionsManifest->getVBucketId();

    // When an item and a manifest are present, vbucket2flush is read from the
    // item. Check it matches the manifest
    if (pendingCommitCnt && collectionsManifest &&
        vbucket2flush != collectionsManifest->getVBucketId()) {
        throw std::logic_error(
                "CouchKVStore::commit2couchstore: manifest/item vbucket "
                "mismatch vbucket2flush:" +
                std::to_string(vbucket2flush) + " manifest vb:" +
                std::to_string(collectionsManifest->getVBucketId()));
    }

    // Use the current fileRev, compaction can't change this until we're done
    // flushing.
    uint64_t fileRev = dbFileRevMap[vbucket2flush];

    std::vector<Doc*> docs(pendingCommitCnt);
    std::vector<DocInfo*> docinfos(pendingCommitCnt);

    for (size_t i = 0; i < pendingCommitCnt; ++i) {
        CouchRequest *req = pendingReqsQ[i];
        docs[i] = (Doc *)req->getDbDoc();
        docinfos[i] = req->getDbDocInfo();
        if (vbucket2flush != req->getVBucketId()) {
            throw std::logic_error(
                    "CouchKVStore::commit2couchstore: "
                    "mismatch between vbucket2flush (which is "
                    + std::to_string(vbucket2flush) + ") and pendingReqsQ["
                    + std::to_string(i) + "] (which is "
                    + std::to_string(req->getVBucketId()) + ")");
        }
    }

    kvstats_ctx kvctx(configuration);
    kvctx.vbucket = vbucket2flush;
    // flush all
    couchstore_error_t errCode = saveDocs(
            vbucket2flush, fileRev, docs, docinfos, kvctx, collectionsManifest);

    if (errCode) {
        success = false;
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::commit2couchstore: saveDocs error:%s, "
                   "vb:%" PRIu16 ", rev:%" PRIu64, couchstore_strerror(errCode),
                   vbucket2flush, fileRev);
    }

    commitCallback(pendingReqsQ, kvctx, errCode);

    // clean up
    for (size_t i = 0; i < pendingCommitCnt; ++i) {
        delete pendingReqsQ[i];
    }
    pendingReqsQ.clear();
    return success;
}

static int readDocInfos(Db *db, DocInfo *docinfo, void *ctx) {
    if (ctx == nullptr) {
        throw std::invalid_argument("readDocInfos: ctx must be non-NULL");
    }
    kvstats_ctx* cbCtx = static_cast<kvstats_ctx*>(ctx);
    if(docinfo) {
        // An item exists in the VB DB file.
        if (!docinfo->deleted) {
            // Collections: TODO: Permanently restore to stored namespace
            auto itr = cbCtx->keyStats.find(makeDocKey(
                    docinfo->id,
                    cbCtx->config.shouldPersistDocNamespace()));
            if (itr != cbCtx->keyStats.end()) {
                itr->second.first = true;
            }
        }
    }
    return 0;
}

couchstore_error_t CouchKVStore::saveDocs(uint16_t vbid,
                                          uint64_t rev,
                                          const std::vector<Doc*>& docs,
                                          std::vector<DocInfo*>& docinfos,
                                          kvstats_ctx& kvctx,
                                          const Item* collectionsManifest) {
    couchstore_error_t errCode;
    uint64_t fileRev = rev;
    DbInfo info;
    if (rev == 0) {
        throw std::invalid_argument(
                "CouchKVStore::saveDocs: rev must be non-zero");
    }

    DbHolder db(this);
    errCode = openDB(
            vbid, fileRev, db.getDbAddress(), COUCHSTORE_OPEN_FLAG_CREATE);
    if (errCode != COUCHSTORE_SUCCESS) {
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::saveDocs: openDB error:%s, vb:%" PRIu16
                   ", rev:%" PRIu64 ", numdocs:%" PRIu64,
                   couchstore_strerror(errCode),
                   vbid,
                   fileRev,
                   uint64_t(docs.size()));
        return errCode;
    } else {
        vbucket_state *state = cachedVBStates[vbid];
        if (state == nullptr) {
            throw std::logic_error(
                    "CouchKVStore::saveDocs: cachedVBStates[" +
                    std::to_string(vbid) + "] is NULL");
        }

        uint64_t maxDBSeqno = 0;

        // Only do a couchstore_save_documents if there are docs
        if (docs.size() > 0) {
            std::vector<sized_buf> ids(docs.size());
            for (size_t idx = 0; idx < docs.size(); idx++) {
                ids[idx] = docinfos[idx]->id;
                maxDBSeqno = std::max(maxDBSeqno, docinfos[idx]->db_seq);
                DocKey key = makeDocKey(
                        ids[idx], configuration.shouldPersistDocNamespace());
                kvctx.keyStats[key] =
                        std::make_pair(false, !docinfos[idx]->deleted);
            }
            couchstore_docinfos_by_id(db.getDb(),
                                      ids.data(),
                                      (unsigned)ids.size(),
                                      0,
                                      readDocInfos,
                                      &kvctx);

            hrtime_t cs_begin = gethrtime();
            uint64_t flags = COMPRESS_DOC_BODIES | COUCHSTORE_SEQUENCE_AS_IS;
            errCode = couchstore_save_documents(db.getDb(),
                                                docs.data(),
                                                docinfos.data(),
                                                (unsigned)docs.size(),
                                                flags);
            st.saveDocsHisto.add((gethrtime() - cs_begin) / 1000);
            if (errCode != COUCHSTORE_SUCCESS) {
                logger.log(EXTENSION_LOG_WARNING,
                           "CouchKVStore::saveDocs: couchstore_save_documents "
                           "error:%s [%s], vb:%" PRIu16 ", numdocs:%" PRIu64,
                           couchstore_strerror(errCode),
                           couchkvstore_strerrno(db.getDb(), errCode).c_str(),
                           vbid,
                           uint64_t(docs.size()));
                return errCode;
            }
        }

        errCode = saveVBState(db.getDb(), *state);
        if (errCode != COUCHSTORE_SUCCESS) {
            logger.log(EXTENSION_LOG_WARNING,
                       "CouchKVStore::saveDocs: saveVBState error:%s [%s]",
                       couchstore_strerror(errCode),
                       couchkvstore_strerrno(db.getDb(), errCode).c_str());
            return errCode;
        }

        if (collectionsManifest) {
            saveCollectionsManifest(*db.getDb(), *collectionsManifest);
        }

        hrtime_t cs_begin = gethrtime();
        errCode = couchstore_commit(db.getDb());
        st.commitHisto.add((gethrtime() - cs_begin) / 1000);
        if (errCode) {
            logger.log(
                    EXTENSION_LOG_WARNING,
                    "CouchKVStore::saveDocs: couchstore_commit error:%s [%s]",
                    couchstore_strerror(errCode),
                    couchkvstore_strerrno(db.getDb(), errCode).c_str());
            return errCode;
        }

        st.batchSize.add(docs.size());

        // retrieve storage system stats for file fragmentation computation
        couchstore_db_info(db.getDb(), &info);
        cachedSpaceUsed[vbid] = info.space_used;
        cachedFileSize[vbid] = info.file_size;
        cachedDeleteCount[vbid] = info.deleted_count;
        cachedDocCount[vbid] = info.doc_count;

        // Check seqno if we wrote documents
        if (docs.size() > 0 && maxDBSeqno != info.last_sequence) {
            logger.log(EXTENSION_LOG_WARNING,
                       "CouchKVStore::saveDocs: Seqno in db header (%" PRIu64 ")"
                       " is not matched with what was persisted (%" PRIu64 ")"
                       " for vb:%" PRIu16,
                       info.last_sequence, maxDBSeqno, vbid);
        }
        state->highSeqno = info.last_sequence;
    }

    /* update stat */
    if(errCode == COUCHSTORE_SUCCESS) {
        st.docsCommitted = docs.size();
    }

    return errCode;
}

void CouchKVStore::remVBucketFromDbFileMap(uint16_t vbucketId) {
    if (vbucketId >= numDbFiles) {
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::remVBucketFromDbFileMap: Cannot remove db "
                   "file map entry for an invalid vbucket, "
                   "vb:%" PRIu16, vbucketId);
        return;
    }

    // just reset revision number of the requested vbucket
    dbFileRevMap[vbucketId] = 1;
}

void CouchKVStore::commitCallback(std::vector<CouchRequest *> &committedReqs,
                                  kvstats_ctx &kvctx,
                                  couchstore_error_t errCode) {
    size_t commitSize = committedReqs.size();

    for (size_t index = 0; index < commitSize; index++) {
        size_t dataSize = committedReqs[index]->getNBytes();
        size_t keySize = committedReqs[index]->getKey().size();
        /* update ep stats */
        ++st.io_num_write;
        st.io_write_bytes += (keySize + dataSize);

        if (committedReqs[index]->isDelete()) {
            int rv = getMutationStatus(errCode);
            if (rv != -1) {
                const auto& key = committedReqs[index]->getKey();
                if (kvctx.keyStats[key].first) {
                    rv = 1; // Deletion is for an existing item on DB file.
                } else {
                    rv = 0; // Deletion is for a non-existing item on DB file.
                }
            }
            if (errCode) {
                ++st.numDelFailure;
            } else {
                st.delTimeHisto.add(committedReqs[index]->getDelta() / 1000);
            }
            committedReqs[index]->getDelCallback()->callback(rv);
        } else {
            int rv = getMutationStatus(errCode);
            const auto& key = committedReqs[index]->getKey();
            bool insertion = !kvctx.keyStats[key].first;
            if (errCode) {
                ++st.numSetFailure;
            } else {
                st.writeTimeHisto.add(committedReqs[index]->getDelta() / 1000);
                st.writeSizeHisto.add(dataSize + keySize);
            }
            mutation_result p(rv, insertion);
            committedReqs[index]->getSetCallback()->callback(p);
        }
    }
}

ENGINE_ERROR_CODE CouchKVStore::readVBState(Db *db, uint16_t vbId) {
    sized_buf id;
    LocalDoc *ldoc = NULL;
    couchstore_error_t errCode = COUCHSTORE_SUCCESS;
    vbucket_state_t state = vbucket_state_dead;
    uint64_t checkpointId = 0;
    uint64_t maxDeletedSeqno = 0;
    int64_t highSeqno = 0;
    std::string failovers;
    uint64_t purgeSeqno = 0;
    uint64_t lastSnapStart = 0;
    uint64_t lastSnapEnd = 0;
    uint64_t maxCas = 0;
    int64_t hlcCasEpochSeqno = HlcCasSeqnoUninitialised;
    bool mightContainXattrs = false;

    DbInfo info;
    errCode = couchstore_db_info(db, &info);
    if (errCode == COUCHSTORE_SUCCESS) {
        highSeqno = info.last_sequence;
        purgeSeqno = info.purge_seq;
    } else {
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::readVBState: couchstore_db_info error:%s"
                   ", vb:%" PRIu16, couchstore_strerror(errCode), vbId);
        return couchErr2EngineErr(errCode);
    }

    id.buf = (char *)"_local/vbstate";
    id.size = sizeof("_local/vbstate") - 1;
    errCode = couchstore_open_local_document(db, (void *)id.buf,
                                             id.size, &ldoc);
    if (errCode != COUCHSTORE_SUCCESS) {
        if (errCode == COUCHSTORE_ERROR_DOC_NOT_FOUND) {
            logger.log(EXTENSION_LOG_NOTICE,
                       "CouchKVStore::readVBState: '_local/vbstate' not found "
                       "for vb:%d", vbId);
        } else {
            logger.log(EXTENSION_LOG_WARNING,
                       "CouchKVStore::readVBState: couchstore_open_local_document"
                       " error:%s, vb:%" PRIu16, couchstore_strerror(errCode),
                       vbId);
        }
    } else {
        const std::string statjson(ldoc->json.buf, ldoc->json.size);
        cJSON *jsonObj = cJSON_Parse(statjson.c_str());
        if (!jsonObj) {
            couchstore_free_local_document(ldoc);
            logger.log(EXTENSION_LOG_WARNING, "CouchKVStore::readVBState: Failed to "
                       "parse the vbstat json doc for vb:%" PRIu16 ", json:%s",
                       vbId , statjson.c_str());
            return couchErr2EngineErr(errCode);
        }

        const std::string vb_state = getJSONObjString(
                                cJSON_GetObjectItem(jsonObj, "state"));
        const std::string checkpoint_id = getJSONObjString(
                                cJSON_GetObjectItem(jsonObj,"checkpoint_id"));
        const std::string max_deleted_seqno = getJSONObjString(
                                cJSON_GetObjectItem(jsonObj, "max_deleted_seqno"));
        const std::string snapStart = getJSONObjString(
                                cJSON_GetObjectItem(jsonObj, "snap_start"));
        const std::string snapEnd = getJSONObjString(
                                cJSON_GetObjectItem(jsonObj, "snap_end"));
        const std::string maxCasValue = getJSONObjString(
                                cJSON_GetObjectItem(jsonObj, "max_cas"));
        const std::string hlcCasEpoch =
                getJSONObjString(cJSON_GetObjectItem(jsonObj, "hlc_epoch"));
        mightContainXattrs = getJSONObjBool(
                cJSON_GetObjectItem(jsonObj, "might_contain_xattrs"));

        cJSON *failover_json = cJSON_GetObjectItem(jsonObj, "failover_table");
        if (vb_state.compare("") == 0 || checkpoint_id.compare("") == 0
                || max_deleted_seqno.compare("") == 0) {
            logger.log(EXTENSION_LOG_WARNING, "CouchKVStore::readVBState: State"
                       " JSON doc for vb:%" PRIu16 " is in the wrong format:%s, "
                       "vb state:%s, checkpoint id:%s and max deleted seqno:%s",
                       vbId, statjson.c_str(), vb_state.c_str(),
                       checkpoint_id.c_str(), max_deleted_seqno.c_str());
        } else {
            state = VBucket::fromString(vb_state.c_str());
            parseUint64(max_deleted_seqno.c_str(), &maxDeletedSeqno);
            parseUint64(checkpoint_id.c_str(), &checkpointId);

            if (snapStart.compare("") == 0) {
                lastSnapStart = highSeqno;
            } else {
                parseUint64(snapStart.c_str(), &lastSnapStart);
            }

            if (snapEnd.compare("") == 0) {
                lastSnapEnd = highSeqno;
            } else {
                parseUint64(snapEnd.c_str(), &lastSnapEnd);
            }

            if (maxCasValue.compare("") != 0) {
                parseUint64(maxCasValue.c_str(), &maxCas);

                // MB-17517: If the maxCas on disk was invalid then don't use it -
                // instead rebuild from the items we load from disk (i.e. as per
                // an upgrade from an earlier version).
                if (maxCas == static_cast<uint64_t>(-1)) {
                    logger.log(EXTENSION_LOG_WARNING,
                               "CouchKVStore::readVBState: Invalid max_cas "
                               "(0x%" PRIx64 ") read from '%s' for vb:%" PRIu16
                               ". Resetting max_cas to zero.",
                               maxCas, id.buf, vbId);
                    maxCas = 0;
                }
            }

            if (!hlcCasEpoch.empty()) {
                parseInt64(hlcCasEpoch.c_str(), &hlcCasEpochSeqno);
            }

            if (failover_json) {
                char* json = cJSON_PrintUnformatted(failover_json);
                failovers.assign(json);
                cJSON_Free(json);
            }
        }
        cJSON_Delete(jsonObj);
        couchstore_free_local_document(ldoc);
    }

    delete cachedVBStates[vbId];
    cachedVBStates[vbId] = new vbucket_state(state,
                                             checkpointId,
                                             maxDeletedSeqno,
                                             highSeqno,
                                             purgeSeqno,
                                             lastSnapStart,
                                             lastSnapEnd,
                                             maxCas,
                                             hlcCasEpochSeqno,
                                             mightContainXattrs,
                                             failovers);

    return couchErr2EngineErr(errCode);
}

couchstore_error_t CouchKVStore::saveVBState(Db *db,
                                             const vbucket_state &vbState) {
    std::stringstream jsonState;

    jsonState << "{\"state\": \"" << VBucket::toString(vbState.state) << "\""
              << ",\"checkpoint_id\": \"" << vbState.checkpointId << "\""
              << ",\"max_deleted_seqno\": \"" << vbState.maxDeletedSeqno << "\"";
    if (!vbState.failovers.empty()) {
        jsonState << ",\"failover_table\": " << vbState.failovers;
    }
    jsonState << ",\"snap_start\": \"" << vbState.lastSnapStart << "\""
              << ",\"snap_end\": \"" << vbState.lastSnapEnd << "\""
              << ",\"max_cas\": \"" << vbState.maxCas << "\""
              << ",\"hlc_epoch\": \"" << vbState.hlcCasEpochSeqno << "\"";

    if (vbState.mightContainXattrs) {
        jsonState << ",\"might_contain_xattrs\": true";
    } else {
        jsonState << ",\"might_contain_xattrs\": false";
    }

    jsonState << "}";

    LocalDoc lDoc;
    lDoc.id.buf = (char *)"_local/vbstate";
    lDoc.id.size = sizeof("_local/vbstate") - 1;
    std::string state = jsonState.str();
    lDoc.json.buf = (char *)state.c_str();
    lDoc.json.size = state.size();
    lDoc.deleted = 0;

    couchstore_error_t errCode = couchstore_save_local_document(db, &lDoc);
    if (errCode != COUCHSTORE_SUCCESS) {
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::saveVBState couchstore_save_local_document "
                   "error:%s [%s]", couchstore_strerror(errCode),
                   couchkvstore_strerrno(db, errCode).c_str());
    }

    return errCode;
}

couchstore_error_t CouchKVStore::saveCollectionsManifest(
        Db& db, const Item& collectionsManifest) {
    LocalDoc lDoc;
    lDoc.id.buf = const_cast<char*>(Collections::CouchstoreManifest);
    lDoc.id.size = Collections::CouchstoreManifestLen;

    // Convert the Item value into JSON
    cb::const_char_buffer buffer(collectionsManifest.getData(),
                                 collectionsManifest.getNBytes());
    std::string state = Collections::VB::Manifest::serialToJson(
            SystemEvent(collectionsManifest.getFlags()),
            buffer,
            collectionsManifest.getBySeqno());

    lDoc.json.buf = const_cast<char*>(state.c_str());
    lDoc.json.size = state.size();
    lDoc.deleted = 0;

    couchstore_error_t errCode = couchstore_save_local_document(&db, &lDoc);

    if (errCode != COUCHSTORE_SUCCESS) {
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::saveCollectionsManifest "
                   "couchstore_save_local_document "
                   "error:%s [%s]",
                   couchstore_strerror(errCode),
                   couchkvstore_strerrno(&db, errCode).c_str());
    }

    return errCode;
}

std::string CouchKVStore::readCollectionsManifest(Db& db) {
    sized_buf id;
    id.buf = const_cast<char*>(Collections::CouchstoreManifest);
    id.size = sizeof(Collections::CouchstoreManifest) - 1;

    LocalDocHolder lDoc;
    auto errCode = couchstore_open_local_document(
            &db, (void*)id.buf, id.size, lDoc.getLocalDocAddress());
    if (errCode != COUCHSTORE_SUCCESS) {
        if (errCode == COUCHSTORE_ERROR_DOC_NOT_FOUND) {
            logger.log(EXTENSION_LOG_NOTICE,
                   "CouchKVStore::readCollectionsManifest: doc not found");
        } else {
            logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::readCollectionsManifest: "
                   "couchstore_open_local_document error:%s",
                   couchstore_strerror(errCode));
        }

        return {};
    }

    return {lDoc.getLocalDoc()->json.buf, lDoc.getLocalDoc()->json.size};
}

int CouchKVStore::getMultiCb(Db *db, DocInfo *docinfo, void *ctx) {
    if (docinfo == nullptr) {
        throw std::invalid_argument("CouchKVStore::getMultiCb: docinfo "
                "must be non-NULL");
    }
    if (ctx == nullptr) {
        throw std::invalid_argument("CouchKVStore::getMultiCb: ctx must "
                "be non-NULL");
    }

    GetMultiCbCtx *cbCtx = static_cast<GetMultiCbCtx *>(ctx);
    // Collections: TODO: Permanently restore to stored namespace
    DocKey key = makeDocKey(docinfo->id,
                            cbCtx->cks.getConfig().shouldPersistDocNamespace());
    KVStoreStats& st = cbCtx->cks.getKVStoreStat();

    vb_bgfetch_queue_t::iterator qitr = cbCtx->fetches.find(key);
    if (qitr == cbCtx->fetches.end()) {
        // this could be a serious race condition in couchstore,
        // log a warning message and continue
        cbCtx->cks.logger.log(EXTENSION_LOG_WARNING,
                              "CouchKVStore::getMultiCb: Couchstore returned "
                              "invalid docinfo, no pending bgfetch has been "
                              "issued for a key in vb:%" PRIu16 ", "
                              "seqno:%" PRIu64, cbCtx->vbId, docinfo->rev_seq);
        return 0;
    }

    vb_bgfetch_item_ctx_t& bg_itm_ctx = (*qitr).second;
    GetMetaOnly meta_only = bg_itm_ctx.isMetaOnly;

    couchstore_error_t errCode = cbCtx->cks.fetchDoc(
            db, docinfo, bg_itm_ctx.value, cbCtx->vbId, meta_only);
    if (errCode != COUCHSTORE_SUCCESS && (meta_only == GetMetaOnly::No)) {
        st.numGetFailure++;
    }

    bg_itm_ctx.value.setStatus(cbCtx->cks.couchErr2EngineErr(errCode));

    bool return_val_ownership_transferred = false;
    for (auto& fetch : bg_itm_ctx.bgfetched_list) {
        return_val_ownership_transferred = true;
        // populate return value for remaining fetch items with the
        // same seqid
        fetch->value = &bg_itm_ctx.value;
        st.readTimeHisto.add(
                std::chrono::duration_cast<std::chrono::microseconds>(
                        ProcessClock::now() - fetch->initTime)
                        .count());
        if (errCode == COUCHSTORE_SUCCESS) {
            st.readSizeHisto.add(bg_itm_ctx.value.item->getKey().size() +
                                 bg_itm_ctx.value.item->getNBytes());
        }
    }
    if (!return_val_ownership_transferred) {
        cbCtx->cks.logger.log(EXTENSION_LOG_WARNING,
                              "CouchKVStore::getMultiCb called with zero"
                              "items in bgfetched_list, vb:%" PRIu16
                              ", seqno:%" PRIu64,
                              cbCtx->vbId, docinfo->rev_seq);
    }

    return 0;
}


void CouchKVStore::closeDatabaseHandle(Db *db) {
    couchstore_error_t ret = couchstore_close_file(db);
    if (ret != COUCHSTORE_SUCCESS) {
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::closeDatabaseHandle: couchstore_close_file "
                   "error:%s [%s]", couchstore_strerror(ret),
                   couchkvstore_strerrno(db, ret).c_str());
    }
    ret = couchstore_free_db(db);
    if (ret != COUCHSTORE_SUCCESS) {
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::closeDatabaseHandle: couchstore_free_db "
                   "error:%s [%s]", couchstore_strerror(ret),
                   couchkvstore_strerrno(nullptr, ret).c_str());
    }
    st.numClose++;
}

ENGINE_ERROR_CODE CouchKVStore::couchErr2EngineErr(couchstore_error_t errCode) {
    switch (errCode) {
    case COUCHSTORE_SUCCESS:
        return ENGINE_SUCCESS;
    case COUCHSTORE_ERROR_ALLOC_FAIL:
        return ENGINE_ENOMEM;
    case COUCHSTORE_ERROR_DOC_NOT_FOUND:
        return ENGINE_KEY_ENOENT;
    case COUCHSTORE_ERROR_NO_SUCH_FILE:
    case COUCHSTORE_ERROR_NO_HEADER:
    default:
        // same as the general error return code of
        // EPBucket::getInternal
        return ENGINE_TMPFAIL;
    }
}

size_t CouchKVStore::getNumPersistedDeletes(uint16_t vbid) {
    size_t delCount = cachedDeleteCount[vbid];
    if (delCount != (size_t) -1) {
        return delCount;
    }

    Db *db = NULL;
    uint64_t rev = dbFileRevMap[vbid];
    couchstore_error_t errCode = openDB(vbid, rev, &db,
                                        COUCHSTORE_OPEN_FLAG_RDONLY);
    if (errCode == COUCHSTORE_SUCCESS) {
        DbInfo info;
        errCode = couchstore_db_info(db, &info);
        if (errCode == COUCHSTORE_SUCCESS) {
            cachedDeleteCount[vbid] = info.deleted_count;
            closeDatabaseHandle(db);
            return info.deleted_count;
        } else {
            throw std::runtime_error("CouchKVStore::getNumPersistedDeletes:"
                "Failed to read database info for vBucket = " +
                std::to_string(vbid) + " rev = " + std::to_string(rev) +
                " with error:" + couchstore_strerror(errCode));
        }
        closeDatabaseHandle(db);
    } else {
        // open failed - map couchstore error code to exception.
        std::errc ec;
        switch (errCode) {
            case COUCHSTORE_ERROR_OPEN_FILE:
                ec = std::errc::no_such_file_or_directory; break;
            default:
                ec = std::errc::io_error; break;
        }
        throw std::system_error(std::make_error_code(ec),
                                "CouchKVStore::getNumPersistedDeletes:"
            "Failed to open database file for vBucket = " +
            std::to_string(vbid) + " rev = " + std::to_string(rev) +
            " with error:" + couchstore_strerror(errCode));
    }
    return 0;
}

DBFileInfo CouchKVStore::getDbFileInfo(uint16_t vbid) {

    DbInfo info = getDbInfo(vbid);
    return DBFileInfo{info.file_size, info.space_used};
}

DBFileInfo CouchKVStore::getAggrDbFileInfo() {
    DBFileInfo kvsFileInfo;
    /**
     * Iterate over all the vbuckets to get the total.
     * If the vbucket is dead, then its value would
     * be zero.
     */
    for (uint16_t vbid = 0; vbid < numDbFiles; vbid++) {
        kvsFileInfo.fileSize += cachedFileSize[vbid].load();
        kvsFileInfo.spaceUsed += cachedSpaceUsed[vbid].load();
    }
    return kvsFileInfo;
}

size_t CouchKVStore::getNumItems(uint16_t vbid, uint64_t min_seq,
                                 uint64_t max_seq) {
    Db *db = NULL;
    uint64_t count = 0;
    uint64_t rev = dbFileRevMap[vbid];
    couchstore_error_t errCode = openDB(vbid, rev, &db,
                                        COUCHSTORE_OPEN_FLAG_RDONLY);
    if (errCode == COUCHSTORE_SUCCESS) {
        errCode = couchstore_changes_count(db, min_seq, max_seq, &count);
        if (errCode != COUCHSTORE_SUCCESS) {
            closeDatabaseHandle(db);
            throw std::runtime_error("CouchKVStore::getNumItems: Failed to "
                "get changes count for vBucket = " + std::to_string(vbid) +
                " rev = " + std::to_string(rev) +
                " with error:" + couchstore_strerror(errCode));
        }
        closeDatabaseHandle(db);
    } else {
        throw std::invalid_argument("CouchKVStore::getNumItems: Failed to "
            "open database file for vBucket = " + std::to_string(vbid) +
            " rev = " + std::to_string(rev) +
            " with error:" + couchstore_strerror(errCode));
    }
    return count;
}

size_t CouchKVStore::getItemCount(uint16_t vbid) {
    if (!isReadOnly()) {
        return cachedDocCount.at(vbid);
    }
    return getDbInfo(vbid).doc_count;
}

RollbackResult CouchKVStore::rollback(uint16_t vbid, uint64_t rollbackSeqno,
                                      std::shared_ptr<RollbackCB> cb) {
    DbHolder db(this);
    DbInfo info;
    uint64_t fileRev = dbFileRevMap[vbid];
    std::stringstream dbFileName;
    dbFileName << dbname << "/" << vbid << ".couch." << fileRev;
    couchstore_error_t errCode;

    errCode = openDB(vbid, fileRev, db.getDbAddress(),
                     (uint64_t) COUCHSTORE_OPEN_FLAG_RDONLY);

    if (errCode == COUCHSTORE_SUCCESS) {
        errCode = couchstore_db_info(db.getDb(), &info);
        if (errCode != COUCHSTORE_SUCCESS) {
            logger.log(EXTENSION_LOG_WARNING,
                       "CouchKVStore::rollback: couchstore_db_info error:%s, "
                       "name:%s", couchstore_strerror(errCode),
                       dbFileName.str().c_str());
            return RollbackResult(false, 0, 0, 0);
        }
    } else {
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::rollback: openDB error:%s, name:%s",
                   couchstore_strerror(errCode), dbFileName.str().c_str());
        return RollbackResult(false, 0, 0, 0);
    }

    uint64_t latestSeqno = info.last_sequence;

    //Count from latest seq no to 0
    uint64_t totSeqCount = 0;
    errCode = couchstore_changes_count(db.getDb(), 0, latestSeqno, &totSeqCount);
    if (errCode != COUCHSTORE_SUCCESS) {
        logger.log(EXTENSION_LOG_WARNING, "CouchKVStore::rollback: "
                   "couchstore_changes_count(0, %" PRIu64 ") error:%s [%s], "
                   "vb:%" PRIu16 ", rev:%" PRIu64, latestSeqno,
                   couchstore_strerror(errCode), cb_strerror().c_str(), vbid,
                   fileRev);
        return RollbackResult(false, 0, 0, 0);
    }

    DbHolder newdb(this);
    errCode = openDB(vbid, fileRev, newdb.getDbAddress(), 0);
    if (errCode != COUCHSTORE_SUCCESS) {
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::rollback: openDB#2 error:%s, name:%s",
                   couchstore_strerror(errCode), dbFileName.str().c_str());
        return RollbackResult(false, 0, 0, 0);
    }

    while (info.last_sequence > rollbackSeqno) {
        errCode = couchstore_rewind_db_header(newdb.getDb());
        if (errCode != COUCHSTORE_SUCCESS) {
            // rewind_db_header cleans up (frees DB) on error; so
            // release db in DbHolder to prevent a double-free.
            newdb.releaseDb();
            logger.log(EXTENSION_LOG_WARNING,
                       "CouchKVStore::rollback: couchstore_rewind_db_header "
                       "error:%s [%s], vb:%" PRIu16 ", latestSeqno:%" PRIu64
                       ", rollbackSeqno:%" PRIu64,
                       couchstore_strerror(errCode), cb_strerror().c_str(),
                       vbid, latestSeqno, rollbackSeqno);
            //Reset the vbucket and send the entire snapshot,
            //as a previous header wasn't found.
            return RollbackResult(false, 0, 0, 0);
        }
        errCode = couchstore_db_info(newdb.getDb(), &info);
        if (errCode != COUCHSTORE_SUCCESS) {
            logger.log(EXTENSION_LOG_WARNING,
                       "CouchKVStore::rollback: couchstore_db_info error:%s, "
                       "name:%s", couchstore_strerror(errCode),
                       dbFileName.str().c_str());
            return RollbackResult(false, 0, 0, 0);
        }
    }

    //Count from latest seq no to rollback seq no
    uint64_t rollbackSeqCount = 0;
    errCode = couchstore_changes_count(db.getDb(), info.last_sequence, latestSeqno,
                                       &rollbackSeqCount);
    if (errCode != COUCHSTORE_SUCCESS) {
        logger.log(EXTENSION_LOG_WARNING, "CouchKVStore::rollback: "
                   "couchstore_changes_count#2(%" PRIu64 ", %" PRIu64 ") "
                   "error:%s [%s], vb:%" PRIu16 ", rev:%" PRIu64,
                   info.last_sequence, latestSeqno, couchstore_strerror(errCode),
                   cb_strerror().c_str(), vbid, fileRev);
        return RollbackResult(false, 0, 0, 0);
    }

    if ((totSeqCount / 2) <= rollbackSeqCount) {
        //doresetVbucket flag set or rollback is greater than 50%,
        //reset the vbucket and send the entire snapshot
        return RollbackResult(false, 0, 0, 0);
    }

    cb->setDbHeader(newdb.getDb());
    std::shared_ptr<Callback<CacheLookup> > cl(new NoLookupCallback());
    ScanContext* ctx = initScanContext(cb, cl, vbid, info.last_sequence+1,
                                       DocumentFilter::ALL_ITEMS,
                                       ValueFilter::KEYS_ONLY);
    scan_error_t error = scan(ctx);
    destroyScanContext(ctx);

    if (error != scan_success) {
        return RollbackResult(false, 0, 0, 0);
    }

    readVBState(newdb.getDb(), vbid);
    cachedDeleteCount[vbid] = info.deleted_count;
    cachedDocCount[vbid] = info.doc_count;

    //Append the rewinded header to the database file
    errCode = couchstore_commit(newdb.getDb());

    if (errCode != COUCHSTORE_SUCCESS) {
        return RollbackResult(false, 0, 0, 0);
    }

    vbucket_state *vb_state = cachedVBStates[vbid];
    return RollbackResult(true, vb_state->highSeqno,
                          vb_state->lastSnapStart, vb_state->lastSnapEnd);
}

int populateAllKeys(Db *db, DocInfo *docinfo, void *ctx) {
    AllKeysCtx *allKeysCtx = (AllKeysCtx *)ctx;
    // Collections: TODO: Restore to stored namespace
    DocKey key = makeDocKey(docinfo->id, false /*restore namespace*/);
    (allKeysCtx->cb)->callback(key);
    if (--(allKeysCtx->count) <= 0) {
        //Only when count met is less than the actual number of entries
        return COUCHSTORE_ERROR_CANCEL;
    }
    return COUCHSTORE_SUCCESS;
}

ENGINE_ERROR_CODE
CouchKVStore::getAllKeys(uint16_t vbid,
                         const DocKey start_key,
                         uint32_t count,
                         std::shared_ptr<Callback<const DocKey&>> cb) {
    Db *db = NULL;
    uint64_t rev = dbFileRevMap[vbid];
    couchstore_error_t errCode = openDB(vbid, rev, &db,
                                        COUCHSTORE_OPEN_FLAG_RDONLY);
    if(errCode == COUCHSTORE_SUCCESS) {
        sized_buf ref = {NULL, 0};
        ref.buf = (char*) start_key.data();
        ref.size = start_key.size();
        AllKeysCtx ctx(cb, count);
        errCode = couchstore_all_docs(db, &ref, COUCHSTORE_NO_DELETES,
                                      populateAllKeys,
                                      static_cast<void *>(&ctx));
        closeDatabaseHandle(db);
        if (errCode == COUCHSTORE_SUCCESS ||
                errCode == COUCHSTORE_ERROR_CANCEL)  {
            return ENGINE_SUCCESS;
        } else {
            logger.log(EXTENSION_LOG_WARNING,
                       "CouchKVStore::getAllKeys: couchstore_all_docs "
                       "error:%s [%s] vb:%" PRIu16 ", rev:%" PRIu64,
                       couchstore_strerror(errCode), cb_strerror().c_str(),
                       vbid, rev);
        }
    } else {
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::getAllKeys: openDB error:%s, vb:%" PRIu16
                   ", rev:%" PRIu64, couchstore_strerror(errCode), vbid, rev);
    }
    return ENGINE_FAILED;
}

void CouchKVStore::unlinkCouchFile(uint16_t vbucket,
                                   uint64_t fRev) {

    if (isReadOnly()) {
        throw std::logic_error("CouchKVStore::unlinkCouchFile: Not valid on a "
                "read-only object.");
    }
    char fname[PATH_MAX];
    try {
        checked_snprintf(fname, sizeof(fname), "%s/%d.couch.%" PRIu64,
                         dbname.c_str(), vbucket, fRev);
    } catch (std::exception& error) {
        LOG(EXTENSION_LOG_WARNING,
            "CouchKVStore::unlinkCouchFile: Failed to build filename:%s",
            fname);
        return;
    }

    if (remove(fname) == -1) {
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::unlinkCouchFile: remove error:%u, "
                   "vb:%" PRIu16 ", rev:%" PRIu64 ", fname:%s", errno, vbucket,
                   fRev, fname);

        if (errno != ENOENT) {
            std::string file_str = fname;
            pendingFileDeletions.push(file_str);
        }
    }
}

void CouchKVStore::removeCompactFile(const std::string &dbname,
                                     uint16_t vbid,
                                     uint64_t fileRev) {

    std::string dbfile = getDBFileName(dbname, vbid, fileRev);
    std::string compact_file = dbfile + ".compact";

    if (!isReadOnly()) {
        removeCompactFile(compact_file);
    } else {
        logger.log(EXTENSION_LOG_WARNING,
                   "CouchKVStore::removeCompactFile: A read-only instance of "
                   "the underlying store was not allowed to delete a temporary"
                   "file: %s",
                   compact_file.c_str());
    }
}

void CouchKVStore::removeCompactFile(const std::string &filename) {
    if (isReadOnly()) {
        throw std::logic_error("CouchKVStore::removeCompactFile: Not valid on "
                "a read-only object.");
    }

    if (access(filename.c_str(), F_OK) == 0) {
        if (remove(filename.c_str()) == 0) {
            logger.log(EXTENSION_LOG_WARNING,
                       "CouchKVStore::removeCompactFile: Removed compact "
                       "filename:%s", filename.c_str());
        }
        else {
            logger.log(EXTENSION_LOG_WARNING,
                       "CouchKVStore::removeCompactFile: remove error:%s, "
                       "filename:%s", cb_strerror().c_str(), filename.c_str());

            if (errno != ENOENT) {
                pendingFileDeletions.push(const_cast<std::string &>(filename));
            }
        }
    }
}

bool CouchKVStore::persistCollectionsManifestItem(uint16_t vbid,
                                                  const Item& manifestItem) {
    DbHolder db(this);

    // openDB logs error details
    couchstore_error_t errCode = openDB(vbid,
                                        dbFileRevMap[vbid],
                                        db.getDbAddress(),
                                        COUCHSTORE_OPEN_FLAG_CREATE);
    if (errCode != COUCHSTORE_SUCCESS) {
        return false;
    }

    // saveCollecionsManifest logs error details
    errCode = saveCollectionsManifest(*db.getDb(), manifestItem);
    if (errCode != COUCHSTORE_SUCCESS) {
        return false;
    }

    // commit logs error details
    errCode = couchstore_commit(db.getDb());
    if (errCode != COUCHSTORE_SUCCESS) {
        return false;
    }

    return true;
}

std::string CouchKVStore::getCollectionsManifest(uint16_t vbid) {
    DbHolder db(this);

    // openDB logs error details
    couchstore_error_t errCode = openDB(vbid,
                                        dbFileRevMap[vbid],
                                        db.getDbAddress(),
                                        COUCHSTORE_OPEN_FLAG_CREATE);
    if (errCode != COUCHSTORE_SUCCESS) {
        return {};
    }

    return readCollectionsManifest(*db.getDb());
}

void CouchKVStore::incrementRevision(uint16_t vbid) {
    dbFileRevMap[vbid]++;
}

uint64_t CouchKVStore::prepareToDelete(uint16_t vbid) {
    // Clear the stats so it looks empty (real deletion of the disk data occurs
    // later)
    cachedDocCount[vbid] = 0;
    cachedDeleteCount[vbid] = 0;
    cachedFileSize[vbid] = 0;
    cachedSpaceUsed[vbid] = 0;
    return dbFileRevMap[vbid];
}

/* end of couch-kvstore.cc */
