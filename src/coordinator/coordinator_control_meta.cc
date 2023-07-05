// Copyright (c) 2023 dingodb.com, Inc. All Rights Reserved
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "butil/containers/flat_map.h"
#include "butil/status.h"
#include "common/logging.h"
#include "coordinator/auto_increment_control.h"
#include "coordinator/coordinator_control.h"
#include "proto/common.pb.h"
#include "proto/coordinator_internal.pb.h"
#include "proto/error.pb.h"
#include "proto/meta.pb.h"

namespace dingodb {

// GenerateRootSchemas
// root schema
// meta schema
// dingo schema
// mysql schema
// information schema
void CoordinatorControl::GenerateRootSchemas(pb::coordinator_internal::SchemaInternal& root_schema_internal,
                                             pb::coordinator_internal::SchemaInternal& meta_schema_internal,
                                             pb::coordinator_internal::SchemaInternal& dingo_schema_internal,
                                             pb::coordinator_internal::SchemaInternal& mysql_schema_internal,
                                             pb::coordinator_internal::SchemaInternal& information_schema_internal) {
  // root schema
  // pb::meta::Schema root_schema;
  root_schema_internal.set_id(::dingodb::pb::meta::ReservedSchemaIds::ROOT_SCHEMA);
  root_schema_internal.set_name("root");

  // meta schema
  // pb::meta::Schema meta_schema;
  meta_schema_internal.set_id(::dingodb::pb::meta::ReservedSchemaIds::META_SCHEMA);
  meta_schema_internal.set_name("meta");

  // dingo schema
  // pb::meta::Schema dingo_schema;
  dingo_schema_internal.set_id(::dingodb::pb::meta::ReservedSchemaIds::DINGO_SCHEMA);
  dingo_schema_internal.set_name("dingo");

  // mysql schema
  // pb::mysql::Schema mysql_schema;
  mysql_schema_internal.set_id(::dingodb::pb::meta::ReservedSchemaIds::MYSQL_SCHEMA);
  mysql_schema_internal.set_name("mysql");

  // information schema
  // pb::information::Schema information_schema;
  information_schema_internal.set_id(::dingodb::pb::meta::ReservedSchemaIds::INFORMATION_SCHEMA);
  information_schema_internal.set_name("information_schema");

  DINGO_LOG(INFO) << "GenerateRootSchemas 0[" << root_schema_internal.DebugString();
  DINGO_LOG(INFO) << "GenerateRootSchemas 1[" << meta_schema_internal.DebugString();
  DINGO_LOG(INFO) << "GenerateRootSchemas 2[" << dingo_schema_internal.DebugString();
  DINGO_LOG(INFO) << "GenerateRootSchemas 3[" << mysql_schema_internal.DebugString();
  DINGO_LOG(INFO) << "GenerateRootSchemas 4[" << information_schema_internal.DebugString();
}

// ValidateSchema
// check if schema_id is exist
// if exist return true
// else return false
bool CoordinatorControl::ValidateSchema(uint64_t schema_id) {
  // BAIDU_SCOPED_LOCK(schema_map_mutex_);
  bool ret = schema_map_.Exists(schema_id);
  if (!ret) {
    DINGO_LOG(ERROR) << " ValidateSchema schema_id is illegal " << schema_id;
    return false;
  }

  return true;
}

// CreateSchema
// create new schema
// only root schema can have sub schema
// schema_name must be unique
// in: parent_schema_id
// in: schema_name
// out: new_schema_id
// out: meta_increment
// return OK if success
// return other if failed
butil::Status CoordinatorControl::CreateSchema(uint64_t parent_schema_id, std::string schema_name,
                                               uint64_t& new_schema_id,
                                               pb::coordinator_internal::MetaIncrement& meta_increment) {
  // validate if parent_schema_id is root schema
  // only root schema can have sub schema
  if (parent_schema_id != ::dingodb::pb::meta::ReservedSchemaIds::ROOT_SCHEMA) {
    DINGO_LOG(ERROR) << " CreateSchema parent_schema_id is not root schema " << parent_schema_id;
    return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS, "parent_schema_id is not root schema");
  }

  if (schema_name.empty()) {
    DINGO_LOG(INFO) << " CreateSchema schema_name is illegal " << schema_name;
    return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS, "schema_name is empty");
  }

  // check if schema_name exists
  uint64_t value = 0;
  schema_name_map_safe_temp_.Get(schema_name, value);
  if (value != 0) {
    DINGO_LOG(INFO) << " CreateSchema schema_name is exist " << schema_name;
    return butil::Status(pb::error::Errno::ESCHEMA_EXISTS, "schema_name is exist");
  }

  // create new schema id
  new_schema_id = GetNextId(pb::coordinator_internal::IdEpochType::ID_NEXT_SCHEMA, meta_increment);

  // update schema_name_map_safe_temp_
  if (schema_name_map_safe_temp_.PutIfAbsent(schema_name, new_schema_id) < 0) {
    DINGO_LOG(INFO) << " CreateSchema schema_name" << schema_name
                    << " is exist, when insert new_schema_id=" << new_schema_id;
    return butil::Status(pb::error::Errno::ESCHEMA_EXISTS, "schema_name is exist");
  }

  // add new schema to  schema_map_
  pb::coordinator_internal::SchemaInternal new_schema_internal;
  new_schema_internal.set_id(new_schema_id);
  new_schema_internal.set_name(schema_name);

  // update meta_increment
  auto* schema_increment = meta_increment.add_schemas();
  schema_increment->set_id(new_schema_id);
  schema_increment->set_op_type(::dingodb::pb::coordinator_internal::MetaIncrementOpType::CREATE);
  schema_increment->set_schema_id(parent_schema_id);

  auto* schema_increment_schema = schema_increment->mutable_schema_internal();
  schema_increment_schema->CopyFrom(new_schema_internal);

  // bump up schema map epoch
  GetNextId(pb::coordinator_internal::IdEpochType::EPOCH_SCHEMA, meta_increment);

  // on_apply
  //  schema_map_.insert(std::make_pair(new_schema_id, new_schema));  // raft_kv_put

  return butil::Status::OK();
}

// DropSchema
// drop schema
// in: parent_schema_id
// in: schema_id
// return: 0 or -1
butil::Status CoordinatorControl::DropSchema(uint64_t parent_schema_id, uint64_t schema_id,
                                             pb::coordinator_internal::MetaIncrement& meta_increment) {
  if (schema_id <= COORDINATOR_ID_OF_MAP_MIN) {
    DINGO_LOG(ERROR) << "ERRROR: schema_id illegal " << schema_id;
    return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS, "schema_id is illegal");
  }

  if (parent_schema_id < 0) {
    DINGO_LOG(ERROR) << "ERRROR: parent_schema_id illegal " << schema_id;
    return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS, "parent_schema_id is illegal");
  }

  pb::coordinator_internal::SchemaInternal schema_internal_to_delete;
  {
    // BAIDU_SCOPED_LOCK(schema_map_mutex_);
    // auto* schema_to_delete = schema_map_.seek(schema_id);
    int ret = schema_map_.Get(schema_id, schema_internal_to_delete);
    if (ret < 0) {
      DINGO_LOG(ERROR) << "ERRROR: schema_id not found" << schema_id;
      return butil::Status(pb::error::Errno::ESCHEMA_NOT_FOUND, "schema_id not found");
    }

    if (schema_internal_to_delete.table_ids_size() > 0) {
      DINGO_LOG(ERROR) << "ERRROR: schema is not empty" << schema_id
                       << " table_ids_size=" << schema_internal_to_delete.table_ids_size();
      return butil::Status(pb::error::Errno::ESCHEMA_NOT_EMPTY, "schema is not empty");
    }
  }

  // bump up epoch
  GetNextId(pb::coordinator_internal::IdEpochType::EPOCH_SCHEMA, meta_increment);

  // delete schema
  auto* schema_to_delete = meta_increment.add_schemas();
  schema_to_delete->set_id(schema_id);
  schema_to_delete->set_op_type(::dingodb::pb::coordinator_internal::MetaIncrementOpType::DELETE);
  schema_to_delete->set_schema_id(parent_schema_id);

  auto* schema_to_delete_schema = schema_to_delete->mutable_schema_internal();
  schema_to_delete_schema->CopyFrom(schema_internal_to_delete);

  // delete schema_name from schema_name_map_safe_temp_
  schema_name_map_safe_temp_.Erase(schema_internal_to_delete.name());

  return butil::Status::OK();
}

// GetSchemas
// get schemas
// in: schema_id
// out: schemas
butil::Status CoordinatorControl::GetSchemas(uint64_t schema_id, std::vector<pb::meta::Schema>& schemas) {
  // only root schema can has sub schemas
  if (schema_id != 0) {
    DINGO_LOG(ERROR) << "ERRROR: schema_id illegal " << schema_id;
    return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS, "schema_id is illegal");
  }

  if (!schemas.empty()) {
    DINGO_LOG(ERROR) << "ERRROR: vector schemas is not empty , size=" << schemas.size();
    return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS, "vector schemas is not empty");
  }

  {
    // BAIDU_SCOPED_LOCK(schema_map_mutex_);
    butil::FlatMap<uint64_t, pb::coordinator_internal::SchemaInternal> schema_map_copy;
    schema_map_copy.init(10000);
    int ret = schema_map_.GetFlatMapCopy(schema_map_copy);
    if (ret < 0) {
      DINGO_LOG(ERROR) << "ERRROR: schema_map_ GetFlatMapCopy failed";
      return butil::Status(pb::error::Errno::EINTERNAL, "schema_map_ GetFlatMapCopy failed");
    }

    for (const auto& it : schema_map_copy) {
      pb::meta::Schema schema;

      auto* temp_id = schema.mutable_id();
      temp_id->set_entity_id(it.first);
      temp_id->set_parent_entity_id(schema_id);
      temp_id->set_entity_type(::dingodb::pb::meta::EntityType::ENTITY_TYPE_SCHEMA);

      schema.set_name(it.second.name());

      // construct table_ids in schema
      for (auto it_table : it.second.table_ids()) {
        pb::meta::DingoCommonId table_id;
        table_id.set_entity_id(it_table);
        table_id.set_parent_entity_id(temp_id->entity_id());
        table_id.set_entity_type(::dingodb::pb::meta::EntityType::ENTITY_TYPE_TABLE);

        schema.add_table_ids()->CopyFrom(table_id);
      }

      schemas.push_back(schema);
    }
  }

  DINGO_LOG(INFO) << "GetSchemas id=" << schema_id << " sub schema count=" << schema_map_.Size();

  return butil::Status::OK();
}

