/**
 *    Copyright 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include <algorithm>
#include <boost/optional.hpp>
#include <memory>

#include "mongo/bson/bsonmisc.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_interface_local.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/storage/recovery_unit_noop.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace {

using namespace mongo;
using namespace mongo::repl;

const auto kIndexVersion = IndexDescriptor::IndexVersion::kV2;

BSONObj makeIdIndexSpec(const NamespaceString& nss) {
    return BSON("ns" << nss.toString() << "name"
                     << "_id_"
                     << "key"
                     << BSON("_id" << 1)
                     << "unique"
                     << true
                     << "v"
                     << static_cast<int>(kIndexVersion));
}

/**
 * Generates a unique namespace from the test registration agent.
 */
template <typename T>
NamespaceString makeNamespace(const T& t, const char* suffix = "") {
    return NamespaceString("local." + t.getSuiteName() + "_" + t.getTestName() + suffix);
}

/**
 * Returns min valid document.
 */
BSONObj getMinValidDocument(OperationContext* opCtx, const NamespaceString& minValidNss) {
    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        Lock::DBLock dblk(opCtx, minValidNss.db(), MODE_IS);
        Lock::CollectionLock lk(opCtx->lockState(), minValidNss.ns(), MODE_IS);
        BSONObj mv;
        if (Helpers::getSingleton(opCtx, minValidNss.ns().c_str(), mv)) {
            return mv;
        }
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(opCtx, "getMinValidDocument", minValidNss.ns());
    return BSONObj();
}

/**
 * Creates collection options suitable for oplog.
 */
CollectionOptions createOplogCollectionOptions() {
    CollectionOptions options;
    options.capped = true;
    options.cappedSize = 64 * 1024 * 1024LL;
    options.autoIndexId = CollectionOptions::NO;
    return options;
}

/**
 * Create test collection.
 * Returns collection.
 */
void createCollection(OperationContext* opCtx,
                      const NamespaceString& nss,
                      const CollectionOptions& options = CollectionOptions()) {
    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        Lock::DBLock dblk(opCtx, nss.db(), MODE_X);
        OldClientContext ctx(opCtx, nss.ns());
        auto db = ctx.db();
        ASSERT_TRUE(db);
        mongo::WriteUnitOfWork wuow(opCtx);
        auto coll = db->createCollection(opCtx, nss.ns(), options);
        ASSERT_TRUE(coll);
        wuow.commit();
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(opCtx, "createCollection", nss.ns());
}

/**
 * Creates an oplog entry with given optime.
 */
BSONObj makeOplogEntry(OpTime opTime) {
    BSONObjBuilder bob;
    bob.appendElements(opTime.toBSON());
    bob.append("h", 1LL);
    bob.append("op", "c");
    bob.append("ns", "test.t");
    return bob.obj();
}

/**
 * Helper to create default ReplSettings for tests.
 */
ReplSettings createReplSettings() {
    ReplSettings settings;
    settings.setOplogSizeBytes(5 * 1024 * 1024);
    settings.setReplSetString("mySet/node1:12345");
    return settings;
}

/**
 * Counts the number of keys in an index using an IndexAccessMethod::validate call.
 */
int64_t getIndexKeyCount(OperationContext* opCtx, IndexCatalog* cat, IndexDescriptor* desc) {
    auto idx = cat->getIndex(desc);
    int64_t numKeys;
    ValidateResults fullRes;
    idx->validate(opCtx, &numKeys, &fullRes);
    return numKeys;
}

class StorageInterfaceImplTest : public ServiceContextMongoDTest {
protected:
    OperationContext* getOperationContext() {
        return _opCtx.get();
    }

private:
    void setUp() override {
        ServiceContextMongoDTest::setUp();
        _createOpCtx();
        ReplicationCoordinator::set(getServiceContext(),
                                    stdx::make_unique<ReplicationCoordinatorMock>(
                                        getServiceContext(), createReplSettings()));
    }

    void tearDown() override {
        _ddv.reset(nullptr);
        _uwb.reset(nullptr);
        _opCtx.reset(nullptr);
        ServiceContextMongoDTest::tearDown();
    }

