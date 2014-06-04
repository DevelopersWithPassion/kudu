// Copyright (c) 2013, Cloudera, inc.

#include <gflags/gflags.h>
#include <gtest/gtest.h>
#include <boost/foreach.hpp>
#include <boost/assign/list_of.hpp>

#include "client/client.h"
#include "common/wire_protocol-test-util.h"
#include "common/schema.h"
#include "consensus/consensus_queue.h"
#include "consensus/raft_consensus.h"
#include "consensus/raft_consensus_state.h"
#include "integration-tests/mini_cluster.h"
#include "master/catalog_manager.h"
#include "master/mini_master.h"
#include "master/master.proxy.h"
#include "server/metadata.pb.h"
#include "tserver/tablet_server.h"
#include "tserver/mini_tablet_server.h"
#include "tserver/tablet_server-test-base.h"
#include "util/random_util.h"
#include "util/stopwatch.h"
#include "util/test_util.h"

DECLARE_int32(default_num_replicas);
DECLARE_int32(consensus_entry_cache_size_soft_limit_mb);
DECLARE_int32(consensus_entry_cache_size_hard_limit_mb);

DEFINE_int32(num_client_threads, 8,
             "Number of client threads to launch");
DEFINE_int64(client_inserts_per_thread, 1000,
             "Number of rows inserted by each client thread");
DEFINE_int64(client_num_batches_per_thread, 100,
             "In how many batches to group the rows, for each client");

namespace kudu {

namespace tserver {

using consensus::RaftConsensus;
using consensus::ReplicaState;
using client::KuduClientOptions;
using client::KuduClient;
using client::KuduTable;
using master::GetTableLocationsRequestPB;
using master::GetTableLocationsResponsePB;
using master::TableIdentifierPB;
using master::TabletLocationsPB;
using master::MiniMaster;
using metadata::QuorumPB;
using metadata::QuorumPeerPB;
using std::vector;
using std::tr1::shared_ptr;
using tserver::TabletServer;

static const int kMaxRetries = 20;
static const int kNumReplicas = 3;

// Integration test for distributed consensus.
class DistConsensusTest : public TabletServerTest {
 public:
  DistConsensusTest()
      : inserters_(FLAGS_num_client_threads) {
  }

  struct ProxyDetails {
    master::TSInfoPB ts_info;
    gscoped_ptr<TabletServerServiceProxy> proxy;
  };

  virtual void SetUp() {
    FLAGS_consensus_entry_cache_size_soft_limit_mb = 5;
    FLAGS_consensus_entry_cache_size_hard_limit_mb = 10;
    KuduTest::SetUp();
    CreateCluster();
    CreateClient();
    WaitForAndGetQuorum();
  }

  void CreateCluster() {
    FLAGS_default_num_replicas = kNumReplicas;
    cluster_.reset(new MiniCluster(env_.get(), test_dir_, kNumReplicas));
    ASSERT_STATUS_OK(cluster_->Start());
    ASSERT_STATUS_OK(cluster_->WaitForTabletServerCount(kNumReplicas));
    CreateTestSchema(&schema_);
  }

  void CreateClient() {
    // Connect to the cluster.
    KuduClientOptions opts;
    opts.master_server_addr = cluster_->mini_master()->bound_rpc_addr().ToString();
    ASSERT_STATUS_OK(KuduClient::Create(opts, &client_));
    // Create a table with a single tablet, with three replicas
    ASSERT_STATUS_OK(client_->CreateTable(kTableId, schema_));
    ASSERT_STATUS_OK(client_->OpenTable(kTableId, &table_));
  }

  void CreateLeaderAndReplicaProxies(const TabletLocationsPB& locations) {
    leader_.reset();
    STLDeleteElements(&replicas_);
    BOOST_FOREACH(const TabletLocationsPB::ReplicaPB& replica_pb, locations.replicas()) {
      HostPort host_port;
      ASSERT_STATUS_OK(HostPortFromPB(replica_pb.ts_info().rpc_addresses(0), &host_port));
      vector<Sockaddr> addresses;
      host_port.ResolveAddresses(&addresses);
      gscoped_ptr<TabletServerServiceProxy> proxy;
      CreateClientProxy(addresses[0], &proxy);
      if (replica_pb.role() == QuorumPeerPB::LEADER) {
        ProxyDetails* leader = new ProxyDetails();
        leader->proxy.reset(proxy.release());
        leader->ts_info.CopyFrom(replica_pb.ts_info());
        leader_.reset(leader);
      } else if (replica_pb.role() == QuorumPeerPB::FOLLOWER) {
        ProxyDetails* replica = new ProxyDetails();
        replica->proxy.reset(proxy.release());
        replica->ts_info.CopyFrom(replica_pb.ts_info());
        replicas_.push_back(replica);
      }
    }
  }

