#include "sqlite_dbengine.h"
#include "string_helper.h"
#include "typedef.h"
#include <fstream>

bool SQLiteDBEngine::Execute(
    const std::string& query) {

    return true;
}

bool SQLiteDBEngine::Select(
    const std::string& query, 
    nlohmann::json& result) {

    return true;
}


SQLiteDBEngine::SQLiteDBEngine(
  std::shared_ptr<ISQLiteFactory> sqlite_factory,
  const std::string& path, 
  const std::string& table_statement_creation) 
  : m_sqlite_factory(sqlite_factory){

    Initialize(path, table_statement_creation);
}

SQLiteDBEngine::~SQLiteDBEngine() {
}


void SQLiteDBEngine::Initialize(
  const std::string& path, 
  const std::string& table_statement_creation) {

 
  if (CleanDB(path)) {
    m_sqlite_connection = m_sqlite_factory->CreateConnection(path);

    const auto create_db_querys_list { StringHelper::split(table_statement_creation,';')};

    m_sqlite_connection->Execute("PRAGMA temp_store = memory;");
    m_sqlite_connection->Execute("PRAGMA synchronous = OFF;");

    for (const auto& query : create_db_querys_list) {
      auto stmt { m_sqlite_factory->CreateStatement(m_sqlite_connection, query) }; 
      stmt->Step();
    }
  }
}

bool SQLiteDBEngine::CleanDB(const std::string& path) {
  auto ret_val { true };
  if (path.compare(":memory") != 0) {
    if (std::ifstream(path)){
      if (0 != std::remove(path.c_str())) {
        ret_val = false;
      }
    }
  }
  return ret_val;
}

bool SQLiteDBEngine::BulkInsert(const std::string& table, const nlohmann::json& data) {
  auto ret_val{ false };
  if (0 != LoadTableData(table)) {
    const auto sql { BuildInsertBulkDataSqlQuery(table) };

    if(!sql.empty()) {
      
      auto transaction { m_sqlite_factory->CreateTransaction(m_sqlite_connection) };
      auto stmt { m_sqlite_factory->CreateStatement(m_sqlite_connection, sql) };
      for (const auto& json_value : data){
        for (const auto& value : m_table_fields[table]) {
          BindJsonData(stmt, value, json_value);
        }
        stmt->Step();
        stmt->Reset();
      }
      ret_val = transaction->Commit();
    }
  }
  return ret_val;
}

size_t SQLiteDBEngine::LoadTableData(const std::string& table) {
  size_t fields_quantity {0ull};
  if (0 == m_table_fields[table].size()) {
    if (LoadFieldData(table)) {
      fields_quantity = m_table_fields[table].size();
    }
  } else {
    fields_quantity = m_table_fields[table].size();
  }
  return fields_quantity;    
}



std::string SQLiteDBEngine::BuildInsertBulkDataSqlQuery(const std::string& table) {
  std::string sql = "INSERT INTO ";
  sql.append(table);
  sql.append(" VALUES (");

  if (0 != m_table_fields[table].size()) {
    for (size_t i = 0; i < m_table_fields[table].size();++i) {
      sql.append("?,");
    }
    sql = sql.substr(0, sql.size()-1);
    sql.append(");");
  } else {
    sql.clear();
  }
  return sql;
}

bool SQLiteDBEngine::LoadFieldData(const std::string& table) {
  auto ret_val { false };
  const std::string sql {"PRAGMA table_info("+table+");"}; 
  
  if (!table.empty()) {
    auto stmt { m_sqlite_factory->CreateStatement(m_sqlite_connection, sql) };
    while (SQLITE_ROW == stmt->Step()) {
      m_table_fields[table].push_back(std::make_tuple(
            stmt->GetColumn(0)->Int(), 
            stmt->GetColumn(1)->String(), 
            ColumnTypeName(stmt->GetColumn(2)->String()), 
            1 == stmt->GetColumn(5)->Int() ? true : false));
    }
    ret_val = true;
  }
  return ret_val;
}


ColumnType SQLiteDBEngine::ColumnTypeName(const std::string& type) {
  for (const auto& col : kColumnTypeNames) {
    if (col.second == type) {
      return col.first;
    }
  }
  return UNKNOWN_TYPE;
}