    void _createOpCtx() {
        _opCtx = cc().makeOperationContext();
        // We are not replicating nor validating these writes.
        _uwb = stdx::make_unique<UnreplicatedWritesBlock>(_opCtx.get());
        _ddv = stdx::make_unique<DisableDocumentValidation>(_opCtx.get());
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
    std::unique_ptr<UnreplicatedWritesBlock> _uwb;
    std::unique_ptr<DisableDocumentValidation> _ddv;
};

/**
 * Recovery unit that tracks if waitUntilDurable() is called.
 */
class RecoveryUnitWithDurabilityTracking : public RecoveryUnitNoop {
public:
    bool waitUntilDurable() override;
    bool waitUntilDurableCalled = false;
};

bool RecoveryUnitWithDurabilityTracking::waitUntilDurable() {
    waitUntilDurableCalled = true;
    return RecoveryUnitNoop::waitUntilDurable();
}

TEST_F(StorageInterfaceImplTest, ServiceContextDecorator) {
    auto serviceContext = getServiceContext();
    ASSERT_FALSE(StorageInterface::get(serviceContext));
    StorageInterface* storage = new StorageInterfaceImpl();
    StorageInterface::set(serviceContext, std::unique_ptr<StorageInterface>(storage));
    ASSERT_TRUE(storage == StorageInterface::get(serviceContext));
    ASSERT_TRUE(storage == StorageInterface::get(*serviceContext));
    ASSERT_TRUE(storage == StorageInterface::get(getOperationContext()));
}

TEST_F(StorageInterfaceImplTest, DefaultMinValidNamespace) {
    ASSERT_EQUALS(NamespaceString(StorageInterfaceImpl::kDefaultMinValidNamespace),
                  StorageInterfaceImpl().getMinValidNss());
}

TEST_F(StorageInterfaceImplTest, InitialSyncFlag) {
    auto nss = makeNamespace(_agent);

    StorageInterfaceImpl storage(nss);
    auto opCtx = getOperationContext();

    // Initial sync flag should be unset after initializing a new storage engine.
    ASSERT_FALSE(storage.getInitialSyncFlag(opCtx));

    // Setting initial sync flag should affect getInitialSyncFlag() result.
    storage.setInitialSyncFlag(opCtx);
    ASSERT_TRUE(storage.getInitialSyncFlag(opCtx));

    // Check min valid document using storage engine interface.
    auto minValidDocument = getMinValidDocument(opCtx, nss);
    ASSERT_TRUE(minValidDocument.hasField(StorageInterfaceImpl::kInitialSyncFlagFieldName));
    ASSERT_TRUE(minValidDocument.getBoolField(StorageInterfaceImpl::kInitialSyncFlagFieldName));

    // Clearing initial sync flag should affect getInitialSyncFlag() result.
    storage.clearInitialSyncFlag(opCtx);
    ASSERT_FALSE(storage.getInitialSyncFlag(opCtx));
}

TEST_F(StorageInterfaceImplTest, GetMinValidAfterSettingInitialSyncFlagWorks) {
    auto nss = makeNamespace(_agent);

    StorageInterfaceImpl storage(nss);
    auto opCtx = getOperationContext();

    // Initial sync flag should be unset after initializing a new storage engine.
    ASSERT_FALSE(storage.getInitialSyncFlag(opCtx));

    // Setting initial sync flag should affect getInitialSyncFlag() result.
    storage.setInitialSyncFlag(opCtx);
    ASSERT_TRUE(storage.getInitialSyncFlag(opCtx));

    ASSERT(storage.getMinValid(opCtx).isNull());
    ASSERT(storage.getAppliedThrough(opCtx).isNull());
    ASSERT(storage.getOplogDeleteFromPoint(opCtx).isNull());
}

TEST_F(StorageInterfaceImplTest, MinValid) {
    auto nss = makeNamespace(_agent);

    StorageInterfaceImpl storage(nss);
    auto opCtx = getOperationContext();

    // MinValid boundaries should all be null after initializing a new storage engine.
    ASSERT(storage.getMinValid(opCtx).isNull());
    ASSERT(storage.getAppliedThrough(opCtx).isNull());
    ASSERT(storage.getOplogDeleteFromPoint(opCtx).isNull());

    // Setting min valid boundaries should affect getMinValid() result.
    OpTime startOpTime({Seconds(123), 0}, 1LL);
    OpTime endOpTime({Seconds(456), 0}, 1LL);
    storage.setAppliedThrough(opCtx, startOpTime);
    storage.setMinValid(opCtx, endOpTime);
    storage.setOplogDeleteFromPoint(opCtx, endOpTime.getTimestamp());

    ASSERT_EQ(storage.getAppliedThrough(opCtx), startOpTime);
    ASSERT_EQ(storage.getMinValid(opCtx), endOpTime);
    ASSERT_EQ(storage.getOplogDeleteFromPoint(opCtx), endOpTime.getTimestamp());


    // setMinValid always changes minValid, but setMinValidToAtLeast only does if higher.
    storage.setMinValid(opCtx, startOpTime);  // Forcibly lower it.
    ASSERT_EQ(storage.getMinValid(opCtx), startOpTime);
    storage.setMinValidToAtLeast(opCtx, endOpTime);  // Higher than current (sets it).
    ASSERT_EQ(storage.getMinValid(opCtx), endOpTime);
    storage.setMinValidToAtLeast(opCtx, startOpTime);  // Lower than current (no-op).
    ASSERT_EQ(storage.getMinValid(opCtx), endOpTime);

    // Check min valid document using storage engine interface.
    auto minValidDocument = getMinValidDocument(opCtx, nss);
    ASSERT_TRUE(minValidDocument.hasField(StorageInterfaceImpl::kBeginFieldName));
    ASSERT_TRUE(minValidDocument[StorageInterfaceImpl::kBeginFieldName].isABSONObj());
    ASSERT_EQUALS(startOpTime,
                  unittest::assertGet(OpTime::parseFromOplogEntry(
                      minValidDocument[StorageInterfaceImpl::kBeginFieldName].Obj())));
    ASSERT_EQUALS(endOpTime, unittest::assertGet(OpTime::parseFromOplogEntry(minValidDocument)));
    ASSERT_EQUALS(
        endOpTime.getTimestamp(),
        minValidDocument[StorageInterfaceImpl::kOplogDeleteFromPointFieldName].timestamp());

    // Recovery unit will be owned by "opCtx".
    RecoveryUnitWithDurabilityTracking* recoveryUnit = new RecoveryUnitWithDurabilityTracking();
    opCtx->setRecoveryUnit(recoveryUnit, OperationContext::kNotInUnitOfWork);

    // Set min valid without waiting for the changes to be durable.
    OpTime endOpTime2({Seconds(789), 0}, 1LL);
    storage.setMinValid(opCtx, endOpTime2);
    storage.setAppliedThrough(opCtx, {});
    ASSERT_EQUALS(storage.getAppliedThrough(opCtx), OpTime());
    ASSERT_EQUALS(storage.getMinValid(opCtx), endOpTime2);
    ASSERT_FALSE(recoveryUnit->waitUntilDurableCalled);
}

TEST_F(StorageInterfaceImplTest, SnapshotSupported) {
    auto opCtx = getOperationContext();
    Status status = opCtx->recoveryUnit()->setReadFromMajorityCommittedSnapshot();
    ASSERT(status.isOK());
}

TEST_F(StorageInterfaceImplTest, InsertDocumentsReturnsOKWhenNoOperationsAreGiven) {
    auto opCtx = getOperationContext();
    auto nss = makeNamespace(_agent);
    createCollection(opCtx, nss);
    StorageInterfaceImpl storage(nss);
    ASSERT_OK(storage.insertDocuments(opCtx, nss, {}));
}

TEST_F(StorageInterfaceImplTest,
       InsertDocumentsReturnsInternalErrorWhenSavingOperationToNonOplogCollection) {
    // Create fake non-oplog collection to ensure saving oplog entries (without _id field) will
    // fail.
    auto opCtx = getOperationContext();
    auto nss = makeNamespace(_agent);
    createCollection(opCtx, nss);

    // Non-oplog collection will enforce mandatory _id field requirement on insertion.
    StorageInterfaceImpl storage(nss);
    auto op = makeOplogEntry({Timestamp(Seconds(1), 0), 1LL});
    auto status = storage.insertDocuments(opCtx, nss, {op});
    ASSERT_EQUALS(ErrorCodes::InternalError, status);
    ASSERT_STRING_CONTAINS(status.reason(), "Collection::insertDocument got document without _id");
}

TEST_F(StorageInterfaceImplTest,
       InsertDocumentsInsertsDocumentsOneAtATimeWhenAllAtOnceInsertingFails) {
    // Create a collection that does not support all-at-once inserting.
    auto opCtx = getOperationContext();
    auto nss = makeNamespace(_agent);
    CollectionOptions options;
    options.capped = true;
    options.cappedSize = 1024 * 1024;
    createCollection(opCtx, nss, options);
    // StorageInterfaceImpl::insertDocuments should fall back on inserting the batch one at a time.
    StorageInterfaceImpl storage(nss);
    auto doc1 = BSON("_id" << 1);
    auto doc2 = BSON("_id" << 2);
    std::vector<BSONObj> docs({doc1, doc2});
    // Confirm that Collection::insertDocuments fails to insert the batch all at once.
    {
        AutoGetCollection autoCollection(opCtx, nss, MODE_IX);
        WriteUnitOfWork wunit(opCtx);
        ASSERT_EQUALS(ErrorCodes::OperationCannotBeBatched,
                      autoCollection.getCollection()->insertDocuments(
                          opCtx, docs.begin(), docs.cend(), nullptr, false));
    }
    ASSERT_OK(storage.insertDocuments(opCtx, nss, docs));

    // Check collection contents. OplogInterface returns documents in reverse natural order.
    OplogInterfaceLocal oplog(opCtx, nss.ns());
    auto iter = oplog.makeIterator();
    ASSERT_BSONOBJ_EQ(doc2, unittest::assertGet(iter->next()).first);
    ASSERT_BSONOBJ_EQ(doc1, unittest::assertGet(iter->next()).first);
    ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, iter->next().getStatus());
}

