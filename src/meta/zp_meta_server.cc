#include "src/meta/zp_meta_server.h"

#include <cstdlib>
#include <ctime>
#include <string>
#include <sys/resource.h>
#include <glog/logging.h>
#include <google/protobuf/text_format.h>
#include <google/protobuf/repeated_field.h>

#include "pink/include/pink_cli.h"
#include "slash/include/env.h"

#include "include/zp_meta.pb.h"

std::string NodeOffsetKey(const std::string& table, int partition_id,
    const std::string& ip, int port) {
  char buf[256];
  snprintf(buf, sizeof(buf), "%s_%u_%s:%u",
      table.c_str(), partition_id, ip.c_str(), port);
  return std::string(buf);
}

ZPMetaServer::ZPMetaServer()
  : should_exit_(false) {
  LOG(INFO) << "ZPMetaServer start initialization";
  
  // Init Command
  cmds_.reserve(300);
  InitClientCmdTable();

  // Open Floyd
  Status s = OpenFloyd();
  if (!s.ok()) {
    LOG(FATAL) << "Failed to open floyd, error: " << s.ToString();
  }

  // Open InfoStore
  info_store_ = new ZPMetaInfoStore(floyd_);

  // Create Migrate Register
  s = ZPMetaMigrateRegister::Create(floyd_, &migrate_register_);
  if (!s.ok()) {
    LOG(FATAL) << "Failed to create migrate register, error: " << s.ToString();
  }

  // Init update thread
  update_thread_ = new ZPMetaUpdateThread(info_store_);

  // Init Condition thread
  condition_cron_ = new ZPMetaConditionCron(&node_offsets_, update_thread_);
  
  // Init Server thread
  conn_factory_ = new ZPMetaClientConnFactory(); 
  server_thread_ = pink::NewDispatchThread(
      g_zp_conf->local_port() + kMetaPortShiftCmd, 
      g_zp_conf->meta_thread_num(),
      conn_factory_,
      kMetaDispathCronInterval,
      kMetaDispathQueueSize,
      nullptr);
  server_thread_->set_thread_name("ZPMetaDispatch");
}

ZPMetaServer::~ZPMetaServer() {
  server_thread_->StopThread();
  delete server_thread_;
  delete conn_factory_;

  delete condition_cron_;
  delete update_thread_;
  delete migrate_register_;
  delete info_store_;
  delete floyd_;

  leader_joint_.CleanLeader();
  DestoryCmdTable(cmds_);
  LOG(INFO) << "ZPMetaServer Delete Done";
}

Status ZPMetaServer::OpenFloyd() {
  int port = 0;
  std::string ip;
  floyd::Options fy_options;
  fy_options.members = g_zp_conf->meta_addr();
  for (std::string& it : fy_options.members) {
    std::replace(it.begin(), it.end(), '/', ':');
    if (!slash::ParseIpPortString(it, ip, port)) {
      LOG(WARNING) << "Error meta addr: " << it;
      return Status::Corruption("Error meta addr");
    }
  }
  fy_options.local_ip = g_zp_conf->local_ip();
  fy_options.local_port = g_zp_conf->local_port() + kMetaPortShiftFY;
  fy_options.path = g_zp_conf->data_path();
  return floyd::Floyd::Open(fy_options, &floyd_);
}

void ZPMetaServer::Start() {
  LOG(INFO) << "ZPMetaServer started on port:" << g_zp_conf->local_port();
  
  Status s = Status::Incomplete("Info store load incompleted");
  while (!should_exit_ && !s.ok()) {
    sleep(1);
    s = info_store_->Refresh();
    LOG(INFO) << "Info store load from floyd, ret: " << s.ToString();
  }
  s = RefreshLeader();
  if (!s.ok()) {
    LOG(WARNING) << "Refresh Leader failed: " << s.ToString();
    return;
  }

  if (should_exit_) {
    return;
  }

  if (pink::RetCode::kSuccess != server_thread_->StartThread()) {
    LOG(WARNING) << "Disptch thread start failed";
    return;
  }

  while (!should_exit_) {
    DoTimingTask();
    int sleep_count = kMetaCronWaitCount;
    while (!should_exit_ && sleep_count-- > 0) {
      usleep(kMetaCronInterval * 1000);
    }
  }
  return;
}