bool SQLiteDBEngine::BindJsonData(std::unique_ptr<SQLite::IStatement>& stmt, const ColumnData& cd, const nlohmann::json::value_type& value_type){
  const auto type = std::get<TableHeader::TYPE>(cd);
  const auto cid = std::get<TableHeader::CID>(cd) + 1;
  const auto name = std::get<TableHeader::NAME>(cd);

  auto ret_val{ false };
  
  if (ColumnType::BIGINT_TYPE == type) {
    int64_t value = value_type[name].is_number() ? value_type[name].get<int64_t>() : 0;
    ret_val = stmt->Bind(cid, value);
  } else if (ColumnType::UNSIGNED_BIGINT_TYPE == type) {
    uint64_t value = value_type[name].is_number_unsigned() ? value_type[name].get<uint64_t>() : 0;
    ret_val = stmt->Bind(cid, value);
  } else if (ColumnType::INTEGER_TYPE == type) {
    int32_t value = value_type[name].is_number() ? value_type[name].get<int32_t>() : 0;
    ret_val = stmt->Bind(cid, value);
  } else if (ColumnType::TEXT_TYPE == type) {
    std::string value = value_type[name].is_string() ? value_type[name].get<std::string>() : "";
    ret_val = stmt->Bind(cid, value);
  } else if (ColumnType::DOUBLE_TYPE == type) {
    double value = value_type[name].is_number_float() ? value_type[name].get<double>() : .0f;
    ret_val = stmt->Bind(cid, value);
  } else if (ColumnType::BLOB_TYPE == type) {
    std::cout << "not implemented "<< __LINE__ << " - " << __FILE__ << std::endl;
  }

  return ret_val;
}

bool SQLiteDBEngine::RefreshTablaData(const nlohmann::json& data, const std::tuple<nlohmann::json&, void *> delta) {
  auto ret_val {false};
  const std::string table { data["table"].is_string() ? data["table"].get<std::string>() : "" };
  if (CreateCopyTempTable(table)) {
    if (BulkInsert(table + kTempTableSubFix, data["data"])) {
      std::vector<std::string> primary_key_list;
      if (GetPrimaryKeysFromTable(table, primary_key_list)) {
        if (!RemoveNotExistsRows(table, primary_key_list, delta)) {
          std::cout << "Error during the delete rows update "<< __LINE__ << " - " << __FILE__ << std::endl;
        }
        if (!ChangeModifiedRows(table, primary_key_list, delta)) {
          std::cout << "Error during the change of modified rows " << __LINE__ << " - " << __FILE__ << std::endl;
        }
        if (!InsertNewRows(table, primary_key_list, delta)) {
          std::cout << "Error during the insert rows update "<< __LINE__ << " - " << __FILE__ << std::endl;
        }
        
      }
      ret_val = true;
    }
    DeleteTempTable(table);
  }
  return ret_val;
}

bool SQLiteDBEngine::CreateCopyTempTable(const std::string& table) {
  auto ret_val { false };
  std::string result_query;
  if (GetTableCreateQuery(table, result_query)) {
    if(StringHelper::replace_string(result_query, "CREATE TABLE " + table, "CREATE TEMP TABLE " + table + "_TEMP")) {
      auto stmt { m_sqlite_factory->CreateStatement(m_sqlite_connection, result_query) };
      if (SQLITE_DONE == stmt->Step()) {
        ret_val = true;
      }
    }
  }
  return ret_val;
}

void SQLiteDBEngine::DeleteTempTable(const std::string& table) { 
  m_sqlite_connection->Execute("DROP TABLE " + table + kTempTableSubFix + ";");
}

bool SQLiteDBEngine::GetTableCreateQuery(
  const std::string& table, 
  std::string& result_query) {

  auto ret_val { false };
  const std::string sql {"SELECT sql FROM sqlite_master WHERE type='table' AND name=?;"}; 

  if (!table.empty()) {
    auto stmt { m_sqlite_factory->CreateStatement(m_sqlite_connection, sql) };
    stmt->Bind(1, table);
    while (SQLITE_ROW == stmt->Step()) {
      result_query.append(std::move(stmt->GetColumn(0)->String()));
      result_query.append(";");
      ret_val = true;
    }
  }
  return ret_val;
}