TEST_F(StorageInterfaceImplTest, InsertDocumentsSavesOperationsReturnsOpTimeOfLastOperation) {
    // Create fake oplog collection to hold operations.
    auto opCtx = getOperationContext();
    auto nss = makeNamespace(_agent);
    createCollection(opCtx, nss, createOplogCollectionOptions());

    // Insert operations using storage interface. Ensure optime return is consistent with last
    // operation inserted.
    StorageInterfaceImpl storage;
    auto op1 = makeOplogEntry({Timestamp(Seconds(1), 0), 1LL});
    auto op2 = makeOplogEntry({Timestamp(Seconds(1), 0), 1LL});
    ASSERT_OK(storage.insertDocuments(opCtx, nss, {op1, op2}));

    // Check contents of oplog. OplogInterface iterates over oplog collection in reverse.
    repl::OplogInterfaceLocal oplog(opCtx, nss.ns());
    auto iter = oplog.makeIterator();
    ASSERT_BSONOBJ_EQ(op2, unittest::assertGet(iter->next()).first);
    ASSERT_BSONOBJ_EQ(op1, unittest::assertGet(iter->next()).first);
    ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, iter->next().getStatus());
}

TEST_F(StorageInterfaceImplTest,
       InsertDocumentsReturnsNamespaceNotFoundIfOplogCollectionDoesNotExist) {
    auto op = makeOplogEntry({Timestamp(Seconds(1), 0), 1LL});
    auto nss = makeNamespace(_agent);
    StorageInterfaceImpl storage;
    auto opCtx = getOperationContext();
    auto status = storage.insertDocuments(opCtx, nss, {op});
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound, status);
    ASSERT_STRING_CONTAINS(status.reason(), "The collection must exist before inserting documents");
}

TEST_F(StorageInterfaceImplTest, InsertMissingDocWorksOnExistingCappedCollection) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    CollectionOptions opts;
    opts.capped = true;
    opts.cappedSize = 1024 * 1024;
    createCollection(opCtx, nss, opts);
    ASSERT_OK(storage.insertDocument(opCtx, nss, BSON("_id" << 1)));
    AutoGetCollectionForReadCommand autoColl(opCtx, nss);
    ASSERT_TRUE(autoColl.getCollection());
}

TEST_F(StorageInterfaceImplTest, InsertMissingDocWorksOnExistingCollection) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    createCollection(opCtx, nss);
    ASSERT_OK(storage.insertDocument(opCtx, nss, BSON("_id" << 1)));
    AutoGetCollectionForReadCommand autoColl(opCtx, nss);
    ASSERT_TRUE(autoColl.getCollection());
}

TEST_F(StorageInterfaceImplTest, InsertMissingDocFailesIfCollectionIsMissing) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    const auto status = storage.insertDocument(opCtx, nss, BSON("_id" << 1));
    ASSERT_NOT_OK(status);
    ASSERT_EQ(status.code(), ErrorCodes::NamespaceNotFound);
}

TEST_F(StorageInterfaceImplTest, CreateCollectionWithIDIndexCommits) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    CollectionOptions opts;
    std::vector<BSONObj> indexes;
    auto loaderStatus =
        storage.createCollectionForBulkLoading(nss, opts, makeIdIndexSpec(nss), indexes);
    ASSERT_OK(loaderStatus.getStatus());
    auto loader = std::move(loaderStatus.getValue());
    std::vector<BSONObj> docs = {BSON("_id" << 1), BSON("_id" << 1), BSON("_id" << 2)};
    ASSERT_OK(loader->insertDocuments(docs.begin(), docs.end()));
    ASSERT_OK(loader->commit());

    AutoGetCollectionForReadCommand autoColl(opCtx, nss);
    auto coll = autoColl.getCollection();
    ASSERT(coll);
    ASSERT_EQ(coll->getRecordStore()->numRecords(opCtx), 2LL);
    auto collIdxCat = coll->getIndexCatalog();
    auto idIdxDesc = collIdxCat->findIdIndex(opCtx);
    auto count = getIndexKeyCount(opCtx, collIdxCat, idIdxDesc);
    ASSERT_EQ(count, 2LL);
}

void _testDestroyUncommitedCollectionBulkLoader(
    OperationContext* opCtx,
    const NamespaceString& nss,
    std::vector<BSONObj> secondaryIndexes,
    stdx::function<void(std::unique_ptr<CollectionBulkLoader> loader)> destroyLoaderFn) {
    StorageInterfaceImpl storage;
    CollectionOptions opts;
    auto loaderStatus =
        storage.createCollectionForBulkLoading(nss, opts, makeIdIndexSpec(nss), secondaryIndexes);
    ASSERT_OK(loaderStatus.getStatus());
    auto loader = std::move(loaderStatus.getValue());
    std::vector<BSONObj> docs = {BSON("_id" << 1)};
    ASSERT_OK(loader->insertDocuments(docs.begin(), docs.end()));

    // Destroy bulk loader.
    // Collection and ID index should not exist after 'loader' is destroyed.
    destroyLoaderFn(std::move(loader));

    AutoGetCollectionForReadCommand autoColl(opCtx, nss);
    auto coll = autoColl.getCollection();

    // Bulk loader is used to create indexes. The collection is not dropped when the bulk loader is
    // destroyed.
    ASSERT_TRUE(coll);
    ASSERT_EQ(1LL, coll->getRecordStore()->numRecords(opCtx));

    // IndexCatalog::numIndexesTotal() includes unfinished indexes. We need to ensure that
    // the bulk loader drops the unfinished indexes.
    auto collIdxCat = coll->getIndexCatalog();
    ASSERT_EQUALS(0, collIdxCat->numIndexesTotal(opCtx));
}

TEST_F(StorageInterfaceImplTest, DestroyingUncommittedCollectionBulkLoaderDropsIndexes) {
    auto opCtx = getOperationContext();
    auto nss = makeNamespace(_agent);
    std::vector<BSONObj> indexes = {BSON("v" << 1 << "key" << BSON("x" << 1) << "name"
                                             << "x_1"
                                             << "ns"
                                             << nss.ns())};
    auto destroyLoaderFn = [](std::unique_ptr<CollectionBulkLoader> loader) {
        // Destroy 'loader' by letting it go out of scope.
    };
    _testDestroyUncommitedCollectionBulkLoader(opCtx, nss, indexes, destroyLoaderFn);
}

TEST_F(StorageInterfaceImplTest, DestructorInitializesClientBeforeDestroyingIdIndexBuilder) {
    auto opCtx = getOperationContext();
    auto nss = makeNamespace(_agent);
    std::vector<BSONObj> indexes;
    auto destroyLoaderFn = [](std::unique_ptr<CollectionBulkLoader> loader) {
        // Destroy 'loader' in a new thread that does not have a Client.
        stdx::thread([&loader]() { loader.reset(); }).join();
    };
    _testDestroyUncommitedCollectionBulkLoader(opCtx, nss, indexes, destroyLoaderFn);
}

TEST_F(StorageInterfaceImplTest,
       DestructorInitializesClientBeforeDestroyingSecondaryIndexesBuilder) {
    auto opCtx = getOperationContext();
    auto nss = makeNamespace(_agent);
    std::vector<BSONObj> indexes = {BSON("v" << 1 << "key" << BSON("x" << 1) << "name"
                                             << "x_1"
                                             << "ns"
                                             << nss.ns())};
    auto destroyLoaderFn = [](std::unique_ptr<CollectionBulkLoader> loader) {
        // Destroy 'loader' in a new thread that does not have a Client.
        stdx::thread([&loader]() { loader.reset(); }).join();
    };
    _testDestroyUncommitedCollectionBulkLoader(opCtx, nss, indexes, destroyLoaderFn);
}