Cmd* ZPMetaServer::GetCmd(const int op) {
  return GetCmdFromTable(op, cmds_);
}

Status ZPMetaServer::GetMetaInfoByTable(const std::string& table,
    ZPMeta::MetaCmdResponse_Pull *ms_info) {
  ms_info->set_version(info_store_->epoch());
  ZPMeta::Table* table_info = ms_info->add_info();
  Status s = info_store_->GetTableMeta(table, table_info);
  if (!s.ok()) {
    LOG(WARNING) << "Get table meta for node failed: " << s.ToString()
      << ", table: " << table;
    return s;
  }
  return Status::OK();
}

Status ZPMetaServer::GetMetaInfoByNode(const std::string& ip_port,
    ZPMeta::MetaCmdResponse_Pull *ms_info) {
  // Get epoch first and because the epoch was updated at last
  // no lock is needed here
  ms_info->set_version(info_store_->epoch());

  std::set<std::string> tables;
  Status s = info_store_->GetTablesForNode(ip_port, &tables);
  if (!s.ok() && !s.IsNotFound()) {
    LOG(WARNING) << "Get all tables for Node failed: " << s.ToString()
      << ", node: " << ip_port;
    return s;
  }

  for (const auto& t : tables) {
    ZPMeta::Table* table_info = ms_info->add_info();
    s = info_store_->GetTableMeta(t, table_info);
    if (!s.ok()) {
      LOG(WARNING) << "Get one table meta for node failed: " << s.ToString()
        << ", node: " << ip_port << ", table: " << t;
      return s;
    }
  }
  return Status::OK();
}

Status ZPMetaServer::WaitSetMaster(const ZPMeta::Node& node,
    const std::string table, int partition) {
  ZPMeta::Node master;
  Status s = info_store_->GetPartitionMaster(table, partition, &master);
  if (!s.ok()) {
    LOG(WARNING) << "Partition not exist: " << table << "_" << partition;
    return s;
  }

  // Stuck partition
  update_thread_->PendingUpdate(
      UpdateTask(
        ZPMetaUpdateOP::kOpSetStuck,
        "",
        table,
        partition));

  // SetMaster when current node catched up with the master
  condition_cron_->AddCronTask(
      OffsetCondition(
        table,
        partition,
        master,
        node),
      UpdateTask(
        ZPMetaUpdateOP::kOpSetMaster,
        slash::IpPortString(node.ip(), node.port()),
        table,
        partition));

  return Status::OK();
}

void ZPMetaServer::UpdateNodeAlive(const std::string& ip_port) {
  if (info_store_->UpdateNodeAlive(ip_port)) {
    // new node
    LOG(INFO) << "PendingUpdate to add Node Alive " << ip_port;
    update_thread_->PendingUpdate(
        UpdateTask(
          ZPMetaUpdateOP::kOpUpNode,
          ip_port));
  }
}

void ZPMetaServer::CheckNodeAlive() {
  std::set<std::string> nodes;
  info_store_->FetchExpiredNode(&nodes);
  for (const auto& n : nodes) {
    LOG(INFO) << "PendingUpdate to remove Node Alive: " << n;
    update_thread_->PendingUpdate(
        UpdateTask(
          ZPMetaUpdateOP::kOpDownNode,
          n));
  }
}

Status ZPMetaServer::GetAllMetaNodes(ZPMeta::MetaCmdResponse_ListMeta *nodes) {
  std::string value;
  ZPMeta::Nodes allnodes;
  std::vector<std::string> meta_nodes;
  floyd_->GetAllNodes(meta_nodes);

  ZPMeta::MetaNodes *p = nodes->mutable_nodes();
  std::string leader_ip;
  int leader_port = 0;
  bool ret = GetLeader(&leader_ip, &leader_port);
  if (ret) {
    ZPMeta::Node leader;
    ZPMeta::Node *np = p->mutable_leader();
    np->set_ip(leader_ip);
    np->set_port(leader_port);
  }

  std::string ip;
  int port = 0;
  for (const auto& iter : meta_nodes) {
    if (!slash::ParseIpPortString(iter, ip, port)) {
      return Status::Corruption("parse ip port error");
    }
    if (ret
        && ip == leader_ip
        && port - kMetaPortShiftFY == leader_port) {
      continue;
    }
    ZPMeta::Node *np = p->add_followers();
    np->set_ip(ip);
    np->set_port(port - kMetaPortShiftFY);
  }
  return Status::OK();
}