// GetSchema
// in: schema_id
// out: schema
butil::Status CoordinatorControl::GetSchema(uint64_t schema_id, pb::meta::Schema& schema) {
  // only root schema can has sub schemas
  if (schema_id < 0) {
    DINGO_LOG(ERROR) << "ERRROR: schema_id illegal " << schema_id;
    return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS, "schema_id is illegal");
  }

  {
    // BAIDU_SCOPED_LOCK(schema_map_mutex_);
    pb::coordinator_internal::SchemaInternal temp_schema;
    int ret = schema_map_.Get(schema_id, temp_schema);
    if (ret < 0) {
      DINGO_LOG(ERROR) << "ERRROR: schema_id not found " << schema_id;
      return butil::Status(pb::error::Errno::ESCHEMA_NOT_FOUND, "schema_id not found");
    }

    auto* temp_id = schema.mutable_id();
    temp_id->set_entity_id(temp_schema.id());
    temp_id->set_parent_entity_id(::dingodb::pb::meta::ROOT_SCHEMA);
    temp_id->set_entity_type(::dingodb::pb::meta::EntityType::ENTITY_TYPE_SCHEMA);

    schema.set_name(temp_schema.name());

    for (auto it : temp_schema.table_ids()) {
      pb::meta::DingoCommonId table_id;
      table_id.set_entity_id(it);
      table_id.set_parent_entity_id(schema_id);
      table_id.set_entity_type(::dingodb::pb::meta::EntityType::ENTITY_TYPE_TABLE);

      schema.add_table_ids()->CopyFrom(table_id);
    }
  }

  DINGO_LOG(INFO) << "GetSchema id=" << schema_id << " sub table count=" << schema.table_ids_size();

  return butil::Status::OK();
}

// GetSchemaByName
// in: schema_name
// out: schema
butil::Status CoordinatorControl::GetSchemaByName(const std::string& schema_name, pb::meta::Schema& schema) {
  if (schema_name.empty()) {
    DINGO_LOG(ERROR) << "ERRROR: schema_name illegal " << schema_name;
    return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS, "schema_name illegal");
  }

  uint64_t temp_schema_id = 0;
  auto ret = schema_name_map_safe_temp_.Get(schema_name, temp_schema_id);
  if (ret < 0) {
    DINGO_LOG(WARNING) << "WARNING: schema_name not found " << schema_name;
    return butil::Status(pb::error::Errno::ESCHEMA_NOT_FOUND, "schema_name not found");
  }

  DINGO_LOG(INFO) << "GetSchemaByName name=" << schema_name << " sub table count=" << schema.table_ids_size();

  return GetSchema(temp_schema_id, schema);
}

// CreateTableId
// in: schema_id
// out: new_table_id, meta_increment
// return: 0 success, -1 failed
butil::Status CoordinatorControl::CreateTableId(uint64_t schema_id, uint64_t& new_table_id,
                                                pb::coordinator_internal::MetaIncrement& meta_increment) {
  // validate schema_id is existed
  {
    // BAIDU_SCOPED_LOCK(schema_map_mutex_);
    bool ret = schema_map_.Exists(schema_id);
    if (!ret) {
      DINGO_LOG(ERROR) << "schema_id is illegal " << schema_id;
      return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS, "schema_id is illegal");
    }
  }

  // create table id
  new_table_id = GetNextId(pb::coordinator_internal::IdEpochType::ID_NEXT_TABLE, meta_increment);
  DINGO_LOG(INFO) << "CreateTableId new_table_id=" << new_table_id;

  return butil::Status::OK();
}

// CreateTable
// in: schema_id, table_definition
// out: new_table_id, meta_increment
// return: 0 success, -1 failed
butil::Status CoordinatorControl::CreateTable(uint64_t schema_id, const pb::meta::TableDefinition& table_definition,
                                              uint64_t& new_table_id,
                                              pb::coordinator_internal::MetaIncrement& meta_increment) {
  // validate schema
  // root schema cannot create table
  if (schema_id < 0 || schema_id == ::dingodb::pb::meta::ROOT_SCHEMA) {
    DINGO_LOG(ERROR) << "schema_id is illegal " << schema_id;
    return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS, "schema_id is illegal");
  }

  {
    // BAIDU_SCOPED_LOCK(schema_map_mutex_);
    bool ret = schema_map_.Exists(schema_id);
    if (!ret) {
      DINGO_LOG(ERROR) << "schema_id is illegal " << schema_id;
      return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS, "schema_id is illegal");
    }
  }

  bool has_auto_increment_column = false;
  butil::Status ret =
      AutoIncrementControl::CheckAutoIncrementInTableDefinition(table_definition, has_auto_increment_column);
  if (!ret.ok()) {
    DINGO_LOG(ERROR) << "check auto increment in table definition error.";
    return ret;
  }

  // validate part information
  if (!table_definition.has_table_partition()) {
    DINGO_LOG(ERROR) << "no table_partition provided ";
    return butil::Status(pb::error::Errno::ETABLE_DEFINITION_ILLEGAL, "no table_partition provided");
  }
  auto const& table_partition = table_definition.table_partition();
  if (table_partition.has_hash_partition()) {
    DINGO_LOG(ERROR) << "hash_partiton is not supported";
    return butil::Status(pb::error::Errno::ETABLE_DEFINITION_ILLEGAL, "hash_partiton is not supported");
  } else if (!table_partition.has_range_partition()) {
    DINGO_LOG(ERROR) << "no range_partition provided ";
    return butil::Status(pb::error::Errno::ETABLE_DEFINITION_ILLEGAL, "no range_partition provided");
  }

  auto const& range_partition = table_partition.range_partition();
  if (range_partition.ranges_size() == 0) {
    DINGO_LOG(ERROR) << "no range provided ";
    return butil::Status(pb::error::Errno::ETABLE_DEFINITION_ILLEGAL, "no range provided");
  }

  // check if table_name exists
  uint64_t value = 0;
  table_name_map_safe_temp_.Get(std::to_string(schema_id) + table_definition.name(), value);
  if (value != 0) {
    DINGO_LOG(INFO) << " Createtable table_name is exist " << table_definition.name();
    return butil::Status(pb::error::Errno::ETABLE_EXISTS,
                         fmt::format("table_name[{}] is exist in get", table_definition.name().c_str()));
  }

  // if new_table_id is not given, create a new table_id
  if (new_table_id <= 0) {
    new_table_id = GetNextId(pb::coordinator_internal::IdEpochType::ID_NEXT_TABLE, meta_increment);
    DINGO_LOG(INFO) << "CreateTable new_table_id=" << new_table_id;
  }

  // create auto increment
  if (has_auto_increment_column) {
    auto status =
        AutoIncrementControl::SyncSendCreateAutoIncrementInternal(new_table_id, table_definition.auto_increment());
    if (!status.ok()) {
      DINGO_LOG(ERROR) << fmt::format("send create auto increment internal error, code: {}, message: {} ",
                                      status.error_code(), status.error_str());
      return butil::Status(pb::error::Errno::EAUTO_INCREMENT_WHILE_CREATING_TABLE,
                           fmt::format("send create auto increment internal error, code: {}, message: {}",
                                       status.error_code(), status.error_str()));
    }
    DINGO_LOG(INFO) << "CreateTable AutoIncrement send create auto increment internal success";
  }

  // update table_name_map_safe_temp_
  if (table_name_map_safe_temp_.PutIfAbsent(std::to_string(schema_id) + table_definition.name(), new_table_id) < 0) {
    DINGO_LOG(INFO) << " CreateTable table_name" << table_definition.name()
                    << " is exist, when insert new_table_id=" << new_table_id;
    return butil::Status(pb::error::Errno::ETABLE_EXISTS,
                         fmt::format("table_name[{}] is exist in put if absent", table_definition.name().c_str()));
  }

  // create table
  // extract part info, create region for each part

  std::vector<uint64_t> new_region_ids;
  int32_t replica = table_definition.replica();
  if (replica < 1) {
    replica = 3;
  }

  pb::common::IndexParameter index_parameter;
  for (int i = 0; i < range_partition.ranges_size(); i++) {
    // int ret = CreateRegion(const std::string &region_name, const std::string
    // &resource_tag, int32_t replica_num, pb::common::Range region_range,
    // uint64_t schema_id, uint64_t table_id, uint64_t &new_region_id)
    std::string const region_name = std::string("T_") + std::to_string(schema_id) + std::string("_") +
                                    table_definition.name() + std::string("_part_") + std::to_string(i);
    uint64_t new_region_id = 0;

    auto ret = CreateRegion(region_name, pb::common::RegionType::STORE_REGION, "", replica, range_partition.ranges(i),
                            schema_id, new_table_id, 0, index_parameter, new_region_id, meta_increment);
    if (!ret.ok()) {
      DINGO_LOG(ERROR) << "CreateRegion failed in CreateTable table_name=" << table_definition.name();
      break;
    }

    DINGO_LOG(INFO) << "CreateTable create region success, region_id=" << new_region_id;

    new_region_ids.push_back(new_region_id);
  }

  if (new_region_ids.size() < range_partition.ranges_size()) {
    DINGO_LOG(ERROR) << "Not enough regions is created, drop residual regions need=" << range_partition.ranges_size()
                     << " created=" << new_region_ids.size();
    for (auto region_id_to_delete : new_region_ids) {
      auto ret = DropRegion(region_id_to_delete, meta_increment);
      if (!ret.ok()) {
        DINGO_LOG(ERROR) << "DropRegion failed in CreateTable table_name=" << table_definition.name()
                         << " region_id =" << region_id_to_delete;
      }
    }

    // remove table_name from map
    table_name_map_safe_temp_.Erase(std::to_string(schema_id) + table_definition.name());
    return butil::Status(pb::error::Errno::ETABLE_REGION_CREATE_FAILED, "Not enough regions is created");
  }

  // bumper up EPOCH_REGION
  GetNextId(pb::coordinator_internal::IdEpochType::EPOCH_REGION, meta_increment);

  // create table_internal, set id & table_definition
  pb::coordinator_internal::TableInternal table_internal;
  table_internal.set_id(new_table_id);
  table_internal.set_schema_id(schema_id);
  auto* definition = table_internal.mutable_definition();
  definition->CopyFrom(table_definition);

  // set part for table_internal
  for (unsigned long new_region_id : new_region_ids) {
    // create part and set region_id & range
    auto* part_internal = table_internal.add_partitions();
    part_internal->set_region_id(new_region_id);
    // auto* part_range = part_internal->mutable_range();
    // part_range->CopyFrom(range_partition.ranges(i));
  }

  // add table_internal to table_map_
  // update meta_increment
  GetNextId(pb::coordinator_internal::IdEpochType::EPOCH_TABLE, meta_increment);
  auto* table_increment = meta_increment.add_tables();
  table_increment->set_id(new_table_id);
  table_increment->set_op_type(::dingodb::pb::coordinator_internal::MetaIncrementOpType::CREATE);
  // table_increment->set_schema_id(schema_id);

  auto* table_increment_table = table_increment->mutable_table();
  table_increment_table->CopyFrom(table_internal);

  // on_apply
  //  table_map_epoch++;                                               // raft_kv_put
  //  table_map_.insert(std::make_pair(new_table_id, table_internal));  // raft_kv_put

  // add table_id to schema
  // auto* table_id = schema_map_[schema_id].add_table_ids();
  // table_id->set_entity_id(new_table_id);
  // table_id->set_parent_entity_id(schema_id);
  // table_id->set_entity_type(::dingodb::pb::meta::EntityType::ENTITY_TYPE_TABLE);
  // raft_kv_put

  return butil::Status::OK();
}