bool SQLiteDBEngine::RemoveNotExistsRows(
  const std::string& table, 
  const std::vector<std::string>& primary_key_list, 
  const std::tuple<nlohmann::json&, void *> delta) {

  auto ret_val { true };
  std::vector<Row> row_keys_value;
  if (GetPKListLeftOnly(table, table+kTempTableSubFix, primary_key_list, row_keys_value)) {
    if (DeleteRows(table, primary_key_list, row_keys_value)) {
      for (const auto& row : row_keys_value){
        nlohmann::json object;
        for (const auto& value : row) {
          if(!GetFieldValueFromTuple(value, object)) {
            std::cout << "not implemented "<< __LINE__ << " - " << __FILE__ << std::endl;
          }
        }
        auto callback { std::get<ResponseType::RT_CALLBACK>(delta) };
        if (nullptr != callback) {
          result_callback Notify = reinterpret_cast<result_callback>(callback);
          cJSON* json_result { cJSON_Parse(object.dump().c_str()) };
          Notify(ReturnTypeCallback::DELETED, json_result);
          cJSON_Delete(json_result);
        } else {
          std::get<ResponseType::RT_JSON>(delta)["deleted"].push_back(std::move(object));
        }
      }
    } else {
      ret_val = false;
    }
  }
  return ret_val;
}

bool SQLiteDBEngine::GetPrimaryKeysFromTable(
  const std::string& table, 
  std::vector<std::string>& primary_key_list) {
    
  for(const auto& value : m_table_fields[table]) {
    if (std::get<TableHeader::PK>(value) == true) {
      primary_key_list.push_back(std::get<TableHeader::NAME>(value));
    }
  }
  return m_table_fields.find(table) != m_table_fields.end();
}

int32_t SQLiteDBEngine::GetTableData(
  std::unique_ptr<SQLite::IStatement>& stmt, 
  const int32_t index, 
  const ColumnType& type, 
  const std::string& field_name, 
  Row& row) {
 
  int32_t rc { SQLITE_OK };
 
  if (ColumnType::BIGINT_TYPE == type) {
    row[field_name] = std::make_tuple(type,std::string(),0,stmt->GetColumn(index)->Int64(),0,0);
  } else if (ColumnType::UNSIGNED_BIGINT_TYPE == type) {
    row[field_name] = std::make_tuple(type,std::string(),0,0,stmt->GetColumn(index)->Int64(),0);                        
  } else if (ColumnType::INTEGER_TYPE == type) {
    row[field_name] = std::make_tuple(type,std::string(),stmt->GetColumn(index)->Int(),0,0,0); 
  } else if (ColumnType::TEXT_TYPE == type) {
    row[field_name] = std::make_tuple(type,stmt->GetColumn(index)->String(),0,0,0,0); 
  } else if (ColumnType::DOUBLE_TYPE == type) {
    row[field_name] = std::make_tuple(type,std::string(),0,0,0,stmt->GetColumn(index)->Double()); 
  } else if (ColumnType::BLOB_TYPE == type) {
    std::cout << "not implemented "<< __LINE__ << " - " << __FILE__ << std::endl;
  } else {
    rc = SQLITE_ERROR;
  }

  return rc;
}

bool SQLiteDBEngine::GetLeftOnly(
  const std::string& t1, 
  const std::string& t2, 
  const std::vector<std::string>& primary_key_list, 
  std::vector<Row>& return_rows) {

  auto ret_val { false };
  
  const std::string query { BuildLeftOnlyQuery(t1, t2, primary_key_list) };

  if (!t1.empty() && !query.empty()) {
    auto stmt { m_sqlite_factory->CreateStatement(m_sqlite_connection, query) };
    const auto table_fields { m_table_fields[t1] };
    while (SQLITE_ROW == stmt->Step()) {
      Row register_fields;
      for(const auto& field : table_fields) {
        GetTableData(
          stmt, 
          std::get<TableHeader::CID>(field), 
          std::get<TableHeader::TYPE>(field),
          std::get<TableHeader::NAME>(field), 
          register_fields);
      }
      return_rows.push_back(std::move(register_fields));
    }
    ret_val = true;
  } 
  return ret_val;
}