TEST_F(StorageInterfaceImplTest, CreateCollectionThatAlreadyExistsFails) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    NamespaceString nss("test.system.indexes");
    createCollection(opCtx, nss);

    const CollectionOptions opts{};
    const std::vector<BSONObj> indexes;
    const auto status =
        storage.createCollectionForBulkLoading(nss, opts, makeIdIndexSpec(nss), indexes);
    ASSERT_NOT_OK(status.getStatus());
}

TEST_F(StorageInterfaceImplTest, CreateOplogCreateCappedCollection) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    NamespaceString nss("local.oplog.X");
    {
        AutoGetCollectionForReadCommand autoColl(opCtx, nss);
        ASSERT_FALSE(autoColl.getCollection());
    }
    ASSERT_OK(storage.createOplog(opCtx, nss));
    {
        AutoGetCollectionForReadCommand autoColl(opCtx, nss);
        ASSERT_TRUE(autoColl.getCollection());
        ASSERT_EQ(nss.toString(), autoColl.getCollection()->ns().toString());
        ASSERT_TRUE(autoColl.getCollection()->isCapped());
    }
}

TEST_F(StorageInterfaceImplTest,
       CreateCollectionReturnsUserExceptionAsStatusIfCollectionCreationThrows) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    NamespaceString nss("local.oplog.Y");
    {
        AutoGetCollectionForReadCommand autoColl(opCtx, nss);
        ASSERT_FALSE(autoColl.getCollection());
    }

    auto status = storage.createCollection(opCtx, nss, CollectionOptions());
    ASSERT_EQUALS(ErrorCodes::fromInt(28838), status);
    ASSERT_STRING_CONTAINS(status.reason(), "cannot create a non-capped oplog collection");
}

TEST_F(StorageInterfaceImplTest, CreateCollectionFailsIfCollectionExists) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    {
        AutoGetCollectionForReadCommand autoColl(opCtx, nss);
        ASSERT_FALSE(autoColl.getCollection());
    }
    ASSERT_OK(storage.createCollection(opCtx, nss, CollectionOptions()));
    {
        AutoGetCollectionForReadCommand autoColl(opCtx, nss);
        ASSERT_TRUE(autoColl.getCollection());
        ASSERT_EQ(nss.toString(), autoColl.getCollection()->ns().toString());
    }
    auto status = storage.createCollection(opCtx, nss, CollectionOptions());
    ASSERT_EQUALS(ErrorCodes::NamespaceExists, status);
    ASSERT_STRING_CONTAINS(status.reason(),
                           str::stream() << "Collection " << nss.ns() << " already exists");
}

TEST_F(StorageInterfaceImplTest, DropCollectionWorksWithExistingWithDataCollection) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    createCollection(opCtx, nss);
    ASSERT_OK(storage.insertDocument(opCtx, nss, BSON("_id" << 1)));
    ASSERT_OK(storage.dropCollection(opCtx, nss));
}

TEST_F(StorageInterfaceImplTest, DropCollectionWorksWithExistingEmptyCollection) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    createCollection(opCtx, nss);
    ASSERT_OK(storage.dropCollection(opCtx, nss));
    AutoGetCollectionForReadCommand autoColl(opCtx, nss);
    ASSERT_FALSE(autoColl.getCollection());
}

TEST_F(StorageInterfaceImplTest, DropCollectionWorksWithMissingCollection) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_FALSE(AutoGetDb(opCtx, nss.db(), MODE_IS).getDb());
    ASSERT_OK(storage.dropCollection(opCtx, nss));
    ASSERT_FALSE(AutoGetCollectionForReadCommand(opCtx, nss).getCollection());
    // Database should not be created after running dropCollection.
    ASSERT_FALSE(AutoGetDb(opCtx, nss.db(), MODE_IS).getDb());
}

TEST_F(StorageInterfaceImplTest, FindDocumentsReturnsInvalidNamespaceIfCollectionIsMissing) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    auto indexName = "_id_"_sd;
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound,
                  storage
                      .findDocuments(opCtx,
                                     nss,
                                     indexName,
                                     StorageInterface::ScanDirection::kForward,
                                     {},
                                     BoundInclusion::kIncludeStartKeyOnly,
                                     1U)
                      .getStatus());
}

TEST_F(StorageInterfaceImplTest, FindDocumentsReturnsIndexNotFoundIfIndexIsMissing) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    auto indexName = "nonexistent"_sd;
    ASSERT_OK(storage.createCollection(opCtx, nss, CollectionOptions()));
    ASSERT_EQUALS(ErrorCodes::IndexNotFound,
                  storage
                      .findDocuments(opCtx,
                                     nss,
                                     indexName,
                                     StorageInterface::ScanDirection::kForward,
                                     {},
                                     BoundInclusion::kIncludeStartKeyOnly,
                                     1U)
                      .getStatus());
}

TEST_F(StorageInterfaceImplTest, FindDocumentsReturnsIndexOptionsConflictIfIndexIsAPartialIndex) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    std::vector<BSONObj> indexes = {BSON("v" << 1 << "key" << BSON("x" << 1) << "name"
                                             << "x_1"
                                             << "ns"
                                             << nss.ns()
                                             << "partialFilterExpression"
                                             << BSON("y" << 1))};
    auto loader = unittest::assertGet(storage.createCollectionForBulkLoading(
        nss, CollectionOptions(), makeIdIndexSpec(nss), indexes));
    std::vector<BSONObj> docs = {BSON("_id" << 1), BSON("_id" << 1), BSON("_id" << 2)};
    ASSERT_OK(loader->insertDocuments(docs.begin(), docs.end()));
    ASSERT_OK(loader->commit());
    auto indexName = "x_1"_sd;
    ASSERT_EQUALS(ErrorCodes::IndexOptionsConflict,
                  storage
                      .findDocuments(opCtx,
                                     nss,
                                     indexName,
                                     StorageInterface::ScanDirection::kForward,
                                     {},
                                     BoundInclusion::kIncludeStartKeyOnly,
                                     1U)
                      .getStatus());
}

TEST_F(StorageInterfaceImplTest, FindDocumentsReturnsEmptyVectorIfCollectionIsEmpty) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    auto indexName = "_id_"_sd;
    ASSERT_OK(storage.createCollection(opCtx, nss, CollectionOptions()));
    ASSERT_TRUE(unittest::assertGet(storage.findDocuments(opCtx,
                                                          nss,
                                                          indexName,
                                                          StorageInterface::ScanDirection::kForward,
                                                          {},
                                                          BoundInclusion::kIncludeStartKeyOnly,
                                                          1U))
                    .empty());
}

std::string _toString(const std::vector<BSONObj>& docs) {
    str::stream ss;
    ss << "[";
    bool first = true;
    for (const auto& doc : docs) {
        if (first) {
            ss << doc;
            first = false;
        } else {
            ss << ", " << doc;
        }
    }
    ss << "]";
    return ss;
}