// DropTable
// in: schema_id, table_id
// out: meta_increment
// return: errno
butil::Status CoordinatorControl::DropTable(uint64_t schema_id, uint64_t table_id,
                                            pb::coordinator_internal::MetaIncrement& meta_increment) {
  if (schema_id < 0) {
    DINGO_LOG(ERROR) << "ERRROR: schema_id illegal " << schema_id;
    return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS, "schema_id illegal");
  }

  if (!ValidateSchema(schema_id)) {
    DINGO_LOG(ERROR) << "ERRROR: schema_id not valid" << schema_id;
    return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS, "schema_id not valid");
  }

  pb::coordinator_internal::TableInternal table_internal;
  {
    // BAIDU_SCOPED_LOCK(table_map_mutex_);
    int ret = table_map_.Get(table_id, table_internal);
    if (ret < 0) {
      DINGO_LOG(ERROR) << "ERRROR: table_id not found" << table_id;
      return butil::Status(pb::error::Errno::ETABLE_NOT_FOUND, "table_id not found");
    }
  }

  // call DropRegion
  for (int i = 0; i < table_internal.partitions_size(); i++) {
    // part id
    uint64_t region_id = table_internal.partitions(i).region_id();

    DropRegion(region_id, meta_increment);
  }

  // delete table
  auto* table_to_delete = meta_increment.add_tables();
  table_to_delete->set_id(table_id);
  table_to_delete->set_op_type(::dingodb::pb::coordinator_internal::MetaIncrementOpType::DELETE);
  // table_to_delete->set_schema_id(schema_id);

  auto* table_to_delete_table = table_to_delete->mutable_table();
  table_to_delete_table->CopyFrom(table_internal);

  // bump up table map epoch
  GetNextId(pb::coordinator_internal::IdEpochType::EPOCH_TABLE, meta_increment);

  // delete table_name from table_name_safe_map_temp_
  table_name_map_safe_temp_.Erase(std::to_string(schema_id) + table_internal.definition().name());

  bool has_auto_increment_column = false;
  AutoIncrementControl::CheckAutoIncrementInTableDefinition(table_internal.definition(), has_auto_increment_column);
  if (has_auto_increment_column) {
    AutoIncrementControl::AsyncSendDeleteAutoIncrementInternal(table_id);
  }

  return butil::Status::OK();
}

// CreateIndexId
// in: schema_id
// out: new_index_id, meta_increment
// return: 0 success, -1 failed
butil::Status CoordinatorControl::CreateIndexId(uint64_t schema_id, uint64_t& new_index_id,
                                                pb::coordinator_internal::MetaIncrement& meta_increment) {
  // validate schema_id is existed
  {
    // BAIDU_SCOPED_LOCK(schema_map_mutex_);
    bool ret = schema_map_.Exists(schema_id);
    if (!ret) {
      DINGO_LOG(ERROR) << "schema_id is illegal " << schema_id;
      return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS, "schema_id is illegal");
    }
  }

  // create index id
  // new_index_id = GetNextId(pb::coordinator_internal::IdEpochType::ID_NEXT_INDEX, meta_increment);
  new_index_id = GetNextId(pb::coordinator_internal::IdEpochType::ID_NEXT_TABLE, meta_increment);
  DINGO_LOG(INFO) << "CreateIndexId new_index_id=" << new_index_id;

  return butil::Status::OK();
}