bool SQLiteDBEngine::GetPKListLeftOnly(
  const std::string& t1, 
  const std::string& t2, 
  const std::vector<std::string>& primary_key_list, 
  std::vector<Row>& return_rows) {

  auto ret_val { false };
  
  const std::string sql { BuildLeftOnlyQuery(t1, t2, primary_key_list, true) };
  std::cout << sql << std::endl;
  if (!t1.empty() && !sql.empty()) {
    auto stmt { m_sqlite_factory->CreateStatement(m_sqlite_connection, sql) };
    const auto table_fields { m_table_fields[t1] };
    while (SQLITE_ROW == stmt->Step()) {
      Row register_fields;
      for(const auto& value_pk : primary_key_list) {
        auto index { 0ull };
        const auto& it = std::find_if(
          table_fields.begin(), 
          table_fields.end(), 
          [&value_pk](const ColumnData& column_data) {
            return std::get<TableHeader::NAME>(column_data) == value_pk;
        });

        if (table_fields.end() != it) { 
          GetTableData(
          stmt, 
          index, 
          std::get<TableHeader::TYPE>(*it),
          std::get<TableHeader::NAME>(*it), 
          register_fields);
        }
        ++index;
      }
      return_rows.push_back(std::move(register_fields));
    }
    ret_val = true;
  } 

  return ret_val;
}

std::string SQLiteDBEngine::BuildDeleteBulkDataSqlQuery(
  const std::string& table, 
  const std::vector<std::string>& primary_key_list) {

    std::string sql = "DELETE FROM ";
    sql.append(table);
    sql.append(" WHERE ");

    if (0 != primary_key_list.size()) {
      for (const auto& value : primary_key_list) {
        sql.append(value);
        sql.append("=? AND ");
      }
      sql = sql.substr(0, sql.size()-5);
      sql.append(";");
    } else {
      sql.clear();
    }
    return sql;
}

bool SQLiteDBEngine::DeleteRows(
  const std::string& table, 
  const std::vector<std::string>& primary_key_list,
  const std::vector<Row>& rows_to_remove) {

  auto ret_val { false };
  const auto sql { BuildDeleteBulkDataSqlQuery(table, primary_key_list) };

  if(!sql.empty()) {
    auto transaction { m_sqlite_factory->CreateTransaction(m_sqlite_connection)};
    auto stmt { m_sqlite_factory->CreateStatement(m_sqlite_connection, sql) };

    for (const auto& row : rows_to_remove){
      auto index {1l};
      for (const auto& value : primary_key_list) {
        if (!BindFieldData(stmt, index, row.at(value))) {
          std::cout << "bind error: " <<  index << std::endl;
        }
        ++index;
      }
      stmt->Step();
      stmt->Reset();
    }
    ret_val = transaction->Commit();
  }
  return ret_val;
}


int32_t SQLiteDBEngine::BindFieldData(
  std::unique_ptr<SQLite::IStatement>& stmt, 
  const int32_t index, 
  const TableField& field_data){

  int32_t rc = SQLITE_ERROR;

  const auto type = std::get<GenericTupleIndex::GEN_TYPE>(field_data);
  
  if (ColumnType::BIGINT_TYPE == type) {
    const auto value { std::get<GenericTupleIndex::GEN_BIGINT>(field_data) };
    rc = stmt->Bind(index, value);
  } else if (ColumnType::UNSIGNED_BIGINT_TYPE == type) {
    const auto value { std::get<GenericTupleIndex::GEN_UNSIGNED_BIGINT>(field_data) };
    rc = stmt->Bind(index, value);
  } else if (ColumnType::INTEGER_TYPE == type) {
    const auto value { std::get<GenericTupleIndex::GEN_INTEGER>(field_data) };
    rc = stmt->Bind(index, value);
  } else if (ColumnType::TEXT_TYPE == type) {
    const auto value { std::get<GenericTupleIndex::GEN_STRING>(field_data) };
    rc = stmt->Bind(index, value);
  } else if (ColumnType::DOUBLE_TYPE == type) {
    const auto value { std::get<GenericTupleIndex::GEN_DOUBLE>(field_data) };
    rc = stmt->Bind(index, value);

  } else if (ColumnType::BLOB_TYPE == type) {
    std::cout << "not implemented "<< __LINE__ << " - " << __FILE__ << std::endl;
  }

  return rc;
}