/**
 * Check collection contents. OplogInterface returns documents in reverse natural order.
 */
void _assertDocumentsInCollectionEquals(OperationContext* opCtx,
                                        const NamespaceString& nss,
                                        const std::vector<BSONObj>& docs) {
    std::vector<BSONObj> reversedDocs(docs);
    std::reverse(reversedDocs.begin(), reversedDocs.end());
    OplogInterfaceLocal oplog(opCtx, nss.ns());
    auto iter = oplog.makeIterator();
    for (const auto& doc : reversedDocs) {
        ASSERT_BSONOBJ_EQ(doc, unittest::assertGet(iter->next()).first);
    }
    ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, iter->next().getStatus());
}

/**
 * Check StatusWith<std::vector<BSONObj>> value.
 */
void _assertDocumentsEqual(const StatusWith<std::vector<BSONObj>>& statusWithDocs,
                           const std::vector<BSONObj>& expectedDocs) {
    const auto actualDocs = unittest::assertGet(statusWithDocs);
    auto iter = actualDocs.cbegin();
    std::string msg = str::stream() << "expected: " << _toString(expectedDocs)
                                    << "; actual: " << _toString(actualDocs);
    for (const auto& doc : expectedDocs) {
        ASSERT_TRUE(iter != actualDocs.cend()) << msg;
        ASSERT_BSONOBJ_EQ(doc, *(iter++));
    }
    ASSERT_TRUE(iter == actualDocs.cend()) << msg;
}

/**
 * Returns first BSONObj from a StatusWith<std::vector<BSONObj>>.
 */
BSONObj _assetGetFront(const StatusWith<std::vector<BSONObj>>& statusWithDocs) {
    auto&& docs = statusWithDocs.getValue();
    ASSERT_FALSE(docs.empty());
    return docs.front();
}

TEST_F(StorageInterfaceImplTest,
       FindDocumentsReturnsDocumentWithLowestKeyValueIfScanDirectionIsForward) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    auto indexName = "_id_"_sd;
    ASSERT_OK(storage.createCollection(opCtx, nss, CollectionOptions()));
    ASSERT_OK(storage.insertDocuments(opCtx,
                                      nss,
                                      {BSON("_id" << 0),
                                       BSON("_id" << 1),
                                       BSON("_id" << 2),
                                       BSON("_id" << 3),
                                       BSON("_id" << 4)}));

    // startKey not provided
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 0),
        _assetGetFront(storage.findDocuments(opCtx,
                                             nss,
                                             indexName,
                                             StorageInterface::ScanDirection::kForward,
                                             {},
                                             BoundInclusion::kIncludeStartKeyOnly,
                                             1U)));

    // startKey not provided. limit is 0.
    _assertDocumentsEqual(storage.findDocuments(opCtx,
                                                nss,
                                                indexName,
                                                StorageInterface::ScanDirection::kForward,
                                                {},
                                                BoundInclusion::kIncludeStartKeyOnly,
                                                0U),
                          {});

    // startKey not provided. limit of 2.
    _assertDocumentsEqual(storage.findDocuments(opCtx,
                                                nss,
                                                indexName,
                                                StorageInterface::ScanDirection::kForward,
                                                {},
                                                BoundInclusion::kIncludeStartKeyOnly,
                                                2U),
                          {BSON("_id" << 0), BSON("_id" << 1)});

    // startKey provided; include start key
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 0),
        _assetGetFront(storage.findDocuments(opCtx,
                                             nss,
                                             indexName,
                                             StorageInterface::ScanDirection::kForward,
                                             BSON("" << 0),
                                             BoundInclusion::kIncludeStartKeyOnly,
                                             1U)));
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 1),
        _assetGetFront(storage.findDocuments(opCtx,
                                             nss,
                                             indexName,
                                             StorageInterface::ScanDirection::kForward,
                                             BSON("" << 1),
                                             BoundInclusion::kIncludeStartKeyOnly,
                                             1U)));

    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 1),
        _assetGetFront(storage.findDocuments(opCtx,
                                             nss,
                                             indexName,
                                             StorageInterface::ScanDirection::kForward,
                                             BSON("" << 0.5),
                                             BoundInclusion::kIncludeStartKeyOnly,
                                             1U)));

    // startKey provided; include both start and end keys
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 1),
        _assetGetFront(storage.findDocuments(opCtx,
                                             nss,
                                             indexName,
                                             StorageInterface::ScanDirection::kForward,
                                             BSON("" << 1),
                                             BoundInclusion::kIncludeStartKeyOnly,
                                             1U)));

    // startKey provided; exclude start key
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 2),
        _assetGetFront(storage.findDocuments(opCtx,
                                             nss,
                                             indexName,
                                             StorageInterface::ScanDirection::kForward,
                                             BSON("" << 1),
                                             BoundInclusion::kIncludeEndKeyOnly,
                                             1U)));

    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 2),
        _assetGetFront(storage.findDocuments(opCtx,
                                             nss,
                                             indexName,
                                             StorageInterface::ScanDirection::kForward,
                                             BSON("" << 1.5),
                                             BoundInclusion::kIncludeEndKeyOnly,
                                             1U)));

    // startKey provided; exclude both start and end keys
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 2),
        _assetGetFront(storage.findDocuments(opCtx,
                                             nss,
                                             indexName,
                                             StorageInterface::ScanDirection::kForward,
                                             BSON("" << 1),
                                             BoundInclusion::kExcludeBothStartAndEndKeys,
                                             1U)));

    // startKey provided; exclude both start and end keys.
    // A limit of 3 should return 2 documents because we reached the end of the collection.
    _assertDocumentsEqual(storage.findDocuments(opCtx,
                                                nss,
                                                indexName,
                                                StorageInterface::ScanDirection::kForward,
                                                BSON("" << 2),
                                                BoundInclusion::kExcludeBothStartAndEndKeys,
                                                3U),
                          {BSON("_id" << 3), BSON("_id" << 4)});

    _assertDocumentsInCollectionEquals(
        opCtx,
        nss,
        {BSON("_id" << 0), BSON("_id" << 1), BSON("_id" << 2), BSON("_id" << 3), BSON("_id" << 4)});
}