Status ZPMetaServer::GetMetaStatus(std::string *result) {
  floyd_->GetServerStatus(*result);
  return Status::OK();
}

Status ZPMetaServer::GetTableList(std::set<std::string>* table_list) {
  return info_store_->GetTableList(table_list);
}

Status ZPMetaServer::GetNodeStatusList(
    std::unordered_map<std::string, ZPMeta::NodeState>* node_list) {
  info_store_->GetAllNodes(node_list);
  return Status::OK();
}

Status ZPMetaServer::Migrate(int epoch, const std::vector<ZPMeta::RelationCmdUnit>& diffs) {
  if (epoch != info_store_->epoch()) {
    return Status::InvalidArgument("Expired epoch");
  }
  
  // Register
  assert(!diffs.empty());
  Status s = migrate_register_->Init(diffs);
  if (!s.ok()) {
    LOG(WARNING) << "Migrate register Init failed, error: " << s.ToString();
    return s;
  }

  // retry some time to ProcessMigrate
  int retry = kInitMigrateRetryNum;
  do {
    s = ProcessMigrate();
  
  } while (s.IsIncomplete() && retry-- > 0);
  return s;
}

Status ZPMetaServer::ProcessMigrate() {
  // Get next 
  std::vector<ZPMeta::RelationCmdUnit> diffs;
  Status s = migrate_register_->GetN(kMetaMigrateOnceCount, &diffs);
  if (s.IsNotFound()) {
    LOG(INFO) << "No migrate to be processed";
  } else if (!s.ok()) {
    LOG(WARNING) << "Get next N migrate diffs failed, error: " << s.ToString();
    return s;
  }

  bool has_process = false;
  for (const auto& diff : diffs) {
    // Add Slave
    update_thread_->PendingUpdate(
        UpdateTask(
          ZPMetaUpdateOP::kOpAddSlave,
          slash::IpPortString(diff.right().ip(), diff.right().port()),
          diff.table(),
          diff.partition()));

    // Stuck partition
    update_thread_->PendingUpdate(
        UpdateTask(
          ZPMetaUpdateOP::kOpSetStuck,
          "",
          diff.table(),
          diff.partition()));

    // Begin offset condition wait
    condition_cron_->AddCronTask(
        OffsetCondition(
          diff.table(),
          diff.partition(),
          diff.left(),
          diff.right()),
        UpdateTask(
          ZPMetaUpdateOP::kOpRemoveSlave,
          slash::IpPortString(diff.left().ip(), diff.left().port()),
          diff.table(),
          diff.partition()));

    has_process = true;
  }

  if (!has_process) {
    LOG(WARNING) << "No migrate item be success begin";
    return Status::Incomplete("no migrate item begin");
  }
  return Status::OK();
}

bool ZPMetaServer::IsLeader() {
  slash::MutexLock l(&(leader_joint_.mutex));
  if (leader_joint_.ip == g_zp_conf->local_ip() && 
      leader_joint_.port == g_zp_conf->local_port() + kMetaPortShiftCmd) {
    return true;
  }
  return false;
}

Status ZPMetaServer::RedirectToLeader(ZPMeta::MetaCmd &request, ZPMeta::MetaCmdResponse *response) {
  // Do not try to connect leader even failed,
  // and leave this to RefreshLeader process in cron
  slash::MutexLock l(&(leader_joint_.mutex));
  if (leader_joint_.cli == NULL) {
    LOG(ERROR) << "Failed to RedirectToLeader, cli is NULL";
    return Status::Corruption("no leader connection");
  }
  Status s = leader_joint_.cli->Send(&request);
  if (!s.ok()) {
    LOG(ERROR) << "Failed to send redirect message to leader, error: "
      << s.ToString() << ", leader: " << leader_joint_.ip
      << ":" << leader_joint_.port;  
    return s;
  }
  s = leader_joint_.cli->Recv(response); 
  if (!s.ok()) {
    LOG(ERROR) << "Failed to recv redirect message from leader, error: "
      << s.ToString() << ", leader: " << leader_joint_.ip
      << ":" << leader_joint_.port;  
  }
  return s;
}