// validate index definition
// in: index_definition
// return: errno
butil::Status CoordinatorControl::ValidateIndexDefinition(const pb::meta::IndexDefinition& index_definition) {
  // validate index name
  if (index_definition.name().empty()) {
    DINGO_LOG(ERROR) << "index name is empty";
    return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS, "index name is empty");
  }

  const auto& index_parameter = index_definition.index_parameter();

  // check index_type is not NONE
  if (index_parameter.index_type() == pb::common::IndexType::INDEX_TYPE_NONE) {
    DINGO_LOG(ERROR) << "index_type is NONE";
    return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS, "index_type is NONE");
  }

  // if index_type is INDEX_TYPE_VECTOR, check vector_index_parameter
  if (index_parameter.index_type() == pb::common::IndexType::INDEX_TYPE_VECTOR) {
    if (!index_parameter.has_vector_index_parameter()) {
      DINGO_LOG(ERROR) << "index_type is INDEX_TYPE_VECTOR, but vector_index_parameter is not set";
      return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS,
                           "index_type is INDEX_TYPE_VECTOR, but vector_index_parameter is not set");
    }

    const auto& vector_index_parameter = index_parameter.vector_index_parameter();

    // check vector_index_parameter.index_type is not NONE
    if (vector_index_parameter.vector_index_type() == pb::common::VectorIndexType::VECTOR_INDEX_TYPE_NONE) {
      DINGO_LOG(ERROR) << "vector_index_parameter.index_type is NONE";
      return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS, "vector_index_parameter.index_type is NONE");
    }

    // if vector_index_type is HNSW, check hnsw_parameter is set
    if (vector_index_parameter.vector_index_type() == pb::common::VectorIndexType::VECTOR_INDEX_TYPE_HNSW) {
      if (!vector_index_parameter.has_hnsw_parameter()) {
        DINGO_LOG(ERROR) << "vector_index_type is HNSW, but hnsw_parameter is not set";
        return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS,
                             "vector_index_type is HNSW, but hnsw_parameter is not set");
      }

      const auto& hnsw_parameter = vector_index_parameter.hnsw_parameter();

      // check hnsw_parameter.dimension
      // The dimension of the vector space. This parameter is required and must be greater than 0.
      if (hnsw_parameter.dimension() <= 0) {
        DINGO_LOG(ERROR) << "hnsw_parameter.dimension is illegal " << hnsw_parameter.dimension();
        return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS,
                             "hnsw_parameter.dimension is illegal " + std::to_string(hnsw_parameter.dimension()));
      }

      // check hnsw_parameter.metric_type
      // The distance metric used to calculate the similarity between vectors. This parameter is required and must not
      // be METRIC_TYPE_NONE.
      if (hnsw_parameter.metric_type() == pb::common::METRIC_TYPE_NONE) {
        DINGO_LOG(ERROR) << "hnsw_parameter.metric_type is illegal " << hnsw_parameter.metric_type();
        return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS,
                             "hnsw_parameter.metric_type is illegal " + std::to_string(hnsw_parameter.metric_type()));
      }

      // check hnsw_parameter.ef_construction
      // The size of the dynamic list for the nearest neighbors during the construction of the graph. This parameter
      // affects the quality of the graph and the construction time. A larger value leads to a higher quality graph but
      // slower construction time. This parameter must be greater than 0.
      if (hnsw_parameter.efconstruction() <= 0) {
        DINGO_LOG(ERROR) << "hnsw_parameter.ef_construction is illegal " << hnsw_parameter.efconstruction();
        return butil::Status(
            pb::error::Errno::EILLEGAL_PARAMTETERS,
            "hnsw_parameter.ef_construction is illegal " + std::to_string(hnsw_parameter.efconstruction()));
      }

      // check hnsw_parameter.max_elements
      // The maximum number of elements that can be indexed. This parameter affects the memory usage of the index. This
      // parameter must be greater than 0.
      if (hnsw_parameter.max_elements() <= 0) {
        DINGO_LOG(ERROR) << "hnsw_parameter.max_elements is illegal " << hnsw_parameter.max_elements();
        return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS,
                             "hnsw_parameter.max_elements is illegal " + std::to_string(hnsw_parameter.max_elements()));
      }

      // check hnsw_parameter.nlinks
      // The number of links for each element in the graph. This parameter affects the quality of the graph and the
      // search time. A larger value leads to a higher quality graph but slower search time. This parameter must be
      // greater than 0.
      if (hnsw_parameter.nlinks() <= 0) {
        DINGO_LOG(ERROR) << "hnsw_parameter.nlinks is illegal " << hnsw_parameter.nlinks();
        return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS,
                             "hnsw_parameter.nlinks is illegal " + std::to_string(hnsw_parameter.nlinks()));
      }
    }

    // if vector_index_type is FLAT, check flat_parameter is set
    if (vector_index_parameter.vector_index_type() == pb::common::VectorIndexType::VECTOR_INDEX_TYPE_FLAT) {
      if (!vector_index_parameter.has_flat_parameter()) {
        DINGO_LOG(ERROR) << "vector_index_type is FLAT, but flat_parameter is not set";
        return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS,
                             "vector_index_type is FLAT, but flat_parameter is not set");
      }

      const auto& flat_parameter = vector_index_parameter.flat_parameter();

      // check flat_parameter.dimension
      if (flat_parameter.dimension() <= 0) {
        DINGO_LOG(ERROR) << "flat_parameter.dimension is illegal " << flat_parameter.dimension();
        return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS,
                             "flat_parameter.dimension is illegal " + std::to_string(flat_parameter.dimension()));
      }

      // check flat_parameter.metric_type
      if (flat_parameter.metric_type() == pb::common::METRIC_TYPE_NONE) {
        DINGO_LOG(ERROR) << "flat_parameter.metric_type is illegal " << flat_parameter.metric_type();
        return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS,
                             "flat_parameter.metric_type is illegal " + std::to_string(flat_parameter.metric_type()));
      }
    }

    // if vector_index_type is IVF_FLAT, check ivf_flat_parameter is set
    if (vector_index_parameter.vector_index_type() == pb::common::VectorIndexType::VECTOR_INDEX_TYPE_IVF_FLAT) {
      if (!vector_index_parameter.has_ivf_flat_parameter()) {
        DINGO_LOG(ERROR) << "vector_index_type is IVF_FLAT, but ivf_flat_parameter is not set";
        return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS,
                             "vector_index_type is IVF_FLAT, but ivf_flat_parameter is not set");
      }

      const auto& ivf_flat_parameter = vector_index_parameter.ivf_flat_parameter();

      // check ivf_flat_parameter.dimension
      // The dimension of the vectors to be indexed. This parameter must be greater than 0.
      if (ivf_flat_parameter.dimension() <= 0) {
        DINGO_LOG(ERROR) << "ivf_flat_parameter.dimension is illegal " << ivf_flat_parameter.dimension();
        return butil::Status(
            pb::error::Errno::EILLEGAL_PARAMTETERS,
            "ivf_flat_parameter.dimension is illegal " + std::to_string(ivf_flat_parameter.dimension()));
      }

      // check ivf_flat_parameter.metric_type
      // The distance metric used to compute the distance between vectors. This parameter affects the accuracy of the
      // search. This parameter must not be METRIC_TYPE_NONE.
      if (ivf_flat_parameter.metric_type() == pb::common::METRIC_TYPE_NONE) {
        DINGO_LOG(ERROR) << "ivf_flat_parameter.metric_type is illegal " << ivf_flat_parameter.metric_type();
        return butil::Status(
            pb::error::Errno::EILLEGAL_PARAMTETERS,
            "ivf_flat_parameter.metric_type is illegal " + std::to_string(ivf_flat_parameter.metric_type()));
      }

      // check ivf_flat_parameter.ncentroids
      // The number of centroids (clusters) used in the product quantization. This parameter affects the memory usage of
      // the index and the accuracy of the search. This parameter must be greater than 0.
      if (ivf_flat_parameter.ncentroids() <= 0) {
        DINGO_LOG(ERROR) << "ivf_flat_parameter.ncentroids is illegal " << ivf_flat_parameter.ncentroids();
        return butil::Status(
            pb::error::Errno::EILLEGAL_PARAMTETERS,
            "ivf_flat_parameter.ncentroids is illegal " + std::to_string(ivf_flat_parameter.ncentroids()));
      }
    }

    // if vector_index_type is IVF_PQ, check ivf_pq_parameter is set
    if (vector_index_parameter.vector_index_type() == pb::common::VectorIndexType::VECTOR_INDEX_TYPE_IVF_PQ) {
      if (!vector_index_parameter.has_ivf_pq_parameter()) {
        DINGO_LOG(ERROR) << "vector_index_type is IVF_PQ, but ivf_pq_parameter is not set";
        return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS,
                             "vector_index_type is IVF_PQ, but ivf_pq_parameter is not set");
      }

      const auto& ivf_pq_parameter = vector_index_parameter.ivf_pq_parameter();

      // check ivf_pq_parameter.dimension
      // The dimension of the vectors to be indexed. This parameter must be greater than 0.
      if (ivf_pq_parameter.dimension() <= 0) {
        DINGO_LOG(ERROR) << "ivf_pq_parameter.dimension is illegal " << ivf_pq_parameter.dimension();
        return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS,
                             "ivf_pq_parameter.dimension is illegal " + std::to_string(ivf_pq_parameter.dimension()));
      }

      // check ivf_pq_parameter.metric_type
      // The distance metric used to compute the distance between vectors. This parameter affects the accuracy of the
      // search. This parameter must not be METRIC_TYPE_NONE.
      if (ivf_pq_parameter.metric_type() == pb::common::METRIC_TYPE_NONE) {
        DINGO_LOG(ERROR) << "ivf_pq_parameter.metric_type is illegal " << ivf_pq_parameter.metric_type();
        return butil::Status(
            pb::error::Errno::EILLEGAL_PARAMTETERS,
            "ivf_pq_parameter.metric_type is illegal " + std::to_string(ivf_pq_parameter.metric_type()));
      }

      // check ivf_pq_parameter.nlist
      // The number of inverted lists (buckets) used in the index. This parameter affects the memory usage of the index
      // and the accuracy of the search. This parameter must be greater than 0.
      if (ivf_pq_parameter.ncentroids() <= 0) {
        DINGO_LOG(ERROR) << "ivf_pq_parameter.ncentroids is illegal " << ivf_pq_parameter.ncentroids();
        return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS,
                             "ivf_pq_parameter.ncentroids is illegal " + std::to_string(ivf_pq_parameter.ncentroids()));
      }

      // check ivf_pq_parameter.nsubvector
      // The number of subvectors used in the product quantization. This parameter affects the memory usage of the index
      // and the accuracy of the search. This parameter must be greater than 0.
      if (ivf_pq_parameter.nsubvector() <= 0) {
        DINGO_LOG(ERROR) << "ivf_pq_parameter.nsubvector is illegal " << ivf_pq_parameter.nsubvector();
        return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS,
                             "ivf_pq_parameter.nsubvector is illegal " + std::to_string(ivf_pq_parameter.nsubvector()));
      }

      // check ivf_pq_parameter.bucket_init_size
      // The number of bits used to represent each subvector in the index. This parameter affects the memory usage of
      // the index and the accuracy of the search. This parameter must be greater than 0.
      if (ivf_pq_parameter.bucket_init_size() <= 0) {
        DINGO_LOG(ERROR) << "ivf_pq_parameter.bucket_init_size is illegal " << ivf_pq_parameter.bucket_init_size();
        return butil::Status(
            pb::error::Errno::EILLEGAL_PARAMTETERS,
            "ivf_pq_parameter.bucket_init_size is illegal " + std::to_string(ivf_pq_parameter.bucket_init_size()));
      }

      // check ivf_pq_parameter.bucket_max_size
      // The maximum number of vectors that can be added to each inverted list (bucket) in the index. This parameter
      // affects the memory usage of the index and the accuracy of the search. This parameter must be greater than 0.
      if (ivf_pq_parameter.bucket_max_size() <= 0) {
        DINGO_LOG(ERROR) << "ivf_pq_parameter.bucket_max_size is illegal " << ivf_pq_parameter.bucket_max_size();
        return butil::Status(
            pb::error::Errno::EILLEGAL_PARAMTETERS,
            "ivf_pq_parameter.bucket_max_size is illegal " + std::to_string(ivf_pq_parameter.bucket_max_size()));
      }

      // If all checks pass, return a butil::Status object with no error.
      return butil::Status::OK();
    }

    // if vector_index_type is diskann, check diskann_parameter is set
    if (vector_index_parameter.vector_index_type() == pb::common::VectorIndexType::VECTOR_INDEX_TYPE_DISKANN) {
      if (!vector_index_parameter.has_diskann_parameter()) {
        DINGO_LOG(ERROR) << "vector_index_type is DISKANN, but diskann_parameter is not set";
        return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS,
                             "vector_index_type is DISKANN, but diskann_parameter is not set");
      }

      const auto& diskann_parameter = vector_index_parameter.diskann_parameter();

      // check diskann_parameter.dimension
      // The dimension of the vectors to be indexed. This parameter must be greater than 0.
      if (diskann_parameter.dimension() <= 0) {
        DINGO_LOG(ERROR) << "diskann_parameter.dimension is illegal " << diskann_parameter.dimension();
        return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS,
                             "diskann_parameter.dimension is illegal " + std::to_string(diskann_parameter.dimension()));
      }

      // check diskann_parameter.metric_type
      // The distance metric used to compute the distance between vectors. This parameter affects the accuracy of the
      // search. This parameter must not be METRIC_TYPE_NONE.
      if (diskann_parameter.metric_type() == pb::common::METRIC_TYPE_NONE) {
        DINGO_LOG(ERROR) << "diskann_parameter.metric_type is illegal " << diskann_parameter.metric_type();
        return butil::Status(
            pb::error::Errno::EILLEGAL_PARAMTETERS,
            "diskann_parameter.metric_type is illegal " + std::to_string(diskann_parameter.metric_type()));
      }

      // check diskann_parameter.num_trees
      // The number of trees to be built in the index. This parameter affects the memory usage of the index and the
      // accuracy of the search. This parameter must be greater than 0.
      if (diskann_parameter.num_trees() <= 0) {
        DINGO_LOG(ERROR) << "diskann_parameter.num_trees is illegal " << diskann_parameter.num_trees();
        return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS,
                             "diskann_parameter.num_trees is illegal " + std::to_string(diskann_parameter.num_trees()));
      }

      // check diskann_parameter.num_neighbors
      // The number of nearest neighbors to be returned by the search. This parameter affects the accuracy of the
      // search. This parameter must be greater than 0.
      if (diskann_parameter.num_neighbors() <= 0) {
        DINGO_LOG(ERROR) << "diskann_parameter.num_neighbors is illegal " << diskann_parameter.num_neighbors();
        return butil::Status(
            pb::error::Errno::EILLEGAL_PARAMTETERS,
            "diskann_parameter.num_neighbors is illegal " + std::to_string(diskann_parameter.num_neighbors()));
      }

      // check diskann_parameter.num_threads
      // The number of CPU cores to be used in building the index. This parameter affects the speed of the index
      // building. This parameter must be greater than 0.
      if (diskann_parameter.num_threads() <= 0) {
        DINGO_LOG(ERROR) << "diskann_parameter.num_threads is illegal " << diskann_parameter.num_threads();
        return butil::Status(
            pb::error::Errno::EILLEGAL_PARAMTETERS,
            "diskann_parameter.num_threads is illegal " + std::to_string(diskann_parameter.num_threads()));
      }

      // If all checks pass, return a butil::Status object with no error.
      return butil::Status::OK();
    }

    return butil::Status::OK();
  } else if (index_parameter.index_type() == pb::common::IndexType::INDEX_TYPE_SCALAR) {
    // check if scalar_index_parameter is set
    if (!index_parameter.has_scalar_index_parameter()) {
      DINGO_LOG(ERROR) << "index_type is SCALAR, but scalar_index_parameter is not set";
      return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS,
                           "index_type is SCALAR, but scalar_index_parameter is not set");
    }

    const auto& scalar_index_parameter = index_parameter.scalar_index_parameter();

    // check scalar index type
    if (scalar_index_parameter.scalar_index_type() == pb::common::ScalarIndexType::SCALAR_INDEX_TYPE_NONE) {
      DINGO_LOG(ERROR) << "scalar_index_type is illegal " << scalar_index_parameter.scalar_index_type();
      return butil::Status(
          pb::error::Errno::EILLEGAL_PARAMTETERS,
          "scalar_index_type is illegal " + std::to_string(scalar_index_parameter.scalar_index_type()));
    }
  }

  return butil::Status::OK();
}