TEST_F(StorageInterfaceImplTest,
       FindDocumentsReturnsDocumentWithHighestKeyValueIfScanDirectionIsBackward) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    auto indexName = "_id_"_sd;
    ASSERT_OK(storage.createCollection(opCtx, nss, CollectionOptions()));
    ASSERT_OK(storage.insertDocuments(opCtx,
                                      nss,
                                      {BSON("_id" << 0),
                                       BSON("_id" << 1),
                                       BSON("_id" << 2),
                                       BSON("_id" << 3),
                                       BSON("_id" << 4)}));

    // startKey not provided
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 4),
        _assetGetFront(storage.findDocuments(opCtx,
                                             nss,
                                             indexName,
                                             StorageInterface::ScanDirection::kBackward,
                                             {},
                                             BoundInclusion::kIncludeStartKeyOnly,
                                             1U)));

    // startKey not provided. limit is 0.
    _assertDocumentsEqual(storage.findDocuments(opCtx,
                                                nss,
                                                indexName,
                                                StorageInterface::ScanDirection::kBackward,
                                                {},
                                                BoundInclusion::kIncludeStartKeyOnly,
                                                0U),
                          {});

    // startKey not provided. limit of 2.
    _assertDocumentsEqual(storage.findDocuments(opCtx,
                                                nss,
                                                indexName,
                                                StorageInterface::ScanDirection::kBackward,
                                                {},
                                                BoundInclusion::kIncludeStartKeyOnly,
                                                2U),
                          {BSON("_id" << 4), BSON("_id" << 3)});

    // startKey provided; include start key
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 4),
        _assetGetFront(storage.findDocuments(opCtx,
                                             nss,
                                             indexName,
                                             StorageInterface::ScanDirection::kBackward,
                                             BSON("" << 4),
                                             BoundInclusion::kIncludeStartKeyOnly,
                                             1U)));
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 3),
        _assetGetFront(storage.findDocuments(opCtx,
                                             nss,
                                             indexName,
                                             StorageInterface::ScanDirection::kBackward,
                                             BSON("" << 3),
                                             BoundInclusion::kIncludeStartKeyOnly,
                                             1U)));

    // startKey provided; include both start and end keys
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 4),
        _assetGetFront(storage.findDocuments(opCtx,
                                             nss,
                                             indexName,
                                             StorageInterface::ScanDirection::kBackward,
                                             BSON("" << 4),
                                             BoundInclusion::kIncludeBothStartAndEndKeys,
                                             1U)));

    // startKey provided; exclude start key
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 2),
        _assetGetFront(storage.findDocuments(opCtx,
                                             nss,
                                             indexName,
                                             StorageInterface::ScanDirection::kBackward,
                                             BSON("" << 3),
                                             BoundInclusion::kIncludeEndKeyOnly,
                                             1U)));

    // startKey provided; exclude both start and end keys
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 2),
        _assetGetFront(storage.findDocuments(opCtx,
                                             nss,
                                             indexName,
                                             StorageInterface::ScanDirection::kBackward,
                                             BSON("" << 3),
                                             BoundInclusion::kExcludeBothStartAndEndKeys,
                                             1U)));

    // startKey provided; exclude both start and end keys.
    // A limit of 3 should return 2 documents because we reached the beginning of the collection.
    _assertDocumentsEqual(storage.findDocuments(opCtx,
                                                nss,
                                                indexName,
                                                StorageInterface::ScanDirection::kBackward,
                                                BSON("" << 2),
                                                BoundInclusion::kExcludeBothStartAndEndKeys,
                                                3U),
                          {BSON("_id" << 1), BSON("_id" << 0)});

    _assertDocumentsInCollectionEquals(
        opCtx,
        nss,
        {BSON("_id" << 0), BSON("_id" << 1), BSON("_id" << 2), BSON("_id" << 3), BSON("_id" << 4)});
}

TEST_F(StorageInterfaceImplTest,
       FindDocumentsCollScanReturnsFirstDocumentInsertedIfScanDirectionIsForward) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_OK(storage.createCollection(opCtx, nss, CollectionOptions()));
    ASSERT_OK(storage.insertDocuments(
        opCtx, nss, {BSON("_id" << 1), BSON("_id" << 2), BSON("_id" << 0)}));
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 1),
        _assetGetFront(storage.findDocuments(opCtx,
                                             nss,
                                             boost::none,
                                             StorageInterface::ScanDirection::kForward,
                                             {},
                                             BoundInclusion::kIncludeStartKeyOnly,
                                             1U)));

    // Check collection contents. OplogInterface returns documents in reverse natural order.
    OplogInterfaceLocal oplog(opCtx, nss.ns());
    auto iter = oplog.makeIterator();
    ASSERT_BSONOBJ_EQ(BSON("_id" << 0), unittest::assertGet(iter->next()).first);
    ASSERT_BSONOBJ_EQ(BSON("_id" << 2), unittest::assertGet(iter->next()).first);
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), unittest::assertGet(iter->next()).first);
    ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, iter->next().getStatus());
}

TEST_F(StorageInterfaceImplTest,
       FindDocumentsCollScanReturnsLastDocumentInsertedIfScanDirectionIsBackward) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_OK(storage.createCollection(opCtx, nss, CollectionOptions()));
    ASSERT_OK(storage.insertDocuments(
        opCtx, nss, {BSON("_id" << 1), BSON("_id" << 2), BSON("_id" << 0)}));
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 0),
        _assetGetFront(storage.findDocuments(opCtx,
                                             nss,
                                             boost::none,
                                             StorageInterface::ScanDirection::kBackward,
                                             {},
                                             BoundInclusion::kIncludeStartKeyOnly,
                                             1U)));

    _assertDocumentsInCollectionEquals(
        opCtx, nss, {BSON("_id" << 1), BSON("_id" << 2), BSON("_id" << 0)});
}

TEST_F(StorageInterfaceImplTest, FindDocumentsCollScanReturnsNoSuchKeyIfStartKeyIsNotEmpty) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_OK(storage.createCollection(opCtx, nss, CollectionOptions()));
    ASSERT_OK(storage.insertDocuments(
        opCtx, nss, {BSON("_id" << 1), BSON("_id" << 2), BSON("_id" << 0)}));
    ASSERT_EQUALS(ErrorCodes::NoSuchKey,
                  storage
                      .findDocuments(opCtx,
                                     nss,
                                     boost::none,
                                     StorageInterface::ScanDirection::kForward,
                                     BSON("" << 1),
                                     BoundInclusion::kIncludeStartKeyOnly,
                                     1U)
                      .getStatus());
}

TEST_F(StorageInterfaceImplTest,
       FindDocumentsCollScanReturnsInvalidOptionsIfBoundIsNotStartKeyOnly) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_OK(storage.createCollection(opCtx, nss, CollectionOptions()));
    ASSERT_OK(storage.insertDocuments(
        opCtx, nss, {BSON("_id" << 1), BSON("_id" << 2), BSON("_id" << 0)}));
    ASSERT_EQUALS(ErrorCodes::InvalidOptions,
                  storage
                      .findDocuments(opCtx,
                                     nss,
                                     boost::none,
                                     StorageInterface::ScanDirection::kForward,
                                     {},
                                     BoundInclusion::kIncludeEndKeyOnly,
                                     1U)
                      .getStatus());
}

TEST_F(StorageInterfaceImplTest, DeleteDocumentsReturnsInvalidNamespaceIfCollectionIsMissing) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    auto indexName = "_id_"_sd;
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound,
                  storage
                      .deleteDocuments(opCtx,
                                       nss,
                                       indexName,
                                       StorageInterface::ScanDirection::kForward,
                                       {},
                                       BoundInclusion::kIncludeStartKeyOnly,
                                       1U)
                      .getStatus());
}

TEST_F(StorageInterfaceImplTest, DeleteDocumentsReturnsIndexNotFoundIfIndexIsMissing) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    auto indexName = "nonexistent"_sd;
    ASSERT_OK(storage.createCollection(opCtx, nss, CollectionOptions()));
    ASSERT_EQUALS(ErrorCodes::IndexNotFound,
                  storage
                      .deleteDocuments(opCtx,
                                       nss,
                                       indexName,
                                       StorageInterface::ScanDirection::kForward,
                                       {},
                                       BoundInclusion::kIncludeStartKeyOnly,
                                       1U)
                      .getStatus());
}