Status ZPMetaServer::RefreshLeader() {
  std::string leader_ip;
  int leader_port = 0, leader_cmd_port = 0;
  if (!GetLeader(&leader_ip, &leader_port)) {
    LOG(WARNING) << "No leader yet";
    return Status::Incomplete("No leader yet");
  }

  // No change
  leader_cmd_port = leader_port + kMetaPortShiftCmd;
  slash::MutexLock l(&(leader_joint_.mutex));
  if (leader_ip == leader_joint_.ip
      && leader_cmd_port == leader_joint_.port) {
    return Status::OK();
  }
  
  // Leader changed
  LOG(WARNING) << "Leader changed from: "
    << leader_joint_.ip << ":" << leader_joint_.port
    << ", To: " <<  leader_ip << ":" << leader_cmd_port;
  leader_joint_.CleanLeader();

  // I'm new leader
  Status s;
  if (leader_ip == g_zp_conf->local_ip() && 
      leader_port == g_zp_conf->local_port()) {
    LOG(INFO) << "Become leader: " << leader_ip << ":" << leader_port;
    s = info_store_->RestoreNodeAlive();
    if (!s.ok()) {
      LOG(ERROR) << "Restore Node alive failed: " << s.ToString();
      return s;
    }
    return Status::OK();
  }
  
  // Connect to new leader
  leader_joint_.cli = pink::NewPbCli();
  leader_joint_.ip = leader_ip;
  leader_joint_.port = leader_cmd_port;
  s = leader_joint_.cli->Connect(leader_joint_.ip,
      leader_joint_.port);
  if (!s.ok()) {
    leader_joint_.CleanLeader();
    LOG(ERROR) << "Connect to leader: " << leader_ip << ":" << leader_cmd_port
      << " failed: " << s.ToString();
  } else {
    LOG(INFO) << "Connect to leader: " << leader_ip << ":" << leader_cmd_port
      << " success.";
    leader_joint_.cli->set_send_timeout(1000);
    leader_joint_.cli->set_recv_timeout(1000);
  }
  return s;
}

void ZPMetaServer::InitClientCmdTable() {

  // Ping Command
  Cmd* pingptr = new PingCmd(kCmdFlagsRead | kCmdFlagsRedirect);
  cmds_.insert(std::pair<int, Cmd*>(static_cast<int>(ZPMeta::Type::PING), pingptr));

  //Pull Command
  Cmd* pullptr = new PullCmd(kCmdFlagsRead);
  cmds_.insert(std::pair<int, Cmd*>(static_cast<int>(ZPMeta::Type::PULL), pullptr));

  //Init Command
  Cmd* initptr = new InitCmd(kCmdFlagsWrite | kCmdFlagsRedirect);
  cmds_.insert(std::pair<int, Cmd*>(static_cast<int>(ZPMeta::Type::INIT), initptr));

  //SetMaster Command
  Cmd* setmasterptr = new SetMasterCmd(kCmdFlagsWrite | kCmdFlagsRedirect);
  cmds_.insert(std::pair<int, Cmd*>(static_cast<int>(ZPMeta::Type::SETMASTER), setmasterptr));

  //AddSlave Command
  Cmd* addslaveptr = new AddSlaveCmd(kCmdFlagsWrite | kCmdFlagsRedirect);
  cmds_.insert(std::pair<int, Cmd*>(static_cast<int>(ZPMeta::Type::ADDSLAVE), addslaveptr));

  //RemoveSlave Command
  Cmd* removeslaveptr = new RemoveSlaveCmd(kCmdFlagsWrite | kCmdFlagsRedirect);
  cmds_.insert(std::pair<int, Cmd*>(static_cast<int>(ZPMeta::Type::REMOVESLAVE), removeslaveptr));

  //ListTable Command
  Cmd* listtableptr = new ListTableCmd(kCmdFlagsRead);
  cmds_.insert(std::pair<int, Cmd*>(static_cast<int>(ZPMeta::Type::LISTTABLE), listtableptr));

  //ListNode Command
  Cmd* listnodeptr = new ListNodeCmd(kCmdFlagsRead);
  cmds_.insert(std::pair<int, Cmd*>(static_cast<int>(ZPMeta::Type::LISTNODE), listnodeptr));

  //ListMeta Command
  Cmd* listmetaptr = new ListMetaCmd(kCmdFlagsRead);
  cmds_.insert(std::pair<int, Cmd*>(static_cast<int>(ZPMeta::Type::LISTMETA), listmetaptr));

  //MetaStatus Command
  Cmd* meta_status_ptr = new MetaStatusCmd(kCmdFlagsRead);
  cmds_.insert(std::pair<int, Cmd*>(static_cast<int>(ZPMeta::Type::METASTATUS), meta_status_ptr));

  //DropTable Command
  Cmd* droptableptr = new DropTableCmd(kCmdFlagsWrite | kCmdFlagsRedirect);
  cmds_.insert(std::pair<int, Cmd*>(static_cast<int>(ZPMeta::Type::DROPTABLE), droptableptr));

  // Migrate Command
  Cmd* migrateptr = new MigrateCmd(kCmdFlagsWrite | kCmdFlagsRedirect);
  cmds_.insert(std::pair<int, Cmd*>(static_cast<int>(ZPMeta::Type::MIGRATE),
        migrateptr));

  // Cancel Migrate Command
  Cmd* cancel_migrate_ptr = new CancelMigrateCmd(kCmdFlagsWrite | kCmdFlagsRedirect);
  cmds_.insert(std::pair<int, Cmd*>(static_cast<int>(ZPMeta::Type::CANCELMIGRATE),
        cancel_migrate_ptr));
  
  //// Check Migrate Command
  //Cmd* check_migrate_ptr = new CheckMigrateCmd(kCmdFlagsWrite);
  //cmds_.insert(std::pair<int, Cmd*>(static_cast<int>(ZPMeta::Type::CHECKMIGRATE),
  //      check_migrate_ptr));
}