std::string SQLiteDBEngine::BuildLeftOnlyQuery(
  const std::string& t1,
  const std::string& t2,
  const std::vector<std::string>& primary_key_list,
  const bool return_only_pk_fields) {

  std::string return_fields_list;
  std::string on_match_list;
  std::string null_filter_list;
  
  for (const auto& value : primary_key_list) {
    if (return_only_pk_fields)
      return_fields_list.append("t1."+value+",");
    on_match_list.append("t1." + value + "= t2." + value + " AND ");
    null_filter_list.append("t2."+value+ " IS NULL AND ");
  }

  if (return_only_pk_fields)
    return_fields_list = return_fields_list.substr(0, return_fields_list.size()-1);
  else
    return_fields_list.append("*");
  on_match_list = on_match_list.substr(0, on_match_list.size()-5);
  null_filter_list = null_filter_list.substr(0, null_filter_list.size()-5);


  return std::string("SELECT "+return_fields_list+" FROM "+t1+" t1 LEFT JOIN "+t2+" t2 ON "+on_match_list+" WHERE "+null_filter_list+";");
} 


bool SQLiteDBEngine::InsertNewRows(
  const std::string& table, 
  const std::vector<std::string>& primary_key_list, 
  const std::tuple<nlohmann::json&, void *> delta) {

  auto ret_val { true };
  std::vector<Row> row_values;
  if (GetLeftOnly(table+kTempTableSubFix, table, primary_key_list, row_values)) {
     if (BulkInsert(table, row_values)) {
       for (const auto& row : row_values){
        nlohmann::json object;
        for (const auto& value : row) {
          if(!GetFieldValueFromTuple(value, object)) {
            std::cout << "not implemented "<< __LINE__ << " - " << __FILE__ << std::endl;
          }
        }
        auto callback { std::get<ResponseType::RT_CALLBACK>(delta) };
        if (nullptr != callback) {
          result_callback Notify = reinterpret_cast<result_callback>(callback);
          cJSON* json_result { cJSON_Parse(object.dump().c_str()) };
          Notify(ReturnTypeCallback::INSERTED, json_result);
          cJSON_Delete(json_result);
        } else {
          std::get<ResponseType::RT_JSON>(delta)["inserted"].push_back(std::move(object));
        }
      }
    } 
    else {
      ret_val = false;
    }
  }
  return ret_val;
}

bool SQLiteDBEngine::BulkInsert(
  const std::string& table, 
  const std::vector<Row>& data) {

  auto ret_val{ false };
  
  const auto sql { BuildInsertBulkDataSqlQuery(table) };

  if(!sql.empty()) {

    auto transaction { m_sqlite_factory->CreateTransaction(m_sqlite_connection)};
    auto stmt { m_sqlite_factory->CreateStatement(m_sqlite_connection, sql) };

    for (const auto& row : data){
      for (const auto& value : m_table_fields[table]) {
        auto it { row.find(std::get<TableHeader::NAME>(value))};
        if (row.end() != it){
          if (!BindFieldData(stmt, std::get<TableHeader::CID>(value) + 1, (*it).second )) {
            std::cout << "bind error: " <<  std::get<TableHeader::CID>(value) << std::endl;
          }
        }
      }

      stmt->Step();
      stmt->Reset();
    }
    ret_val = transaction->Commit();
  }
  return ret_val;
}

int SQLiteDBEngine::ChangeModifiedRows(
  const std::string& table, 
  const std::vector<std::string>& primary_key_list, 
  const std::tuple<nlohmann::json&, void *> delta) {

  auto ret_val { true };
  std::vector<Row> row_keys_value;
  if (GetRowsToModify(table,primary_key_list,row_keys_value)) {
    if (UpdateRows(table, primary_key_list, row_keys_value)) {
      for (const auto& row : row_keys_value){
        nlohmann::json object;
        for (const auto& value : row) {
          if(!GetFieldValueFromTuple(value, object)) {
            std::cout << "not implemented "<< __LINE__ << " - " << __FILE__ << std::endl;
          }
        }
        auto callback { std::get<ResponseType::RT_CALLBACK>(delta) };
        if (nullptr != callback) {
          result_callback Notify = reinterpret_cast<result_callback>(callback);
          cJSON* json_result { cJSON_Parse(object.dump().c_str()) };
          Notify(ReturnTypeCallback::MODIFIED, json_result);
          cJSON_Delete(json_result);
        } else {
          std::get<ResponseType::RT_JSON>(delta)["modified"].push_back(std::move(object));
        }
      }
    } else {
      ret_val = false;
    }
  }
  return ret_val;
}