// CreateIndex
// in: schema_id, index_definition
// out: new_index_id, meta_increment
// return: 0 success, -1 failed
butil::Status CoordinatorControl::CreateIndex(uint64_t schema_id, const pb::meta::IndexDefinition& index_definition,
                                              uint64_t& new_index_id,
                                              pb::coordinator_internal::MetaIncrement& meta_increment) {
  // validate schema
  // root schema cannot create index
  if (schema_id < 0 || schema_id == ::dingodb::pb::meta::ROOT_SCHEMA) {
    DINGO_LOG(ERROR) << "schema_id is illegal " << schema_id;
    return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS, "schema_id is illegal");
  }

  {
    // BAIDU_SCOPED_LOCK(schema_map_mutex_);
    bool ret = schema_map_.Exists(schema_id);
    if (!ret) {
      DINGO_LOG(ERROR) << "schema_id is illegal " << schema_id;
      return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS, "schema_id is illegal");
    }
  }

  // validate index_definition
  auto status = ValidateIndexDefinition(index_definition);
  if (!status.ok()) {
    return status;
  }

  // validate part information
  if (!index_definition.has_index_partition()) {
    DINGO_LOG(ERROR) << "no index_partition provided ";
    return butil::Status(pb::error::Errno::EINDEX_DEFINITION_ILLEGAL, "no index_partition provided");
  }

  auto const& index_partition = index_definition.index_partition();
  if (index_partition.has_hash_partition()) {
    DINGO_LOG(ERROR) << "hash_partiton is not supported";
    return butil::Status(pb::error::Errno::EINDEX_DEFINITION_ILLEGAL, "hash_partiton is not supported");
  } else if (!index_partition.has_range_partition()) {
    DINGO_LOG(ERROR) << "no range_partition provided ";
    return butil::Status(pb::error::Errno::EINDEX_DEFINITION_ILLEGAL, "no range_partition provided");
  }

  auto const& range_partition = index_partition.range_partition();
  if (range_partition.ranges_size() == 0) {
    DINGO_LOG(ERROR) << "no range provided ";
    return butil::Status(pb::error::Errno::EINDEX_DEFINITION_ILLEGAL, "no range provided");
  }

  // check if index_name exists
  uint64_t value = 0;
  index_name_map_safe_temp_.Get(std::to_string(schema_id) + index_definition.name(), value);
  if (value != 0) {
    DINGO_LOG(INFO) << " Createindex index_name is exist " << index_definition.name();
    return butil::Status(pb::error::Errno::EINDEX_EXISTS,
                         fmt::format("index_name[{}] is exist in get", index_definition.name().c_str()));
  }

  // if new_index_id is not given, create a new index_id
  if (new_index_id <= 0) {
    // new_index_id = GetNextId(pb::coordinator_internal::IdEpochType::ID_NEXT_INDEX, meta_increment);
    new_index_id = GetNextId(pb::coordinator_internal::IdEpochType::ID_NEXT_TABLE, meta_increment);
    DINGO_LOG(INFO) << "CreateIndex new_index_id=" << new_index_id;
  }

  // create auto increment
  if (index_definition.with_auto_incrment()) {
    auto status =
        AutoIncrementControl::SyncSendCreateAutoIncrementInternal(new_index_id, index_definition.auto_increment());
    if (!status.ok()) {
      DINGO_LOG(ERROR) << fmt::format("send create auto increment internal error, code: {}, message: {} ",
                                      status.error_code(), status.error_str());
      return butil::Status(pb::error::Errno::EAUTO_INCREMENT_WHILE_CREATING_TABLE,
                           fmt::format("send create auto increment internal error, code: {}, message: {}",
                                       status.error_code(), status.error_str()));
    }
    DINGO_LOG(INFO) << "CreateIndex AutoIncrement send create auto increment internal success";
  }

  // update index_name_map_safe_temp_
  if (index_name_map_safe_temp_.PutIfAbsent(std::to_string(schema_id) + index_definition.name(), new_index_id) < 0) {
    DINGO_LOG(INFO) << " CreateIndex index_name" << index_definition.name()
                    << " is exist, when insert new_index_id=" << new_index_id;
    return butil::Status(pb::error::Errno::EINDEX_EXISTS,
                         fmt::format("index_name[{}] is exist in put if absent", index_definition.name().c_str()));
  }

  // create index
  // extract part info, create region for each part

  std::vector<uint64_t> new_region_ids;
  int32_t replica = index_definition.replica();
  if (replica < 1) {
    replica = 3;
  }
  for (int i = 0; i < range_partition.ranges_size(); i++) {
    // int ret = CreateRegion(const std::string &region_name, const std::string
    // &resource_tag, int32_t replica_num, pb::common::Range region_range,
    // uint64_t schema_id, uint64_t index_id, uint64_t &new_region_id)
    std::string const region_name = std::string("I_") + std::to_string(schema_id) + std::string("_") +
                                    index_definition.name() + std::string("_part_") + std::to_string(i);
    uint64_t new_region_id = 0;

    auto ret =
        CreateRegion(region_name, pb::common::RegionType::INDEX_REGION, "", replica, range_partition.ranges(i),
                     schema_id, 0, new_index_id, index_definition.index_parameter(), new_region_id, meta_increment);
    if (!ret.ok()) {
      DINGO_LOG(ERROR) << "CreateRegion failed in CreateIndex index_name=" << index_definition.name();
      break;
    }

    DINGO_LOG(INFO) << "CreateIndex create region success, region_id=" << new_region_id;

    new_region_ids.push_back(new_region_id);
  }

  if (new_region_ids.size() < range_partition.ranges_size()) {
    DINGO_LOG(ERROR) << "Not enough regions is created, drop residual regions need=" << range_partition.ranges_size()
                     << " created=" << new_region_ids.size();
    for (auto region_id_to_delete : new_region_ids) {
      auto ret = DropRegion(region_id_to_delete, meta_increment);
      if (!ret.ok()) {
        DINGO_LOG(ERROR) << "DropRegion failed in CreateIndex index_name=" << index_definition.name()
                         << " region_id =" << region_id_to_delete;
      }
    }

    // remove index_name from map
    index_name_map_safe_temp_.Erase(std::to_string(schema_id) + index_definition.name());
    return butil::Status(pb::error::Errno::EINDEX_REGION_CREATE_FAILED, "Not enough regions is created");
  }

  // bumper up EPOCH_REGION
  GetNextId(pb::coordinator_internal::IdEpochType::EPOCH_REGION, meta_increment);

  // create index_internal, set id & index_definition
  pb::coordinator_internal::IndexInternal index_internal;
  index_internal.set_id(new_index_id);
  index_internal.set_schema_id(schema_id);
  auto* definition = index_internal.mutable_definition();
  definition->CopyFrom(index_definition);

  // set part for index_internal
  for (unsigned long new_region_id : new_region_ids) {
    // create part and set region_id & range
    auto* part_internal = index_internal.add_partitions();
    part_internal->set_region_id(new_region_id);
    // auto* part_range = part_internal->muindex_range();
    // part_range->CopyFrom(range_partition.ranges(i));
  }

  // add index_internal to index_map_
  // update meta_increment
  GetNextId(pb::coordinator_internal::IdEpochType::EPOCH_INDEX, meta_increment);
  auto* index_increment = meta_increment.add_indexes();
  index_increment->set_id(new_index_id);
  index_increment->set_op_type(::dingodb::pb::coordinator_internal::MetaIncrementOpType::CREATE);
  // index_increment->set_schema_id(schema_id);

  auto* index_increment_index = index_increment->mutable_index();
  index_increment_index->CopyFrom(index_internal);

  // on_apply
  //  index_map_epoch++;                                               // raft_kv_put
  //  index_map_.insert(std::make_pair(new_index_id, index_internal));  // raft_kv_put

  // add index_id to schema
  // auto* index_id = schema_map_[schema_id].add_index_ids();
  // index_id->set_entity_id(new_index_id);
  // index_id->set_parent_entity_id(schema_id);
  // index_id->set_entity_type(::dingodb::pb::meta::EntityType::ENTITY_TYPE_INDEX);
  // raft_kv_put

  return butil::Status::OK();
}

// DropIndex
// in: schema_id, index_id
// out: meta_increment
// return: errno
butil::Status CoordinatorControl::DropIndex(uint64_t schema_id, uint64_t index_id,
                                            pb::coordinator_internal::MetaIncrement& meta_increment) {
  if (schema_id < 0) {
    DINGO_LOG(ERROR) << "ERRROR: schema_id illegal " << schema_id;
    return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS, "schema_id illegal");
  }

  if (!ValidateSchema(schema_id)) {
    DINGO_LOG(ERROR) << "ERRROR: schema_id not valid" << schema_id;
    return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS, "schema_id not valid");
  }

  pb::coordinator_internal::IndexInternal index_internal;
  {
    // BAIDU_SCOPED_LOCK(index_map_mutex_);
    int ret = index_map_.Get(index_id, index_internal);
    if (ret < 0) {
      DINGO_LOG(ERROR) << "ERRROR: index_id not found" << index_id;
      return butil::Status(pb::error::Errno::EINDEX_NOT_FOUND, "index_id not found");
    }
  }

  // call DropRegion
  for (int i = 0; i < index_internal.partitions_size(); i++) {
    // part id
    uint64_t region_id = index_internal.partitions(i).region_id();

    DropRegion(region_id, meta_increment);
  }

  // delete index
  auto* index_to_delete = meta_increment.add_indexes();
  index_to_delete->set_id(index_id);
  index_to_delete->set_op_type(::dingodb::pb::coordinator_internal::MetaIncrementOpType::DELETE);
  // index_to_delete->set_schema_id(schema_id);

  auto* index_to_delete_index = index_to_delete->mutable_index();
  index_to_delete_index->CopyFrom(index_internal);

  // bump up index map epoch
  GetNextId(pb::coordinator_internal::IdEpochType::EPOCH_INDEX, meta_increment);

  // delete index_name from index_name_safe_map_temp_
  index_name_map_safe_temp_.Erase(std::to_string(schema_id) + index_internal.definition().name());

  if (index_internal.definition().with_auto_incrment()) {
    AutoIncrementControl::AsyncSendDeleteAutoIncrementInternal(index_id);
  }

  return butil::Status::OK();
}

// get tables
butil::Status CoordinatorControl::GetTables(uint64_t schema_id,
                                            std::vector<pb::meta::TableDefinitionWithId>& table_definition_with_ids) {
  DINGO_LOG(INFO) << "GetTables in control schema_id=" << schema_id;

  if (schema_id < 0) {
    DINGO_LOG(ERROR) << "ERRROR: schema_id illegal " << schema_id;
    return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS, "schema_id illegal");
  }

  if (!table_definition_with_ids.empty()) {
    DINGO_LOG(ERROR) << "ERRROR: vector table_definition_with_ids is not empty , size="
                     << table_definition_with_ids.size();
    return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS, "vector table_definition_with_ids is not empty");
  }

  {
    // BAIDU_SCOPED_LOCK(schema_map_mutex_);
    pb::coordinator_internal::SchemaInternal schema_internal;
    int ret = schema_map_.Get(schema_id, schema_internal);
    if (ret < 0) {
      DINGO_LOG(ERROR) << "ERRROR: schema_id not found" << schema_id;
      return butil::Status(pb::error::Errno::ESCHEMA_NOT_FOUND, "schema_id not found");
    }

    for (int i = 0; i < schema_internal.table_ids_size(); i++) {
      uint64_t table_id = schema_internal.table_ids(i);

      pb::coordinator_internal::TableInternal table_internal;
      int ret = table_map_.Get(table_id, table_internal);
      if (ret < 0) {
        DINGO_LOG(ERROR) << "ERRROR: table_id not found" << table_id;
        continue;
      }

      DINGO_LOG(INFO) << "GetTables found table_id=" << table_id;

      // construct return value
      pb::meta::TableDefinitionWithId table_def_with_id;

      // table_def_with_id.mutable_table_id()->CopyFrom(schema_internal.schema().table_ids(i));
      auto* table_id_for_response = table_def_with_id.mutable_table_id();
      table_id_for_response->set_entity_type(::dingodb::pb::meta::EntityType::ENTITY_TYPE_TABLE);
      table_id_for_response->set_entity_id(table_id);
      table_id_for_response->set_parent_entity_id(schema_id);

      table_def_with_id.mutable_table_definition()->CopyFrom(table_internal.definition());
      table_definition_with_ids.push_back(table_def_with_id);
    }
  }

  DINGO_LOG(INFO) << "GetTables schema_id=" << schema_id << " tables count=" << table_definition_with_ids.size();

  return butil::Status::OK();
}