TEST_F(StorageInterfaceImplTest, DeleteDocumentsReturnsEmptyVectorIfCollectionIsEmpty) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    auto indexName = "_id_"_sd;
    ASSERT_OK(storage.createCollection(opCtx, nss, CollectionOptions()));
    ASSERT_TRUE(
        unittest::assertGet(storage.deleteDocuments(opCtx,
                                                    nss,
                                                    indexName,
                                                    StorageInterface::ScanDirection::kForward,
                                                    {},
                                                    BoundInclusion::kIncludeStartKeyOnly,
                                                    1U))
            .empty());
}

TEST_F(StorageInterfaceImplTest,
       DeleteDocumentsReturnsDocumentWithLowestKeyValueIfScanDirectionIsForward) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    auto indexName = "_id_"_sd;
    ASSERT_OK(storage.createCollection(opCtx, nss, CollectionOptions()));
    ASSERT_OK(storage.insertDocuments(opCtx,
                                      nss,
                                      {BSON("_id" << 0),
                                       BSON("_id" << 1),
                                       BSON("_id" << 2),
                                       BSON("_id" << 3),
                                       BSON("_id" << 4),
                                       BSON("_id" << 5),
                                       BSON("_id" << 6),
                                       BSON("_id" << 7)}));

    // startKey not provided
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 0),
        _assetGetFront(storage.deleteDocuments(opCtx,
                                               nss,
                                               indexName,
                                               StorageInterface::ScanDirection::kForward,
                                               {},
                                               BoundInclusion::kIncludeStartKeyOnly,
                                               1U)));

    _assertDocumentsInCollectionEquals(opCtx,
                                       nss,
                                       {BSON("_id" << 1),
                                        BSON("_id" << 2),
                                        BSON("_id" << 3),
                                        BSON("_id" << 4),
                                        BSON("_id" << 5),
                                        BSON("_id" << 6),
                                        BSON("_id" << 7)});

    // startKey not provided. limit is 0.
    _assertDocumentsEqual(storage.deleteDocuments(opCtx,
                                                  nss,
                                                  indexName,
                                                  StorageInterface::ScanDirection::kForward,
                                                  {},
                                                  BoundInclusion::kIncludeStartKeyOnly,
                                                  0U),
                          {});

    _assertDocumentsInCollectionEquals(opCtx,
                                       nss,
                                       {BSON("_id" << 1),
                                        BSON("_id" << 2),
                                        BSON("_id" << 3),
                                        BSON("_id" << 4),
                                        BSON("_id" << 5),
                                        BSON("_id" << 6),
                                        BSON("_id" << 7)});

    // startKey provided; include start key
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 2),
        _assetGetFront(storage.deleteDocuments(opCtx,
                                               nss,
                                               indexName,
                                               StorageInterface::ScanDirection::kForward,
                                               BSON("" << 2),
                                               BoundInclusion::kIncludeStartKeyOnly,
                                               1U)));

    _assertDocumentsInCollectionEquals(opCtx,
                                       nss,
                                       {BSON("_id" << 1),
                                        BSON("_id" << 3),
                                        BSON("_id" << 4),
                                        BSON("_id" << 5),
                                        BSON("_id" << 6),
                                        BSON("_id" << 7)});

    // startKey provided; exclude start key
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 5),
        _assetGetFront(storage.deleteDocuments(opCtx,
                                               nss,
                                               indexName,
                                               StorageInterface::ScanDirection::kForward,
                                               BSON("" << 4),
                                               BoundInclusion::kIncludeEndKeyOnly,
                                               1U)));

    _assertDocumentsInCollectionEquals(
        opCtx,
        nss,
        {BSON("_id" << 1), BSON("_id" << 3), BSON("_id" << 4), BSON("_id" << 6), BSON("_id" << 7)});

    // startKey provided; exclude start key.
    // A limit of 3 should return 2 documents because we reached the end of the collection.
    _assertDocumentsEqual(storage.deleteDocuments(opCtx,
                                                  nss,
                                                  indexName,
                                                  StorageInterface::ScanDirection::kForward,
                                                  BSON("" << 4),
                                                  BoundInclusion::kIncludeEndKeyOnly,
                                                  3U),
                          {BSON("_id" << 6), BSON("_id" << 7)});

    _assertDocumentsInCollectionEquals(
        opCtx, nss, {BSON("_id" << 1), BSON("_id" << 3), BSON("_id" << 4)});
}

TEST_F(StorageInterfaceImplTest,
       DeleteDocumentsReturnsDocumentWithHighestKeyValueIfScanDirectionIsBackward) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    auto indexName = "_id_"_sd;
    ASSERT_OK(storage.createCollection(opCtx, nss, CollectionOptions()));
    ASSERT_OK(storage.insertDocuments(opCtx,
                                      nss,
                                      {BSON("_id" << 0),
                                       BSON("_id" << 1),
                                       BSON("_id" << 2),
                                       BSON("_id" << 3),
                                       BSON("_id" << 4),
                                       BSON("_id" << 5),
                                       BSON("_id" << 6),
                                       BSON("_id" << 7)}));

    // startKey not provided
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 7),
        _assetGetFront(storage.deleteDocuments(opCtx,
                                               nss,
                                               indexName,
                                               StorageInterface::ScanDirection::kBackward,
                                               {},
                                               BoundInclusion::kIncludeStartKeyOnly,
                                               1U)));

    _assertDocumentsInCollectionEquals(opCtx,
                                       nss,
                                       {BSON("_id" << 0),
                                        BSON("_id" << 1),
                                        BSON("_id" << 2),
                                        BSON("_id" << 3),
                                        BSON("_id" << 4),
                                        BSON("_id" << 5),
                                        BSON("_id" << 6)});

    // startKey not provided. limit is 0.
    _assertDocumentsEqual(storage.deleteDocuments(opCtx,
                                                  nss,
                                                  indexName,
                                                  StorageInterface::ScanDirection::kBackward,
                                                  {},
                                                  BoundInclusion::kIncludeStartKeyOnly,
                                                  0U),
                          {});

    _assertDocumentsInCollectionEquals(opCtx,
                                       nss,
                                       {BSON("_id" << 0),
                                        BSON("_id" << 1),
                                        BSON("_id" << 2),
                                        BSON("_id" << 3),
                                        BSON("_id" << 4),
                                        BSON("_id" << 5),
                                        BSON("_id" << 6)});

    // startKey provided; include start key
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 5),
        _assetGetFront(storage.deleteDocuments(opCtx,
                                               nss,
                                               indexName,
                                               StorageInterface::ScanDirection::kBackward,
                                               BSON("" << 5),
                                               BoundInclusion::kIncludeStartKeyOnly,
                                               1U)));

    _assertDocumentsInCollectionEquals(opCtx,
                                       nss,
                                       {BSON("_id" << 0),
                                        BSON("_id" << 1),
                                        BSON("_id" << 2),
                                        BSON("_id" << 3),
                                        BSON("_id" << 4),
                                        BSON("_id" << 6)});

    // startKey provided; exclude start key
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 2),
        _assetGetFront(storage.deleteDocuments(opCtx,
                                               nss,
                                               indexName,
                                               StorageInterface::ScanDirection::kBackward,
                                               BSON("" << 3),
                                               BoundInclusion::kIncludeEndKeyOnly,
                                               1U)));

    _assertDocumentsInCollectionEquals(
        opCtx,
        nss,
        {BSON("_id" << 0), BSON("_id" << 1), BSON("_id" << 3), BSON("_id" << 4), BSON("_id" << 6)});

    // startKey provided; exclude start key.
    // A limit of 3 should return 2 documents because we reached the beginning of the collection.
    _assertDocumentsEqual(storage.deleteDocuments(opCtx,
                                                  nss,
                                                  indexName,
                                                  StorageInterface::ScanDirection::kBackward,
                                                  BSON("" << 3),
                                                  BoundInclusion::kIncludeEndKeyOnly,
                                                  3U),
                          {BSON("_id" << 1), BSON("_id" << 0)});

    _assertDocumentsInCollectionEquals(
        opCtx, nss, {BSON("_id" << 3), BSON("_id" << 4), BSON("_id" << 6)});
}