std::string SQLiteDBEngine::BuildUpdateDataSqlQuery(
  const std::string& table, 
  const std::vector<std::string>& primary_key_list,
  const Row& row,
  const std::pair<const std::__cxx11::string, TableField> &field) {

  std::string sql = "UPDATE ";
  sql.append(table);
  sql.append(" SET ");
  sql.append(field.first);
  sql.append("=");
  if (GetFieldValueFromTuple(field, sql, true))
  {
    sql.append(" WHERE ");

    if (0 != primary_key_list.size()) {
      for (const auto& value : primary_key_list) {

        const auto it_pk_value { row.find("PK_"+value) };
        if (it_pk_value != row.end())
        {
          sql.append(value);
          sql.append("=");  
          if (!GetFieldValueFromTuple((*it_pk_value), sql, true))
          {
            sql.clear();
            break;
          }
        } else {
          sql.clear();
          break;
        }
        sql.append(" AND ");
      }
      sql = sql.substr(0, sql.length()-5);
      if (sql.length() > 0) {
        sql.append(";");
      }
    } else {
      sql.clear();
    }
  } else {
    sql.clear();
  }
  
  return sql;
}

std::string SQLiteDBEngine::BuildModifiedRowsQuery(
  const std::string& t1,
  const std::string& t2,
  const std::vector<std::string>& primary_key_list) {

  std::string return_fields_list;
  std::string on_match_list;
  std::string null_filter_list;
  
  for (const auto& value : primary_key_list) {
    return_fields_list.append("t1."+value+",");
    on_match_list.append("t1." + value + "=t2." + value + " AND ");
  }

  for (const auto& value : m_table_fields[t1]) {
    const auto field_name {std::get<TableHeader::NAME>(value)};
    return_fields_list.append("CASE WHEN t1.");
    return_fields_list.append(field_name);
    return_fields_list.append("<>t2.");
    return_fields_list.append(field_name);
    return_fields_list.append(" THEN t1.");
    return_fields_list.append(field_name);
    return_fields_list.append(" ELSE NULL END AS DIF_");
    return_fields_list.append(field_name);
    return_fields_list.append(",");
  }

  return_fields_list = return_fields_list.substr(0, return_fields_list.size()-1);
  on_match_list = on_match_list.substr(0, on_match_list.size()-5);
  std::string ret_val {"SELECT "};
  ret_val.append(return_fields_list);
  ret_val.append(" FROM (select *,'");
  ret_val.append(t1);
  ret_val.append("' as val from ");
  ret_val.append(t1);
  ret_val.append(" UNION ALL select *,'");
  ret_val.append(t2);
  ret_val.append("' as val from ");
  ret_val.append(t2);
  ret_val.append(") t1 INNER JOIN ");
  ret_val.append(t1);
  ret_val.append(" t2 ON ");
  ret_val.append(on_match_list);
  ret_val.append(" WHERE t1.val = '");
  ret_val.append(t2);
  ret_val.append("';");
  
  return ret_val;
} 