// get indexs
butil::Status CoordinatorControl::GetIndexs(uint64_t schema_id,
                                            std::vector<pb::meta::IndexDefinitionWithId>& index_definition_with_ids) {
  DINGO_LOG(INFO) << "GetIndexs in control schema_id=" << schema_id;

  if (schema_id < 0) {
    DINGO_LOG(ERROR) << "ERRROR: schema_id illegal " << schema_id;
    return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS, "schema_id illegal");
  }

  if (!index_definition_with_ids.empty()) {
    DINGO_LOG(ERROR) << "ERRROR: vector index_definition_with_ids is not empty , size="
                     << index_definition_with_ids.size();
    return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS, "vector index_definition_with_ids is not empty");
  }

  {
    // BAIDU_SCOPED_LOCK(schema_map_mutex_);
    pb::coordinator_internal::SchemaInternal schema_internal;
    int ret = schema_map_.Get(schema_id, schema_internal);
    if (ret < 0) {
      DINGO_LOG(ERROR) << "ERRROR: schema_id not found" << schema_id;
      return butil::Status(pb::error::Errno::ESCHEMA_NOT_FOUND, "schema_id not found");
    }

    for (int i = 0; i < schema_internal.index_ids_size(); i++) {
      uint64_t index_id = schema_internal.index_ids(i);

      pb::coordinator_internal::IndexInternal index_internal;
      int ret = index_map_.Get(index_id, index_internal);
      if (ret < 0) {
        DINGO_LOG(ERROR) << "ERRROR: index_id not found" << index_id;
        continue;
      }

      DINGO_LOG(INFO) << "GetIndexs found index_id=" << index_id;

      // construct return value
      pb::meta::IndexDefinitionWithId index_def_with_id;

      // index_def_with_id.muindex_index_id()->CopyFrom(schema_internal.schema().index_ids(i));
      auto* index_id_for_response = index_def_with_id.mutable_index_id();
      index_id_for_response->set_entity_type(::dingodb::pb::meta::EntityType::ENTITY_TYPE_INDEX);
      index_id_for_response->set_entity_id(index_id);
      index_id_for_response->set_parent_entity_id(schema_id);

      index_def_with_id.mutable_index_definition()->CopyFrom(index_internal.definition());
      index_definition_with_ids.push_back(index_def_with_id);
    }
  }

  DINGO_LOG(INFO) << "GetIndexs schema_id=" << schema_id << " indexs count=" << index_definition_with_ids.size();

  return butil::Status::OK();
}

// get tables count
butil::Status CoordinatorControl::GetTablesCount(uint64_t schema_id, uint64_t& tables_count) {
  DINGO_LOG(INFO) << "GetTables in control schema_id=" << schema_id;

  if (schema_id < 0) {
    DINGO_LOG(ERROR) << "ERRROR: schema_id illegal " << schema_id;
    return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS, "schema_id illegal");
  }

  {
    // BAIDU_SCOPED_LOCK(schema_map_mutex_);
    pb::coordinator_internal::SchemaInternal schema_internal;
    int ret = schema_map_.Get(schema_id, schema_internal);
    if (ret < 0) {
      DINGO_LOG(ERROR) << "ERRROR: schema_id not found" << schema_id;
      return butil::Status(pb::error::Errno::ESCHEMA_NOT_FOUND, "schema_id not found");
    }

    tables_count = schema_internal.table_ids_size();
  }

  DINGO_LOG(INFO) << "GetTablesCount schema_id=" << schema_id << " tables count=" << tables_count;

  return butil::Status::OK();
}

// get indexs count
butil::Status CoordinatorControl::GetIndexsCount(uint64_t schema_id, uint64_t& indexs_count) {
  DINGO_LOG(INFO) << "GetIndexs in control schema_id=" << schema_id;

  if (schema_id < 0) {
    DINGO_LOG(ERROR) << "ERRROR: schema_id illegal " << schema_id;
    return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS, "schema_id illegal");
  }

  {
    // BAIDU_SCOPED_LOCK(schema_map_mutex_);
    pb::coordinator_internal::SchemaInternal schema_internal;
    int ret = schema_map_.Get(schema_id, schema_internal);
    if (ret < 0) {
      DINGO_LOG(ERROR) << "ERRROR: schema_id not found" << schema_id;
      return butil::Status(pb::error::Errno::ESCHEMA_NOT_FOUND, "schema_id not found");
    }

    indexs_count = schema_internal.index_ids_size();
  }

  DINGO_LOG(INFO) << "GetIndexsCount schema_id=" << schema_id << " indexs count=" << indexs_count;

  return butil::Status::OK();
}

// get table
butil::Status CoordinatorControl::GetTable(uint64_t schema_id, uint64_t table_id,
                                           pb::meta::TableDefinitionWithId& table_definition_with_id) {
  DINGO_LOG(INFO) << "GetTable in control schema_id=" << schema_id;

  if (schema_id < 0) {
    DINGO_LOG(ERROR) << "ERRROR: schema_id illegal " << schema_id;
    return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS, "schema_id illegal");
  }

  if (table_id <= 0) {
    DINGO_LOG(ERROR) << "ERRROR: table illegal, table_id=" << table_id;
    return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS, "table_id illegal");
  }

  // validate schema_id
  if (!ValidateSchema(schema_id)) {
    DINGO_LOG(ERROR) << "ERRROR: schema_id not valid" << schema_id;
    return butil::Status(pb::error::Errno::ESCHEMA_NOT_FOUND, "schema_id not valid");
  }

  // validate table_id & get table definition
  {
    // BAIDU_SCOPED_LOCK(table_map_mutex_);
    pb::coordinator_internal::TableInternal table_internal;
    int ret = table_map_.Get(table_id, table_internal);
    if (ret < 0) {
      DINGO_LOG(ERROR) << "ERRROR: table_id not found" << table_id;
      return butil::Status(pb::error::Errno::ETABLE_NOT_FOUND, "table_id not found");
    }

    DINGO_LOG(INFO) << "GetTable found table_id=" << table_id;

    // table_def_with_id.mutable_table_id()->CopyFrom(schema_internal.schema().table_ids(i));
    auto* table_id_for_response = table_definition_with_id.mutable_table_id();
    table_id_for_response->set_entity_type(::dingodb::pb::meta::EntityType::ENTITY_TYPE_TABLE);
    table_id_for_response->set_entity_id(table_id);
    table_id_for_response->set_parent_entity_id(schema_id);

    table_definition_with_id.mutable_table_definition()->CopyFrom(table_internal.definition());
  }

  DINGO_LOG(DEBUG) << fmt::format("GetTable schema_id={} table_id={} table_definition_with_id={}", schema_id, table_id,
                                  table_definition_with_id.DebugString());

  return butil::Status::OK();
}

// get index
butil::Status CoordinatorControl::GetIndex(uint64_t schema_id, uint64_t index_id,
                                           pb::meta::IndexDefinitionWithId& index_definition_with_id) {
  DINGO_LOG(INFO) << "GetIndex in control schema_id=" << schema_id;

  if (schema_id < 0) {
    DINGO_LOG(ERROR) << "ERRROR: schema_id illegal " << schema_id;
    return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS, "schema_id illegal");
  }

  if (index_id <= 0) {
    DINGO_LOG(ERROR) << "ERRROR: index illegal, index_id=" << index_id;
    return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS, "index_id illegal");
  }

  // validate schema_id
  if (!ValidateSchema(schema_id)) {
    DINGO_LOG(ERROR) << "ERRROR: schema_id not valid" << schema_id;
    return butil::Status(pb::error::Errno::ESCHEMA_NOT_FOUND, "schema_id not valid");
  }

  // validate index_id & get index definition
  {
    // BAIDU_SCOPED_LOCK(index_map_mutex_);
    pb::coordinator_internal::IndexInternal index_internal;
    int ret = index_map_.Get(index_id, index_internal);
    if (ret < 0) {
      DINGO_LOG(ERROR) << "ERRROR: index_id not found" << index_id;
      return butil::Status(pb::error::Errno::EINDEX_NOT_FOUND, "index_id not found");
    }

    DINGO_LOG(INFO) << "GetIndex found index_id=" << index_id;

    // index_def_with_id.muindex_index_id()->CopyFrom(schema_internal.schema().index_ids(i));
    auto* index_id_for_response = index_definition_with_id.mutable_index_id();
    index_id_for_response->set_entity_type(::dingodb::pb::meta::EntityType::ENTITY_TYPE_INDEX);
    index_id_for_response->set_entity_id(index_id);
    index_id_for_response->set_parent_entity_id(schema_id);

    index_definition_with_id.mutable_index_definition()->CopyFrom(index_internal.definition());
  }

  DINGO_LOG(DEBUG) << fmt::format("GetIndex schema_id={} index_id={} index_definition_with_id={}", schema_id, index_id,
                                  index_definition_with_id.DebugString());

  return butil::Status::OK();
}

// get table by name
butil::Status CoordinatorControl::GetTableByName(uint64_t schema_id, const std::string& table_name,
                                                 pb::meta::TableDefinitionWithId& table_definition) {
  DINGO_LOG(INFO) << fmt::format("GetTableByName in control schema_id={} table_name={}", schema_id, table_name);

  if (schema_id < 0) {
    DINGO_LOG(ERROR) << "ERRROR: schema_id illegal " << schema_id;
    return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS, "schema_id illegal");
  }

  if (table_name.empty()) {
    DINGO_LOG(ERROR) << "ERRROR: table_name illegal " << table_name;
    return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS, "table_name illegal");
  }

  if (!ValidateSchema(schema_id)) {
    DINGO_LOG(ERROR) << "ERRROR: schema_id not valid" << schema_id;
    return butil::Status(pb::error::Errno::ESCHEMA_NOT_FOUND, "schema_id not valid");
  }

  uint64_t temp_table_id = 0;
  auto ret = table_name_map_safe_temp_.Get(std::to_string(schema_id) + table_name, temp_table_id);
  if (ret < 0) {
    DINGO_LOG(WARNING) << "WARNING: table_name not found " << table_name;
    return butil::Status(pb::error::Errno::ETABLE_NOT_FOUND, "table_name not found");
  }

  DINGO_LOG(DEBUG) << fmt::format("GetTableByName schema_id={} table_name={} table_definition={}", schema_id,
                                  table_name, table_definition.DebugString());

  return GetTable(schema_id, temp_table_id, table_definition);
}

// get index by name
butil::Status CoordinatorControl::GetIndexByName(uint64_t schema_id, const std::string& index_name,
                                                 pb::meta::IndexDefinitionWithId& index_definition) {
  DINGO_LOG(INFO) << fmt::format("GetIndexByName in control schema_id={} index_name={}", schema_id, index_name);

  if (schema_id < 0) {
    DINGO_LOG(ERROR) << "ERRROR: schema_id illegal " << schema_id;
    return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS, "schema_id illegal");
  }

  if (index_name.empty()) {
    DINGO_LOG(ERROR) << "ERRROR: index_name illegal " << index_name;
    return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS, "index_name illegal");
  }

  if (!ValidateSchema(schema_id)) {
    DINGO_LOG(ERROR) << "ERRROR: schema_id not valid" << schema_id;
    return butil::Status(pb::error::Errno::ESCHEMA_NOT_FOUND, "schema_id not valid");
  }

  uint64_t temp_index_id = 0;
  auto ret = index_name_map_safe_temp_.Get(std::to_string(schema_id) + index_name, temp_index_id);
  if (ret < 0) {
    DINGO_LOG(ERROR) << "ERRROR: index_name not found " << index_name;
    return butil::Status(pb::error::Errno::EINDEX_NOT_FOUND, "index_name not found");
  }

  DINGO_LOG(DEBUG) << fmt::format("GetIndexByName schema_id={} index_name={} index_definition={}", schema_id,
                                  index_name, index_definition.DebugString());

  return GetIndex(schema_id, temp_index_id, index_definition);
}