TEST_F(StorageInterfaceImplTest,
       DeleteDocumentsCollScanReturnsFirstDocumentInsertedIfScanDirectionIsForward) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_OK(storage.createCollection(opCtx, nss, CollectionOptions()));
    ASSERT_OK(storage.insertDocuments(
        opCtx, nss, {BSON("_id" << 1), BSON("_id" << 2), BSON("_id" << 0)}));
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 1),
        _assetGetFront(storage.deleteDocuments(opCtx,
                                               nss,
                                               boost::none,
                                               StorageInterface::ScanDirection::kForward,
                                               {},
                                               BoundInclusion::kIncludeStartKeyOnly,
                                               1U)));

    _assertDocumentsInCollectionEquals(opCtx, nss, {BSON("_id" << 2), BSON("_id" << 0)});
}

TEST_F(StorageInterfaceImplTest,
       DeleteDocumentsCollScanReturnsLastDocumentInsertedIfScanDirectionIsBackward) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_OK(storage.createCollection(opCtx, nss, CollectionOptions()));
    ASSERT_OK(storage.insertDocuments(
        opCtx, nss, {BSON("_id" << 1), BSON("_id" << 2), BSON("_id" << 0)}));
    ASSERT_BSONOBJ_EQ(
        BSON("_id" << 0),
        _assetGetFront(storage.deleteDocuments(opCtx,
                                               nss,
                                               boost::none,
                                               StorageInterface::ScanDirection::kBackward,
                                               {},
                                               BoundInclusion::kIncludeStartKeyOnly,
                                               1U)));

    _assertDocumentsInCollectionEquals(opCtx, nss, {BSON("_id" << 1), BSON("_id" << 2)});
}

TEST_F(StorageInterfaceImplTest, DeleteDocumentsCollScanReturnsNoSuchKeyIfStartKeyIsNotEmpty) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_OK(storage.createCollection(opCtx, nss, CollectionOptions()));
    ASSERT_OK(storage.insertDocuments(
        opCtx, nss, {BSON("_id" << 1), BSON("_id" << 2), BSON("_id" << 0)}));
    ASSERT_EQUALS(ErrorCodes::NoSuchKey,
                  storage
                      .deleteDocuments(opCtx,
                                       nss,
                                       boost::none,
                                       StorageInterface::ScanDirection::kForward,
                                       BSON("" << 1),
                                       BoundInclusion::kIncludeStartKeyOnly,
                                       1U)
                      .getStatus());
}

TEST_F(StorageInterfaceImplTest,
       DeleteDocumentsCollScanReturnsInvalidOptionsIfBoundIsNotStartKeyOnly) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_OK(storage.createCollection(opCtx, nss, CollectionOptions()));
    ASSERT_OK(storage.insertDocuments(
        opCtx, nss, {BSON("_id" << 1), BSON("_id" << 2), BSON("_id" << 0)}));
    ASSERT_EQUALS(ErrorCodes::InvalidOptions,
                  storage
                      .deleteDocuments(opCtx,
                                       nss,
                                       boost::none,
                                       StorageInterface::ScanDirection::kForward,
                                       {},
                                       BoundInclusion::kIncludeEndKeyOnly,
                                       1U)
                      .getStatus());
}

TEST_F(StorageInterfaceImplTest,
       GetCollectionCountReturnsNamespaceNotFoundWhenDatabaseDoesNotExist) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    NamespaceString nss("nosuchdb.coll");
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound,
                  storage.getCollectionCount(opCtx, nss).getStatus());
}

TEST_F(StorageInterfaceImplTest,
       GetCollectionCountReturnsNamespaceNotFoundWhenCollectionDoesNotExist) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    NamespaceString wrongColl(nss.db(), "wrongColl"_sd);
    ASSERT_OK(storage.createCollection(opCtx, nss, CollectionOptions()));
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound,
                  storage.getCollectionCount(opCtx, wrongColl).getStatus());
}

TEST_F(StorageInterfaceImplTest, GetCollectionCountReturnsZeroOnEmptyCollection) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_OK(storage.createCollection(opCtx, nss, CollectionOptions()));
    auto count = unittest::assertGet(storage.getCollectionCount(opCtx, nss));
    ASSERT_EQUALS(0UL, count);
}

TEST_F(StorageInterfaceImplTest, GetCollectionCountReturnsCollectionCount) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_OK(storage.createCollection(opCtx, nss, CollectionOptions()));
    ASSERT_OK(storage.insertDocuments(
        opCtx, nss, {BSON("_id" << 1), BSON("_id" << 2), BSON("_id" << 0)}));
    auto count = unittest::assertGet(storage.getCollectionCount(opCtx, nss));
    ASSERT_EQUALS(3UL, count);
}

TEST_F(StorageInterfaceImplTest,
       GetCollectionSizeReturnsNamespaceNotFoundWhenDatabaseDoesNotExist) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound, storage.getCollectionSize(opCtx, nss).getStatus());
}

TEST_F(StorageInterfaceImplTest,
       GetCollectionSizeReturnsNamespaceNotFoundWhenCollectionDoesNotExist) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    NamespaceString wrongColl(nss.db(), "wrongColl"_sd);
    ASSERT_OK(storage.createCollection(opCtx, nss, CollectionOptions()));
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound,
                  storage.getCollectionSize(opCtx, wrongColl).getStatus());
}

TEST_F(StorageInterfaceImplTest, GetCollectionSizeReturnsZeroOnEmptyCollection) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_OK(storage.createCollection(opCtx, nss, CollectionOptions()));
    auto size = unittest::assertGet(storage.getCollectionSize(opCtx, nss));
    ASSERT_EQUALS(0UL, size);
}

TEST_F(StorageInterfaceImplTest, GetCollectionSizeReturnsCollectionSize) {
    auto opCtx = getOperationContext();
    StorageInterfaceImpl storage;
    auto nss = makeNamespace(_agent);
    ASSERT_OK(storage.createCollection(opCtx, nss, CollectionOptions()));
    ASSERT_OK(storage.insertDocuments(
        opCtx, nss, {BSON("_id" << 1), BSON("_id" << 2), BSON("_id" << 0)}));
    auto size = unittest::assertGet(storage.getCollectionSize(opCtx, nss));
    ASSERT_NOT_EQUALS(0UL, size);
}

}  // namespace