inline bool ZPMetaServer::GetLeader(std::string *ip, int *port) {
  int fy_port = 0;
  bool res = floyd_->GetLeader(ip, &fy_port);
  if (res) {
    *port = fy_port - kMetaPortShiftFY;
  }
  return res;
}


void ZPMetaServer::UpdateNodeOffset(const ZPMeta::MetaCmd_Ping &ping) {
  slash::MutexLock l(&(node_offsets_.mutex));
  for (const auto& po : ping.offset()) {
    std::string offset_key = NodeOffsetKey(po.table_name(), po.partition(),
        ping.node().ip(), ping.node().port());
    node_offsets_.offsets[offset_key] = NodeOffset(po.filenum(), po.offset());
  }
}

bool ZPMetaServer::GetSlaveOffset(const std::string &table, int partition,
    const std::string ip, int port, NodeOffset* node_offset) {
  slash::MutexLock l(&(node_offsets_.mutex));
  auto iter = node_offsets_.offsets.find(NodeOffsetKey(table,
        partition, ip, port));
  if (iter == node_offsets_.offsets.end()) {
    return false;
  }
  *node_offset = iter->second;
  return true;
}

void ZPMetaServer::DebugOffset() {
  slash::MutexLock l(&(node_offsets_.mutex));
  for (const auto& item : node_offsets_.offsets) {
    LOG(INFO) << item.first << "->"
      << item.second.filenum << "_" << item.second.offset; 
  }
}

void ZPMetaServer::ResetLastSecQueryNum() {
  uint64_t cur_time_us = slash::NowMicros();
  statistic.last_qps = (statistic.query_num - statistic.last_query_num)
    * 1000000
    / (cur_time_us - statistic.last_time_us + 1);
  statistic.last_query_num = statistic.query_num.load();
  statistic.last_time_us = cur_time_us;
}

void ZPMetaServer::DoTimingTask() {
  // Refresh Leader joint
  Status s = RefreshLeader();
  if (!s.ok()) {
    LOG(WARNING) << "Refresh Leader failed: " << s.ToString();
  }

  // Refresh info_store
  if (!IsLeader()) {
    Status s = info_store_->Refresh();
    if (!s.ok()) {
      LOG(WARNING) << "Refresh info_store_ failed: " << s.ToString();
    }
  }

  // Update statistic info
  ResetLastSecQueryNum();
  LOG(INFO) << "ServerQueryNum: " << statistic.query_num
      << " ServerCurrentQps: " << statistic.last_qps;

  // Check alive
  CheckNodeAlive();
}