// get table range
butil::Status CoordinatorControl::GetTableRange(uint64_t schema_id, uint64_t table_id,
                                                pb::meta::TableRange& table_range) {
  if (schema_id < 0) {
    DINGO_LOG(ERROR) << "ERRROR: schema_id illegal " << schema_id;
    return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS, "schema_id illegal");
  }

  pb::coordinator_internal::TableInternal table_internal;
  if (!ValidateSchema(schema_id)) {
    DINGO_LOG(ERROR) << "ERRROR: schema_id not found" << schema_id;
    return butil::Status(pb::error::Errno::ESCHEMA_NOT_FOUND, "schema_id not found");
  }
  {
    // BAIDU_SCOPED_LOCK(table_map_mutex_);
    int ret = table_map_.Get(table_id, table_internal);
    if (ret < 0) {
      DINGO_LOG(ERROR) << "ERRROR: table_id not found" << table_id;
      return butil::Status(pb::error::Errno::ETABLE_NOT_FOUND, "table_id not found");
    }
  }

  for (int i = 0; i < table_internal.partitions_size(); i++) {
    auto* range_distribution = table_range.add_range_distribution();

    // range_distribution id
    uint64_t region_id = table_internal.partitions(i).region_id();

    auto* common_id_region = range_distribution->mutable_id();
    common_id_region->set_entity_id(region_id);
    common_id_region->set_parent_entity_id(table_id);
    common_id_region->set_entity_type(::dingodb::pb::meta::EntityType::ENTITY_TYPE_PART);

    // get region
    pb::common::Region part_region;
    int ret = region_map_.Get(region_id, part_region);
    if (ret < 0) {
      DINGO_LOG(ERROR) << fmt::format("ERROR cannot find region in regionmap_ while GetTable, table_id={} region_id={}",
                                      table_id, region_id);
      continue;
    }

    // range_distribution range
    auto* part_range = range_distribution->mutable_range();
    // part_range->CopyFrom(table_internal.partitions(i).range());
    part_range->CopyFrom(part_region.definition().range());

    // range_distribution leader location
    auto* leader_location = range_distribution->mutable_leader();

    // range_distribution voter & learner locations
    for (int j = 0; j < part_region.definition().peers_size(); j++) {
      const auto& part_peer = part_region.definition().peers(j);
      if (part_peer.store_id() == part_region.leader_store_id()) {
        leader_location->CopyFrom(part_peer.server_location());
      }

      if (part_peer.role() == ::dingodb::pb::common::PeerRole::VOTER) {
        auto* voter_location = range_distribution->add_voters();
        voter_location->CopyFrom(part_peer.server_location());
      } else if (part_peer.role() == ::dingodb::pb::common::PeerRole::LEARNER) {
        auto* learner_location = range_distribution->add_learners();
        learner_location->CopyFrom(part_peer.server_location());
      }
    }

    // range_distribution regionmap_epoch
    uint64_t region_map_epoch = GetPresentId(pb::coordinator_internal::IdEpochType::EPOCH_REGION);
    range_distribution->set_regionmap_epoch(region_map_epoch);

    // range_distribution storemap_epoch
    uint64_t store_map_epoch = GetPresentId(pb::coordinator_internal::IdEpochType::EPOCH_STORE);
    range_distribution->set_storemap_epoch(store_map_epoch);
  }

  return butil::Status::OK();
}

// get index range
butil::Status CoordinatorControl::GetIndexRange(uint64_t schema_id, uint64_t index_id,
                                                pb::meta::IndexRange& index_range) {
  if (schema_id < 0) {
    DINGO_LOG(ERROR) << "ERRROR: schema_id illegal " << schema_id;
    return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS, "schema_id illegal");
  }

  pb::coordinator_internal::IndexInternal index_internal;
  if (!ValidateSchema(schema_id)) {
    DINGO_LOG(ERROR) << "ERRROR: schema_id not found" << schema_id;
    return butil::Status(pb::error::Errno::ESCHEMA_NOT_FOUND, "schema_id not found");
  }
  {
    // BAIDU_SCOPED_LOCK(index_map_mutex_);
    int ret = index_map_.Get(index_id, index_internal);
    if (ret < 0) {
      DINGO_LOG(ERROR) << "ERRROR: index_id not found" << index_id;
      return butil::Status(pb::error::Errno::EINDEX_NOT_FOUND, "index_id not found");
    }
  }

  for (int i = 0; i < index_internal.partitions_size(); i++) {
    auto* range_distribution = index_range.add_range_distribution();

    // range_distribution id
    uint64_t region_id = index_internal.partitions(i).region_id();

    auto* common_id_region = range_distribution->mutable_id();
    common_id_region->set_entity_id(region_id);
    common_id_region->set_parent_entity_id(index_id);
    common_id_region->set_entity_type(::dingodb::pb::meta::EntityType::ENTITY_TYPE_PART);

    // get region
    pb::common::Region part_region;
    int ret = region_map_.Get(region_id, part_region);
    if (ret < 0) {
      DINGO_LOG(ERROR) << fmt::format("ERROR cannot find region in regionmap_ while GetIndex, index_id={} region_id={}",
                                      index_id, region_id);
      continue;
    }

    // range_distribution range
    auto* part_range = range_distribution->mutable_range();
    // part_range->CopyFrom(index_internal.partitions(i).range());
    part_range->CopyFrom(part_region.definition().range());

    // range_distribution leader location
    auto* leader_location = range_distribution->mutable_leader();

    // range_distribution voter & learner locations
    for (int j = 0; j < part_region.definition().peers_size(); j++) {
      const auto& part_peer = part_region.definition().peers(j);
      if (part_peer.store_id() == part_region.leader_store_id()) {
        leader_location->CopyFrom(part_peer.server_location());
      }

      if (part_peer.role() == ::dingodb::pb::common::PeerRole::VOTER) {
        auto* voter_location = range_distribution->add_voters();
        voter_location->CopyFrom(part_peer.server_location());
      } else if (part_peer.role() == ::dingodb::pb::common::PeerRole::LEARNER) {
        auto* learner_location = range_distribution->add_learners();
        learner_location->CopyFrom(part_peer.server_location());
      }
    }

    // range_distribution regionmap_epoch
    uint64_t region_map_epoch = GetPresentId(pb::coordinator_internal::IdEpochType::EPOCH_REGION);
    range_distribution->set_regionmap_epoch(region_map_epoch);

    // range_distribution storemap_epoch
    uint64_t store_map_epoch = GetPresentId(pb::coordinator_internal::IdEpochType::EPOCH_STORE);
    range_distribution->set_storemap_epoch(store_map_epoch);
  }

  return butil::Status::OK();
}

// get table metrics
butil::Status CoordinatorControl::GetTableMetrics(uint64_t schema_id, uint64_t table_id,
                                                  pb::meta::TableMetricsWithId& table_metrics) {
  if (schema_id < 0) {
    DINGO_LOG(ERROR) << "ERRROR: schema_id illegal " << schema_id;
    return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS, "schema_id illegal");
  }

  if (table_metrics.id().entity_id() != 0) {
    DINGO_LOG(ERROR) << "ERRROR: table is not empty , table_id=" << table_metrics.id().entity_id();
    return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS, "table is not empty");
  }

  if (!ValidateSchema(schema_id)) {
    DINGO_LOG(ERROR) << "ERRROR: schema_id not found" << schema_id;
    return butil::Status(pb::error::Errno::ESCHEMA_NOT_FOUND, "schema_id not found");
  }

  {
    // BAIDU_SCOPED_LOCK(table_map_mutex_);
    if (!table_map_.Exists(table_id)) {
      DINGO_LOG(ERROR) << "ERRROR: table_id not found" << table_id;
      return butil::Status(pb::error::Errno::ETABLE_NOT_FOUND, "table_id not found");
    }
  }

  pb::coordinator_internal::TableMetricsInternal table_metrics_internal;
  {
    // BAIDU_SCOPED_LOCK(table_metrics_map_mutex_);
    int ret = table_metrics_map_.Get(table_id, table_metrics_internal);

    // auto* temp_table_metrics = table_metrics_map_.seek(table_id);
    if (ret < 0) {
      DINGO_LOG(INFO) << "table_metrics not found, try to calculate new one" << table_id;

      // calculate table metrics using region metrics
      auto* table_metrics_single = table_metrics_internal.mutable_table_metrics();
      if (CalculateTableMetricsSingle(table_id, *table_metrics_single) < 0) {
        DINGO_LOG(ERROR) << "ERRROR: CalculateTableMetricsSingle failed" << table_id;
        return butil::Status(pb::error::Errno::ETABLE_METRICS_FAILED, "CalculateTableMetricsSingle failed");
      } else {
        table_metrics_internal.set_id(table_id);
        table_metrics_internal.mutable_table_metrics()->CopyFrom(*table_metrics_single);
        // table_metrics_map_[table_id] = table_metrics_internal;
        // temp_table_metrics->CopyFrom(table_metrics_internal);
        table_metrics_map_.Put(table_id, table_metrics_internal);

        DINGO_LOG(INFO) << fmt::format(
            "table_metrics first calculated, table_id={} row_count={} min_key={} max_key={} part_count={}", table_id,
            table_metrics_single->rows_count(), table_metrics_single->min_key(), table_metrics_single->max_key(),
            table_metrics_single->part_count());
      }
    } else {
      // construct TableMetrics from table_metrics_internal
      DINGO_LOG(DEBUG) << "table_metrics found, return metrics in map" << table_id;
      // table_metrics_internal = *temp_table_metrics;
    }
  }

  // construct TableMetricsWithId
  auto* common_id_table = table_metrics.mutable_id();
  common_id_table->set_entity_id(table_id);
  common_id_table->set_parent_entity_id(schema_id);
  common_id_table->set_entity_type(::dingodb::pb::meta::EntityType::ENTITY_TYPE_TABLE);

  table_metrics.mutable_table_metrics()->CopyFrom(table_metrics_internal.table_metrics());

  return butil::Status::OK();
}