  // Gets the the locations of the quorum and waits until 1 LEADER and kNumReplicas - 1
  // FOLLOWERS are reported.
  void WaitForAndGetQuorum() {
    GetTableLocationsRequestPB req;
    TableIdentifierPB* id = req.mutable_table();
    id->set_table_name(kTableId);

    GetTableLocationsResponsePB resp;
    RpcController controller;

    CHECK_OK(client_->master_proxy()->GetTableLocations(req, &resp, &controller));
    ASSERT_EQ(resp.tablet_locations_size(), 1);
    tablet_id = resp.tablet_locations(0).tablet_id();

    TabletLocationsPB locations;
    int num_retries = 0;
    // make sure the three replicas are up and find the leader
    while (true) {
      if (num_retries >= kMaxRetries) {
        FAIL() << " Reached max. retries while looking up the quorum.";
      }
      // TODO add a way to wait for a tablet to be ready. Also to wait for it to
      // have a certain _active_ replication count.
      replicas_.clear();
      Status status = cluster_->WaitForReplicaCount(resp.tablet_locations(0).tablet_id(),
                                                    kNumReplicas, &locations);
      if (status.IsTimedOut()) {
        LOG(WARNING)<< "Timeout waiting for all three replicas to be online, retrying...";
        num_retries++;
        continue;
      }

      ASSERT_STATUS_OK(status);
      CreateLeaderAndReplicaProxies(locations);

      if (leader_.get() == NULL || replicas_.size() < kNumReplicas - 1) {
        LOG(WARNING)<< "Couldn't find the leader and/or replicas. Locations: "
        << locations.ShortDebugString();
        sleep(1);
        num_retries++;
        continue;
      }
      break;
    }
    CreateSharedRegion();
  }

  void ScanReplica(TabletServerServiceProxy* replica_proxy,
                   vector<string>* results) {

    ScanRequestPB req;
    ScanResponsePB resp;
    RpcController rpc;

    NewScanRequestPB* scan = req.mutable_new_scan_request();
    scan->set_tablet_id(tablet_id);
    ASSERT_STATUS_OK(SchemaToColumnPBs(schema_, scan->mutable_projected_columns()));

    // Send the call
    {
      req.set_batch_size_bytes(0);
      SCOPED_TRACE(req.DebugString());
      ASSERT_STATUS_OK(replica_proxy->Scan(req, &resp, &rpc));
      SCOPED_TRACE(resp.DebugString());
      ASSERT_FALSE(resp.has_error());
    }

    if (!resp.has_more_results())
      return;

    // Drain all the rows from the scanner.
    ASSERT_NO_FATAL_FAILURE(DrainScannerToStrings(resp.scanner_id(),
                                                  schema_,
                                                  results,
                                                  replica_proxy));

    std::sort(results->begin(), results->end());
  }

  void AssertRowsExistInReplicas() {
    vector<string> leader_results;
    vector<string> replica_results;
    ScanReplica(leader_->proxy.get(), &leader_results);
    BOOST_FOREACH(ProxyDetails* replica, replicas_) {
      SCOPED_TRACE(strings::Substitute("Replica results did not match the leaders."
          "\nReplica: $0\nLeader:$1", replica->ts_info.ShortDebugString(),
          leader_->ts_info.ShortDebugString()));
      ScanReplica(replica->proxy.get(), &replica_results);
      ASSERT_EQ(leader_results.size(), replica_results.size());
      for (int i = 0; i < leader_results.size(); i++) {
        ASSERT_EQ(leader_results[i], replica_results[i]);
      }
      replica_results.clear();
    }
  }

  void InsertTestRowsRemoteThread(int tid,
                                  uint64_t first_row,
                                  uint64_t count,
                                  uint64_t num_batches,
                                  TabletServerServiceProxy* proxy) {
    TabletServerTest::InsertTestRowsRemote(tid, first_row, count, num_batches, proxy, tablet_id);
    inserters_.CountDown();
  }

  // Brings Chaos to a MiniTabletServer by introducing random delays. Does this by stealing the
  // consensus lock a portion of the time.
  // TODO use the consensus/tablet/log hooks _as_well_as_ lock stealing
  // TODO This generates sleeps < 1.163175 seconds, with 99% probability. That is, this is tailored
  // to go over the default consensus timeout (1 sec) with < 5% probability. Change this to be
  // able to handle any timeout.
  void DelayInjectorThread(MiniTabletServer* mini_tablet_server) {
    shared_ptr<TabletPeer> peer;
    CHECK(mini_tablet_server->server()->tablet_manager()->LookupTabletUnlocked(tablet_id, &peer));
    RaftConsensus* consensus = down_cast<RaftConsensus*>(peer->consensus());
    ReplicaState* state = consensus->GetReplicaStateForTests();
    while (inserters_.count() > 0) {

      double sleep_time = NormalDist(0, 0.5);
      if (sleep_time < 0) sleep_time = 0;

      double val = (rand() * 1.0) / RAND_MAX;

      if (val < 0.8) {
        usleep(sleep_time * 1000 * 1000);
        continue;
      }

      ReplicaState::UniqueLock lock;
      CHECK_OK(state->LockForRead(&lock));
      usleep(sleep_time * 1000 * 1000);
    }
  }