bool SQLiteDBEngine::GetRowsToModify(
  const std::string& table, 
  const std::vector<std::string>& primary_key_list,
  std::vector<Row>& row_keys_value) {

  auto ret_val{ false };

  auto sql { BuildModifiedRowsQuery(table, table+kTempTableSubFix, primary_key_list) };

  if(!sql.empty())
  {
    auto stmt { m_sqlite_factory->CreateStatement(m_sqlite_connection, sql) };

    while (SQLITE_ROW == stmt->Step()) {
      const auto table_fields { m_table_fields[table] };
      Row register_fields;
      int32_t index {0l};
      for(const auto& pk_value : primary_key_list) {
        const auto it = std::find_if(table_fields.begin(), table_fields.end(), [&pk_value] (const ColumnData& cd) {
          return std::get<TableHeader::NAME>(cd).compare(pk_value) == 0;
        });
        if (table_fields.end() != it) {
          GetTableData(
                      stmt, 
                      index, 
                      std::get<TableHeader::TYPE>(*it),
                      "PK_" + std::get<TableHeader::NAME>(*it), 
                      register_fields);
        }
        
        ++index;
      }
      for(const auto& field : table_fields) {
        if (register_fields.end() == register_fields.find(std::get<TableHeader::NAME>(field))){
          if (!stmt->GetColumn(index)->IsNullValue()) {
            GetTableData(stmt, index, std::get<TableHeader::TYPE>(field), std::get<TableHeader::NAME>(field), register_fields);
          }
        }
        ++index;
      }
      row_keys_value.push_back(std::move(register_fields));
    }
    ret_val = true;
  }
  return ret_val;
}

bool SQLiteDBEngine::UpdateRows(
  const std::string& table, 
  const std::vector<std::string>& primary_key_list,
  std::vector<Row>& row_keys_value) {

  auto transaction { m_sqlite_factory->CreateTransaction(m_sqlite_connection)};

  for (const auto& row : row_keys_value) {
    for (const auto& field : row) {
      if (0 != field.first.substr(0,3).compare("PK_"))
      {
        const auto sql { BuildUpdateDataSqlQuery(
          table, 
          primary_key_list,
          row,
          field) };
        
        if (!m_sqlite_connection->Execute(sql)) {
          std::cout << "error" << std::endl;
        }
      }
    }
  }
  return transaction->Commit();
}

bool SQLiteDBEngine::GetFieldValueFromTuple(
  const std::pair<const std::__cxx11::string, TableField> &value,
  nlohmann::json& object) {
  auto ret_val { true };

  const auto row_type { std::get<GenericTupleIndex::GEN_TYPE>(value.second) };
  if (ColumnType::BIGINT_TYPE == row_type) {
    object[value.first] = std::get<ColumnType::BIGINT_TYPE>(value.second);
  } else if (ColumnType::UNSIGNED_BIGINT_TYPE == row_type) {
    object[value.first] = std::get<ColumnType::UNSIGNED_BIGINT_TYPE>(value.second);
  } else if (ColumnType::INTEGER_TYPE == row_type) {
    object[value.first] = std::get<ColumnType::INTEGER_TYPE>(value.second);
  } else if (ColumnType::TEXT_TYPE == row_type) {
    object[value.first] = std::get<ColumnType::TEXT_TYPE>(value.second);
  } else if (ColumnType::DOUBLE_TYPE == row_type) {
    object[value.first] = std::get<ColumnType::DOUBLE_TYPE>(value.second);
  } else {
    std::cout << "not implemented "<< __LINE__ << " - " << __FILE__ << std::endl;
    ret_val = false;
  }

  return ret_val;
}

bool SQLiteDBEngine::GetFieldValueFromTuple(
  const std::pair<const std::__cxx11::string, TableField> &value,
  std::string& result_value,
  const bool quotation_marks) {
  auto ret_val { true };
  const auto row_type { std::get<GenericTupleIndex::GEN_TYPE>(value.second) };
  if (ColumnType::BIGINT_TYPE == row_type) {
    result_value.append(std::to_string(std::get<ColumnType::BIGINT_TYPE>(value.second)));
  } else if (ColumnType::UNSIGNED_BIGINT_TYPE == row_type) {
    result_value.append(std::to_string(std::get<ColumnType::UNSIGNED_BIGINT_TYPE>(value.second)));
  } else if (ColumnType::INTEGER_TYPE == row_type) {
    result_value.append(std::to_string(std::get<ColumnType::INTEGER_TYPE>(value.second)));
  } else if (ColumnType::TEXT_TYPE == row_type) {
    if(quotation_marks) {
      result_value.append("'"+std::get<ColumnType::TEXT_TYPE>(value.second)+"'");
    }
    else {
      result_value.append(std::get<ColumnType::TEXT_TYPE>(value.second));
    }
  } else if (ColumnType::DOUBLE_TYPE == row_type) {
    result_value.append(std::to_string(std::get<ColumnType::DOUBLE_TYPE>(value.second)));
  } else {
    ret_val = false;
  }
  return ret_val;
}