// get index metrics
butil::Status CoordinatorControl::GetIndexMetrics(uint64_t schema_id, uint64_t index_id,
                                                  pb::meta::IndexMetricsWithId& index_metrics) {
  if (schema_id < 0) {
    DINGO_LOG(ERROR) << "ERRROR: schema_id illegal " << schema_id;
    return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS, "schema_id illegal");
  }

  if (index_metrics.id().entity_id() != 0) {
    DINGO_LOG(ERROR) << "ERRROR: index is not empty , index_id=" << index_metrics.id().entity_id();
    return butil::Status(pb::error::Errno::EILLEGAL_PARAMTETERS, "index is not empty");
  }

  if (!ValidateSchema(schema_id)) {
    DINGO_LOG(ERROR) << "ERRROR: schema_id not found" << schema_id;
    return butil::Status(pb::error::Errno::ESCHEMA_NOT_FOUND, "schema_id not found");
  }

  {
    // BAIDU_SCOPED_LOCK(index_map_mutex_);
    if (!index_map_.Exists(index_id)) {
      DINGO_LOG(ERROR) << "ERRROR: index_id not found" << index_id;
      return butil::Status(pb::error::Errno::EINDEX_NOT_FOUND, "index_id not found");
    }
  }

  pb::coordinator_internal::IndexMetricsInternal index_metrics_internal;
  {
    // BAIDU_SCOPED_LOCK(index_metrics_map_mutex_);
    int ret = index_metrics_map_.Get(index_id, index_metrics_internal);

    // auto* temp_index_metrics = index_metrics_map_.seek(index_id);
    if (ret < 0) {
      DINGO_LOG(INFO) << "index_metrics not found, try to calculate new one" << index_id;

      // calculate index metrics using region metrics
      auto* index_metrics_single = index_metrics_internal.mutable_index_metrics();
      if (CalculateIndexMetricsSingle(index_id, *index_metrics_single) < 0) {
        DINGO_LOG(ERROR) << "ERRROR: CalculateIndexMetricsSingle failed" << index_id;
        return butil::Status(pb::error::Errno::EINDEX_METRICS_FAILED, "CalculateIndexMetricsSingle failed");
      } else {
        index_metrics_internal.set_id(index_id);
        index_metrics_internal.mutable_index_metrics()->CopyFrom(*index_metrics_single);
        // index_metrics_map_[index_id] = index_metrics_internal;
        // temp_index_metrics->CopyFrom(index_metrics_internal);
        index_metrics_map_.Put(index_id, index_metrics_internal);

        DINGO_LOG(INFO) << fmt::format(
            "index_metrics first calculated, index_id={} row_count={} min_key={} max_key={} part_count={}", index_id,
            index_metrics_single->rows_count(), index_metrics_single->min_key(), index_metrics_single->max_key(),
            index_metrics_single->part_count());
      }
    } else {
      // construct IndexMetrics from index_metrics_internal
      DINGO_LOG(DEBUG) << "index_metrics found, return metrics in map" << index_id;
      // index_metrics_internal = *temp_index_metrics;
    }
  }

  // construct IndexMetricsWithId
  auto* common_id_index = index_metrics.mutable_id();
  common_id_index->set_entity_id(index_id);
  common_id_index->set_parent_entity_id(schema_id);
  common_id_index->set_entity_type(::dingodb::pb::meta::EntityType::ENTITY_TYPE_INDEX);

  index_metrics.mutable_index_metrics()->CopyFrom(index_metrics_internal.index_metrics());

  return butil::Status::OK();
}

// CalculateTableMetricsSingle
uint64_t CoordinatorControl::CalculateTableMetricsSingle(uint64_t table_id, pb::meta::TableMetrics& table_metrics) {
  pb::coordinator_internal::TableInternal table_internal;
  {
    // BAIDU_SCOPED_LOCK(table_map_mutex_);
    int ret = table_map_.Get(table_id, table_internal);
    if (ret < 0) {
      DINGO_LOG(ERROR) << "ERRROR: table_id not found" << table_id;
      return -1;
    }
  }

  // build result metrics
  uint64_t row_count = 0;
  std::string min_key(10, '\x00');
  std::string max_key(10, '\xFF');

  for (int i = 0; i < table_internal.partitions_size(); i++) {
    // part id
    uint64_t region_id = table_internal.partitions(i).region_id();

    // get region
    pb::common::Region part_region;
    {
      // BAIDU_SCOPED_LOCK(region_map_mutex_);
      int ret = region_map_.Get(region_id, part_region);
      if (ret < 0) {
        DINGO_LOG(ERROR) << fmt::format(
            "ERROR cannot find region in regionmap_ while GetTable, table_id={} region_id={}", table_id, region_id);
        continue;
      }
    }

    if (!part_region.has_metrics()) {
      DINGO_LOG(ERROR) << fmt::format("ERROR region has no metrics, table_id={} region_id={}", table_id, region_id);
      continue;
    }

    const auto& region_metrics = part_region.metrics();
    row_count += region_metrics.row_count();

    if (min_key.empty()) {
      min_key = region_metrics.min_key();
    } else {
      if (min_key.compare(region_metrics.min_key()) > 0) {
        min_key = region_metrics.min_key();
      }
    }

    if (max_key.empty()) {
      max_key = region_metrics.max_key();
    } else {
      if (max_key.compare(region_metrics.max_key()) < 0) {
        max_key = region_metrics.max_key();
      }
    }
  }

  table_metrics.set_rows_count(row_count);
  table_metrics.set_min_key(min_key);
  table_metrics.set_max_key(max_key);
  table_metrics.set_part_count(table_internal.partitions_size());

  DINGO_LOG(DEBUG) << fmt::format(
      "table_metrics calculated in CalculateTableMetricsSingle, table_id={} row_count={} min_key={} max_key={} "
      "part_count={}",
      table_id, table_metrics.rows_count(), table_metrics.min_key(), table_metrics.max_key(),
      table_metrics.part_count());

  return 0;
}

// CalculateIndexMetricsSingle
uint64_t CoordinatorControl::CalculateIndexMetricsSingle(uint64_t index_id, pb::meta::IndexMetrics& index_metrics) {
  pb::coordinator_internal::IndexInternal index_internal;
  {
    // BAIDU_SCOPED_LOCK(index_map_mutex_);
    int ret = index_map_.Get(index_id, index_internal);
    if (ret < 0) {
      DINGO_LOG(ERROR) << "ERRROR: index_id not found" << index_id;
      return -1;
    }
  }

  // build result metrics
  uint64_t row_count = 0;
  std::string min_key(10, '\x00');
  std::string max_key(10, '\xFF');

  for (int i = 0; i < index_internal.partitions_size(); i++) {
    // part id
    uint64_t region_id = index_internal.partitions(i).region_id();

    // get region
    pb::common::Region part_region;
    {
      // BAIDU_SCOPED_LOCK(region_map_mutex_);
      int ret = region_map_.Get(region_id, part_region);
      if (ret < 0) {
        DINGO_LOG(ERROR) << fmt::format(
            "ERROR cannot find region in regionmap_ while GetIndex, index_id={} region_id={}", index_id, region_id);
        continue;
      }
    }

    if (!part_region.has_metrics()) {
      DINGO_LOG(ERROR) << fmt::format("ERROR region has no metrics, index_id={} region_id={}", index_id, region_id);
      continue;
    }

    const auto& region_metrics = part_region.metrics();
    row_count += region_metrics.row_count();

    if (min_key.empty()) {
      min_key = region_metrics.min_key();
    } else {
      if (min_key.compare(region_metrics.min_key()) > 0) {
        min_key = region_metrics.min_key();
      }
    }

    if (max_key.empty()) {
      max_key = region_metrics.max_key();
    } else {
      if (max_key.compare(region_metrics.max_key()) < 0) {
        max_key = region_metrics.max_key();
      }
    }
  }

  index_metrics.set_rows_count(row_count);
  index_metrics.set_min_key(min_key);
  index_metrics.set_max_key(max_key);
  index_metrics.set_part_count(index_internal.partitions_size());

  DINGO_LOG(DEBUG) << fmt::format(
      "index_metrics calculated in CalculateIndexMetricsSingle, index_id={} row_count={} min_key={} max_key={} "
      "part_count={}",
      index_id, index_metrics.rows_count(), index_metrics.min_key(), index_metrics.max_key(),
      index_metrics.part_count());

  return 0;
}

// CalculateTableMetrics
// calculate table metrics using region metrics
// only recalculate when table_metrics_map_ does contain table_id
// if the table_id is not in table_map_, remove it from table_metrics_map_
void CoordinatorControl::CalculateTableMetrics() {
  // BAIDU_SCOPED_LOCK(table_metrics_map_mutex_);

  butil::FlatMap<uint64_t, pb::coordinator_internal::TableMetricsInternal> temp_table_metrics_map;
  temp_table_metrics_map.init(10000);
  table_metrics_map_.GetFlatMapCopy(temp_table_metrics_map);

  for (auto& table_metrics_internal : temp_table_metrics_map) {
    uint64_t table_id = table_metrics_internal.first;
    pb::meta::TableMetrics table_metrics;
    if (CalculateTableMetricsSingle(table_id, table_metrics) < 0) {
      DINGO_LOG(ERROR) << "ERRROR: CalculateTableMetricsSingle failed, remove metrics from map" << table_id;
      table_metrics_map_.Erase(table_id);

      // mbvar table
      coordinator_bvar_metrics_table_.DeleteTableBvar(table_id);

    } else {
      table_metrics_internal.second.mutable_table_metrics()->CopyFrom(table_metrics);

      // update table_metrics_map_ in memory
      table_metrics_map_.PutIfExists(table_id, table_metrics_internal.second);

      // mbvar table
      coordinator_bvar_metrics_table_.UpdateTableBvar(table_id, table_metrics.rows_count(), table_metrics.part_count());
    }
  }
}

// CalculateIndexMetrics
// calculate index metrics using region metrics
// only recalculate when index_metrics_map_ does contain index_id
// if the index_id is not in index_map_, remove it from index_metrics_map_
void CoordinatorControl::CalculateIndexMetrics() {
  // BAIDU_SCOPED_LOCK(index_metrics_map_mutex_);

  butil::FlatMap<uint64_t, pb::coordinator_internal::IndexMetricsInternal> temp_index_metrics_map;
  temp_index_metrics_map.init(10000);
  index_metrics_map_.GetFlatMapCopy(temp_index_metrics_map);

  for (auto& index_metrics_internal : temp_index_metrics_map) {
    uint64_t index_id = index_metrics_internal.first;
    pb::meta::IndexMetrics index_metrics;
    if (CalculateIndexMetricsSingle(index_id, index_metrics) < 0) {
      DINGO_LOG(ERROR) << "ERRROR: CalculateIndexMetricsSingle failed, remove metrics from map" << index_id;
      index_metrics_map_.Erase(index_id);

      // mbvar index
      coordinator_bvar_metrics_index_.DeleteIndexBvar(index_id);

    } else {
      index_metrics_internal.second.mutable_index_metrics()->CopyFrom(index_metrics);

      // update index_metrics_map_ in memory
      index_metrics_map_.PutIfExists(index_id, index_metrics_internal.second);

      // mbvar index
      coordinator_bvar_metrics_index_.UpdateIndexBvar(index_id, index_metrics.rows_count(), index_metrics.part_count());
    }
  }
}

}  // namespace dingodb