  virtual void TearDown() {
    cluster_->Shutdown();
    STLDeleteElements(&replicas_);
  }

 protected:
  gscoped_ptr<MiniCluster> cluster_;
  shared_ptr<KuduClient> client_;
  scoped_refptr<KuduTable> table_;
  gscoped_ptr<ProxyDetails> leader_;
  vector<ProxyDetails*> replicas_;

  QuorumPB quorum_;
  Schema schema_;
  string tablet_id;

  std::vector<scoped_refptr<kudu::Thread> > threads_;
  CountDownLatch inserters_;
};

// TODO allow the scan to define an operation id, fetch the last id
// from the leader and then use that id to make the replica wait
// until it is done. This will avoid the sleeps below.
TEST_F(DistConsensusTest, TestInsertAndMutateThroughConsensus) {

  if (AllowSlowTests()) {
    for (int i = 0; i < 100; i++) {
      InsertTestRowsRemoteThread(0, i * 1000, 1000, 100, leader_->proxy.get());
    }
    // sleep to let the request get committed to the replicas.
    usleep(500000);
  } else {
    InsertTestRowsRemoteThread(0, 0, 1000, 100, leader_->proxy.get());
    usleep(1000000);
  }
  AssertRowsExistInReplicas();
}

TEST_F(DistConsensusTest, TestFailedTransaction) {
  WriteRequestPB req;
  req.set_tablet_id(tablet_id);
  RowOperationsPB* data = req.mutable_row_operations();
  data->set_rows("some gibberish!");

  WriteResponsePB resp;
  RpcController controller;

  controller.set_timeout(MonoDelta::FromSeconds(FLAGS_rpc_timeout));
  ASSERT_STATUS_OK(DCHECK_NOTNULL(leader_->proxy.get())->Write(req, &resp, &controller));

  ASSERT_TRUE(resp.has_error());
}

// Inserts rows through consensus and also starts one delay injecting thread
// that steals consensus peer locks for a while. This is meant to test that
// even with timeouts and repeated requests consensus still works.
TEST_F(DistConsensusTest, MultiThreadedMutateAndInsertThroughConsensus) {
  if (1000 == FLAGS_client_inserts_per_thread) {
    if (this->AllowSlowTests()) {
      FLAGS_client_inserts_per_thread = 50000;
      FLAGS_client_num_batches_per_thread = 5000;
    }
  }

  int num_threads = FLAGS_num_client_threads;
  for (int i = 0; i < num_threads; i++) {
    scoped_refptr<kudu::Thread> new_thread;
    CHECK_OK(kudu::Thread::Create("test", strings::Substitute("ts-test$0", i),
                                  &DistConsensusTest::InsertTestRowsRemoteThread,
                                  this, i, i * FLAGS_client_inserts_per_thread,
                                  FLAGS_client_inserts_per_thread,
                                  FLAGS_client_num_batches_per_thread,
                                  leader_->proxy.get(),
                                  &new_thread));
    threads_.push_back(new_thread);
  }
  for (int i = 0; i < kNumReplicas; i++) {
    scoped_refptr<kudu::Thread> new_thread;
    CHECK_OK(kudu::Thread::Create("test", strings::Substitute("chaos-test$0", i),
                                  &DistConsensusTest::DelayInjectorThread,
                                  this, cluster_->mini_tablet_server(i),
                                  &new_thread));
    threads_.push_back(new_thread);
  }
  BOOST_FOREACH(scoped_refptr<kudu::Thread> thr, threads_) {
   CHECK_OK(ThreadJoiner(thr.get()).Join());
  }

  if (AllowSlowTests()) {
    usleep(5000000);
  } else {
    usleep(2000000);
  }
  AssertRowsExistInReplicas();
}

TEST_F(DistConsensusTest, TestInsertOnNonLeader) {
  // Manually construct a write RPC to a replica and make sure it responds
  // with the correct error code.
  WriteRequestPB req;
  WriteResponsePB resp;
  RpcController rpc;
  req.set_tablet_id(tablet_id);
  ASSERT_STATUS_OK(SchemaToPB(schema_, req.mutable_schema()));

  AddTestRowToPB(RowOperationsPB::INSERT, schema_, 1234, 5678,
                 "hello world via RPC", req.mutable_row_operations());
  ASSERT_STATUS_OK(replicas_[0]->proxy->Write(req, &resp, &rpc));
  SCOPED_TRACE(resp.DebugString());
  ASSERT_TRUE(resp.has_error());
  Status s = StatusFromPB(resp.error().status());
  EXPECT_TRUE(s.IsIllegalState());
  ASSERT_STR_CONTAINS(s.ToString(), "Replica is not leader of this quorum");
  // TODO: need to change the error code to be something like REPLICA_NOT_LEADER
  // so that the client can properly handle this case! plumbing this is a little difficult
  // so not addressing at the moment.
}

}  // namespace tserver
}  // namespace kudu

